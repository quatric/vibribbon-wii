/* VSync wait skip.
 *
 * Every program on the disc waits for vblank in PsyQ's libetc v_wait():
 *
 *   loop: lw v0,16(sp); addiu v0,-1; sw v0,16(sp); lw v0,16(sp)
 *         bne v0,v1(-1),poll          -- timeout counter on the stack
 *         ... "VSync: timeout" ...
 *   poll: lui/lw v0,Vcount; slt v0,v0,a0; bne v0,zero,loop
 *
 * Nothing but the vblank interrupt handler changes Vcount, so the loop is
 * pure waiting, and it is most of each frame. The runtime's generic idle
 * detector can't take it: the loop stores its countdown to the stack and
 * checks interrupts at two PCs per iteration.
 *
 * Once several iterations have run with the same cycle cost and the expected
 * effects (the countdown down by one, one store, no MMIO, registers unchanged),
 * whole iterations are skipped up to just before the next event the guest
 * could observe: guest time advances by k iterations' cycles (devices replay
 * their events exactly) and the countdown drops by k. The next real iteration
 * then takes the interrupt at the same check, on the same cycle, as running
 * every iteration would. */
#include <stdint.h>
#include <string.h>
#include "cpu_state.h"
#include "psx_cycles.h"

extern uint64_t g_guest_store_count, g_mmio_access_count;
extern uint32_t psx_idle_cycles_to_next_observable_event(void);
extern uint8_t *memory_get_ram_ptr(void);
extern int psx_get_in_exception(void);
extern void psx_poll_probe_begin(void);
extern int psx_poll_probe_settled(void);
extern uint32_t psx_read_word(uint32_t addr);
extern uint8_t psx_read_byte(uint32_t addr);
extern void psx_write_word(uint32_t addr, uint32_t val);
extern void psx_write_byte(uint32_t addr, uint8_t val);
extern int psx_ram_bulk_write_ok_noted(uint32_t phys, uint32_t len);
extern void psx_ram_bulk_note_text(uint32_t phys, uint32_t len);

#define STREAK_MIN 3
#define SKIP_MAX   1200000u            /* more than one vblank interval */

static const uint32_t s_head[8] = {
    0x8FA20010, 0x00000000, 0x2442FFFF, 0xAFA20010,
    0x8FA20010, 0x00000000, 0x1443000B, 0x00000000,
};

static uint32_t s_pc;                  /* verified loop head, 0 if none */
static uint64_t s_cycle, s_stores, s_mmio;
static uint32_t s_t, s_sp, s_a0, s_q, s_streak;
static int s_valid;                    /* the snapshot above is from the previous iteration */

uint64_t g_vsync_skip_count, g_vsync_skip_cycles;

static inline uint32_t ram_word(const uint8_t *ram, uint32_t addr)
{
    uint32_t w;
    memcpy(&w, ram + (addr & 0x1FFFFCu), 4);
    return __builtin_bswap32(w);                       /* lwbrx */
}

static inline void ram_store(uint8_t *ram, uint32_t addr, uint32_t v)
{
    v = __builtin_bswap32(v);
    memcpy(ram + (addr & 0x1FFFFCu), &v, 4);
}

static int is_v_wait_loop(const uint8_t *ram, uint32_t pc)
{
    for (int i = 0; i < 8; i++)
        if (ram_word(ram, pc + 4u * i) != s_head[i]) return 0;
    /* poll block: lui v0,hi; lw v0,lo(v0); nop; slt v0,v0,a0; bne v0,zero,loop; nop */
    return (ram_word(ram, pc + 0x48) >> 16) == 0x3C02 &&
           (ram_word(ram, pc + 0x4C) >> 16) == 0x8C42 &&
           ram_word(ram, pc + 0x50) == 0x00000000 &&
           ram_word(ram, pc + 0x54) == 0x0044102A &&
           ram_word(ram, pc + 0x58) == 0x1440FFE9 &&
           ram_word(ram, pc + 0x5C) == 0x00000000;
}

/* Poll loops, such as the title program's wait for a CD read (lw/beq on
 * flags its interrupt handler sets), the TestEvent() loops of the game and of
 * the BIOS's CD reads, or libcd's CdReady() polled from the game's loading
 * loops: between two visits to the same check PC, with calls and returns in
 * between, every register the same at both, and nothing done that a later
 * iteration could see. The next iteration then repeats this one exactly
 * until an interrupt handler or DMA changes memory, which only happens when
 * devices are serviced: whole iterations are skipped up to just before the
 * next device deadline.
 *
 * A loop with no store and no MMIO access qualifies as it is. One that stores
 * or reads device registers (VSync(-1) reads the timers, CdReady() the CD
 * status) is run once more with the runtime probing it: it qualifies when
 * every store wrote what memory already held (or went to the stack below the
 * loop's own frame), every device access was a side-effect-free one, and no
 * device deadline came round meanwhile. */
static uint32_t s_ppc, s_pq, s_pds, s_pdm, s_pstreak, s_pcool, s_pvisits, s_pmiss;
static int s_psnap;                    /* s_pgpr holds the registers at s_ppc */
static uint64_t s_pcycle, s_pstores, s_pmmio, s_pdeadline;
static uint32_t s_pgpr[32], s_phi, s_plo;

int g_poll_store_probe, g_poll_store_dirty;
uint32_t g_poll_dead_lo = 0xFFFFFFFFu;
uint64_t g_poll_skip_count, g_poll_skip_cycles, g_cdsync_skip_count, g_cdsync_skip_cycles;

static inline void probe_off(void)
{
    g_poll_store_probe = 0;
    g_poll_dead_lo = 0xFFFFFFFFu;
}

static void poll_reset(uint32_t check_pc)
{
    probe_off();
    s_ppc = check_pc;
    s_psnap = 0; s_pstreak = 0; s_pvisits = 0; s_pcool = 0; s_pmiss = 0;
}

static void poll_snap(CPUState *cpu)
{
    memcpy(&s_pgpr[1], &cpu->gpr[1], 31 * 4);
    s_phi = cpu->hi; s_plo = cpu->lo;
    s_pcycle = psx_get_cycle_count();
    s_pstores = g_guest_store_count;
    s_pmmio = g_mmio_access_count;
    s_psnap = 1; s_pstreak = 0;
}

static void poll_loop_check(CPUState *cpu, uint32_t check_pc)
{
    if (s_pcool) {                         /* a loop that changes registers: look again later */
        if (--s_pcool == 0) poll_reset(check_pc);
        return;
    }
    if (s_ppc == 0) {                      /* moved on from a PC visited with other registers */
        s_ppc = check_pc;
        s_pvisits = 0;
        if (psx_get_in_exception() == 0) poll_snap(cpu);
        return;
    }
    if (check_pc != s_ppc) {               /* inside the iteration */
        if (++s_pvisits > 64) poll_reset(check_pc);
        return;
    }
    s_pvisits = 0;
    int probed = g_poll_store_probe, dirty = g_poll_store_dirty;
    if (probed) {
        probe_off();
        if (!dirty && !psx_poll_probe_settled()) dirty = 1;
    }
    if (psx_get_in_exception() != 0) { poll_reset(check_pc); return; }
    if (!s_psnap) { poll_snap(cpu); return; }
    uint64_t cyc = psx_get_cycle_count(), last = s_pcycle;
    uint32_t ds = (uint32_t)(g_guest_store_count - s_pstores);
    uint32_t dm = (uint32_t)(g_mmio_access_count - s_pmmio);
    s_pcycle = cyc;
    s_pstores = g_guest_store_count;
    s_pmmio = g_mmio_access_count;
    if (cpu->hi != s_phi || cpu->lo != s_plo || memcmp(&cpu->gpr[1], &s_pgpr[1], 31 * 4) != 0) {
        /* A PC can come round more than once an iteration (a call made
         * twice with other arguments): try the next one, a few times. */
        s_psnap = 0; s_pstreak = 0;
        if (++s_pmiss >= 16) s_pcool = 256;
        else s_ppc = 0;
        return;
    }
    uint32_t q = (uint32_t)(cyc - last);
    if (q == 0 || q >= 65536u) s_pstreak = 0;
    else s_pstreak = q == s_pq && ds == s_pds && dm == s_pdm ? s_pstreak + 1 : 1;
    s_pq = q; s_pds = ds; s_pdm = dm;
    if (s_pstreak < STREAK_MIN) return;
    if (psx_in_device_service || psx_next_service_cycle <= cyc) return;
    if (ds | dm) {
        if (!probed || dirty || cyc >= s_pdeadline) {   /* devices ran: probe again */
            g_poll_store_dirty = 0;
            psx_poll_probe_begin();
            g_poll_dead_lo = (cpu->gpr[29] & 0x1FFFFFu) - 0x1000u;
            g_poll_store_probe = 1;
            s_pdeadline = psx_next_service_cycle;
            return;
        }
    }

    /* Stop short of the next device deadline rather than the next event:
     * devices then catch up at the same cycles as they would have, which the
     * runtime's device timing depends on, and nothing can happen before it. */
    uint32_t k = (uint32_t)((psx_next_service_cycle - cyc - 1u) / q);
    if (k == 0) return;
    g_poll_skip_count++;
    g_poll_skip_cycles += (uint64_t)k * q;
    psx_advance_cycles(k * q);
    s_pcycle = psx_get_cycle_count();
}

/* libcd CdSync(0) waits for a CD command to complete in the resident boot
 * program:
 *
 *   loop: VSync(-1); timeout check; if (sync state >= 0) { VSync(-1); check }
 *         s0 = sync state; until done
 *
 * VSync(-1) reads the vblank counter (and the hblank counter, which reading
 * doesn't change) and spills to its own stack frame, so every iteration is the
 * same calls with the same stores. Only the CD and vblank interrupt handlers
 * change what it reads. Iterations that check in at the loop head with
 * identical registers, cycle cost, store and MMIO counts are skipped up to
 * just before the next event the guest could observe, as for v_wait. */
#define CDSYNC_LOOP  0x80011830u
#define CDSYNC_STATE 0x80016344u        /* s1; the state word is at s1 - 8 */
#define VSYNC_FN     0x80012B84u
#define VCOUNT       0x8001646Cu

static const uint32_t s_cdsync_code[36] = {
    0x0C004AE1, 0x2404FFFF, 0x8E230000, 0x00000000, 0x246304B0, 0x0062182A, 0x14600013, 0x2410FFFF,
    0x8E22FFF8, 0x00000000, 0x04400009, 0x00000000, 0x0C004AE1, 0x2404FFFF, 0x8E23FFFC, 0x00000000,
    0x2463003C, 0x0062182A, 0x10600006, 0x00000000, 0x0C00451F, 0x24040001, 0x8E30FFE4, 0x08004626,
    0x00000000, 0x8E30FFF8, 0x16600009, 0x24040001, 0x8E420000, 0x00000000, 0x10400003, 0x00000000,
    0x1200FFDF, 0x00000000, 0x1E00FFDD, 0x24040001,
};
static const uint32_t s_vsync_code[28] = {
    0x3C028001, 0x8C426434, 0x3C058001, 0x8CA56438, 0x27BDFFD8, 0xAFBF0020, 0xAFB1001C, 0xAFB00018,
    0x8C500000, 0x8CA20000, 0x00000000, 0xAFA20010, 0x8FA30010, 0x8CA20000, 0x00000000, 0x1462FFF9,
    0x00000000, 0x8FA20010, 0x3C038001, 0x8C63643C, 0x00000000, 0x00431023, 0x04810005, 0x3051FFFF,
    0x3C028001, 0x8C42646C, 0x08004B3A, 0x00000000,
};
static const uint32_t s_vsync_tail[6] = {
    0x02201021, 0x8FBF0020, 0x8FB1001C, 0x8FB00018, 0x03E00008, 0x27BD0028,
};

static int s_cd_valid, s_cd_code;      /* s_cd_code: 1 code verified, -1 not this game */
static uint32_t s_cd_q, s_cd_ds, s_cd_dm, s_cd_streak;
static uint64_t s_cd_cycle, s_cd_stores, s_cd_mmio;
static uint32_t s_cd_gpr[32], s_cd_hi, s_cd_lo;

static int code_matches(const uint8_t *ram, uint32_t at, const uint32_t *w, int n)
{
    for (int i = 0; i < n; i++)
        if (ram_word(ram, at + 4u * i) != w[i]) return 0;
    return 1;
}

static void cdsync_check(CPUState *cpu, const uint8_t *ram)
{
    if (s_cd_code == 0)
        s_cd_code = code_matches(ram, CDSYNC_LOOP, s_cdsync_code, 36) &&
                    code_matches(ram, VSYNC_FN, s_vsync_code, 28) &&
                    code_matches(ram, 0x80012CE4u, s_vsync_tail, 6) ? 1 : -1;
    if (s_cd_code < 0) return;

    uint64_t cyc = psx_get_cycle_count();
    uint32_t q = (uint32_t)(cyc - s_cd_cycle);
    uint32_t ds = (uint32_t)(g_guest_store_count - s_cd_stores);
    uint32_t dm = (uint32_t)(g_mmio_access_count - s_cd_mmio);
    int ok = s_cd_valid && psx_get_in_exception() == 0 &&
             cpu->hi == s_cd_hi && cpu->lo == s_cd_lo &&
             memcmp(&cpu->gpr[1], &s_cd_gpr[1], 31 * 4) == 0 &&
             q > 0 && q < 65536u;
    s_cd_streak = !ok ? 0 : (q == s_cd_q && ds == s_cd_ds && dm == s_cd_dm ? s_cd_streak + 1 : 1);
    s_cd_q = q; s_cd_ds = ds; s_cd_dm = dm;
    s_cd_valid = 1;
    s_cd_cycle = cyc;
    s_cd_stores = g_guest_store_count;
    s_cd_mmio = g_mmio_access_count;
    if (!ok) {
        memcpy(&s_cd_gpr[1], &cpu->gpr[1], 31 * 4);
        s_cd_hi = cpu->hi; s_cd_lo = cpu->lo;
    }
    if (s_cd_streak < STREAK_MIN) return;

    /* Mode 0 on the libcd state, both VSync calls taken and neither timeout
     * reached: the iteration is VSync, VSync and the loads, nothing else.
     * (Re-check the code: overlays never load below the boot program's end,
     * but stay safe.) */
    uint32_t s1 = cpu->gpr[17], vc = ram_word(ram, VCOUNT);
    if (s1 != CDSYNC_STATE || cpu->gpr[18] != s1 + 8u || cpu->gpr[19] != 0) return;
    if ((int32_t)ram_word(ram, s1 - 8u) < 0) return;
    if ((int32_t)(ram_word(ram, s1) + 1200u) < (int32_t)vc ||
        (int32_t)(ram_word(ram, s1 - 4u) + 60u) < (int32_t)vc) return;
    if (ram_word(ram, CDSYNC_LOOP) != s_cdsync_code[0] || ram_word(ram, VSYNC_FN + 0x2C) != s_vsync_code[11])
        return;

    psx_devices_mmio_sync();
    uint32_t dist = psx_idle_cycles_to_next_observable_event();
    if (dist <= q) return;
    uint32_t k = (dist - 1u) / q;
    if ((uint64_t)k * q > SKIP_MAX) k = SKIP_MAX / q;
    if (k == 0) return;
    g_cdsync_skip_count++;
    g_cdsync_skip_cycles += (uint64_t)k * q;
    psx_advance_cycles(k * q);
    s_cd_cycle = psx_get_cycle_count();
    s_cd_stores = g_guest_store_count;
    s_cd_mmio = g_mmio_access_count;
}

/* The SCPH-5552 BIOS's boot loops, which are most of the first second:
 *
 *   copy:  lbu t6,0(a1); addiu a2,-1; addiu a1,1; addiu a0,1; bgtz a2; sb t6,-1(a0)
 *   clear: sw zero,0(t1); addi t1,4; bne t1,t2                (0xA0010000 up to sp)
 *   delay: a stack counter at 28(sp) counted up to 50000 through loads and stores
 *
 * They run from ROM, so their code cannot change. Once a loop has come round
 * to its head several times with the same cycle cost and its counters moved by
 * one iteration each time, whole iterations are applied at once up to just
 * before the next device deadline, as the poll-loop skip does, and the last
 * iteration is left to run. */
enum { BL_COPY, BL_CLEAR, BL_DELAY, BL_N };
static const uint32_t s_bl_head[BL_N] = { 0x1FC02B68u, 0x1FC0D864u, 0x1FC073CCu };
static const uint32_t s_bl_code[BL_N][3] = {
    { 0x90AE0000u, 0x24C6FFFFu, 0x24A50001u },
    { 0xAD200000u, 0x21290004u, 0x152AFFFDu },
    { 0x00000000u, 0xAFAF001Cu, 0x8FB8001Cu },
};
static int s_bl_rom;                    /* 1 the ROM holds these loops, -1 it does not */
static struct { uint64_t cycle; uint32_t q, streak, v[3]; } s_bl[BL_N];
uint64_t g_bios_loop_skip_count, g_bios_loop_skip_cycles;

static int bios_loop_check(CPUState *cpu, uint32_t phys)
{
    int l = 0;
    while (l < BL_N && s_bl_head[l] != phys) l++;
    if (l == BL_N) return 0;
    if (s_bl_rom == 0) {
        s_bl_rom = 1;
        for (int i = 0; i < BL_N; i++)
            for (int j = 0; j < 3; j++)
                if (psx_read_word(0xA0000000u | (s_bl_head[i] + 4u * j)) != s_bl_code[i][j]) s_bl_rom = -1;
    }
    if (s_bl_rom < 0) return 0;

    const uint32_t *g = cpu->gpr;
    uint32_t v[3], remaining;
    int moved;
    switch (l) {
    case BL_COPY:        /* a0 dst, a1 src, a2 bytes left */
        v[0] = g[4]; v[1] = g[5]; v[2] = g[6];
        moved = v[0] == s_bl[l].v[0] + 1u && v[1] == s_bl[l].v[1] + 1u && v[2] == s_bl[l].v[2] - 1u;
        remaining = (int32_t)v[2] > 0 ? v[2] : 0u;
        break;
    case BL_CLEAR:       /* t1 next word, t2 end */
        v[0] = g[9]; v[1] = g[10]; v[2] = 0;
        moved = v[0] == s_bl[l].v[0] + 4u && v[1] == s_bl[l].v[1];
        remaining = v[1] > v[0] && !((v[1] - v[0]) & 3u) ? (v[1] - v[0]) / 4u : 0u;
        break;
    default:             /* the count, sp, t7 (the count loaded in the branch delay slot) */
        v[0] = psx_read_word(g[29] + 28u); v[1] = g[29]; v[2] = g[15];
        moved = v[0] == s_bl[l].v[0] + 1u && v[1] == s_bl[l].v[1] && v[2] == v[0] && g[2] == 50000u;
        remaining = v[0] < 50000u ? 50000u - v[0] : 0u;
        break;
    }
    uint64_t cyc = psx_get_cycle_count();
    uint32_t q = (uint32_t)(cyc - s_bl[l].cycle);
    int ok = moved && psx_get_in_exception() == 0 && q > 0 && q < 4096u;
    s_bl[l].streak = !ok ? 0 : (q == s_bl[l].q ? s_bl[l].streak + 1 : 1);
    s_bl[l].q = q;
    s_bl[l].cycle = cyc;
    memcpy(s_bl[l].v, v, sizeof(v));
    if (s_bl[l].streak < STREAK_MIN) return 1;
    if (psx_in_device_service || psx_next_service_cycle <= cyc || remaining < 2u) return 1;
    uint32_t k = (uint32_t)((psx_next_service_cycle - cyc - 1u) / q);
    if (k > remaining - 1u) k = remaining - 1u;
    if (k == 0) return 1;
    /* Memory only: a device register would be read or written at other times. */
    if (l == BL_COPY) {
        uint32_t d = v[0] & 0x1FFFFFFFu, src = v[1] & 0x1FFFFFFFu;
        if (d >= 0x00800000u || k > 0x00800000u - d) return 1;
        if (!((src < 0x00800000u && k <= 0x00800000u - src) ||
              (src >= 0x1FC00000u && src < 0x1FC80000u && k <= 0x1FC80000u - src))) return 1;
    } else if (l == BL_CLEAR) {
        uint32_t d = v[0] & 0x1FFFFFFFu;
        if (d >= 0x00200000u || k > (0x00200000u - d) / 4u) return 1;
    }

    switch (l) {
    case BL_COPY: {
        uint8_t b = 0;
        for (uint32_t i = 0; i < k; i++) {          /* in order: the ranges may overlap */
            b = psx_read_byte(v[1] + i);
            psx_write_byte(v[0] + i, b);
        }
        cpu->gpr[14] = b;
        cpu->gpr[4] = v[0] + k; cpu->gpr[5] = v[1] + k; cpu->gpr[6] = v[2] - k;
        s_bl[l].v[0] += k; s_bl[l].v[1] += k; s_bl[l].v[2] -= k;
        break;
    }
    case BL_CLEAR: {
        uint32_t phys0 = v[0] & 0x1FFFFFFFu;
        if (psx_ram_bulk_write_ok_noted(phys0, 4u * k)) {
            memset(memory_get_ram_ptr() + phys0, 0, 4u * k);
            psx_ram_bulk_note_text(phys0, 4u * k);
            g_guest_store_count += k;
        } else {
            for (uint32_t i = 0; i < k; i++) psx_write_word(v[0] + 4u * i, 0);
        }
        cpu->gpr[9] = v[0] + 4u * k;
        s_bl[l].v[0] += 4u * k;
        break;
    }
    default: {
        uint32_t c = v[0] + k;
        psx_write_word(v[1] + 28u, c);
        cpu->gpr[15] = c; cpu->gpr[24] = c - 1u; cpu->gpr[25] = c; cpu->gpr[8] = c;
        s_bl[l].v[0] = c; s_bl[l].v[2] = c;
        break;
    }
    }
    g_bios_loop_skip_count++;
    g_bios_loop_skip_cycles += (uint64_t)k * q;
    psx_advance_cycles(k * q);
    s_bl[l].cycle = psx_get_cycle_count();
    return 1;
}

void __wrap_psx_idle_note_check(CPUState *cpu, uint32_t check_pc)
{
    const uint8_t *ram = memory_get_ram_ptr();
    if ((check_pc & 0x1FE00000u) != 0) {                  /* RAM, or the BIOS ROM's own waits */
        if ((check_pc & 0x1FF80000u) == 0x1FC00000u && !bios_loop_check(cpu, check_pc & 0x1FFFFFFFu))
            poll_loop_check(cpu, check_pc);
        return;
    }
    if (check_pc == CDSYNC_LOOP) {
        cdsync_check(cpu, ram);
        return;
    }
    if (check_pc != s_pc) {
        if (check_pc == s_pc + 0x48) return;              /* the poll check of the same loop */
        if (ram_word(ram, check_pc) != 0x8FA20010 || !is_v_wait_loop(ram, check_pc)) {
            poll_loop_check(cpu, check_pc);
            return;
        }
        s_pc = check_pc;
        s_valid = 0;
    } else if (ram_word(ram, check_pc + 0x0C) != 0xAFA20010 ||
               ram_word(ram, check_pc + 0x58) != 0x1440FFE9) {
        s_pc = 0;                                         /* code changed under us */
        return;
    }

    uint32_t sp = cpu->gpr[29], a0 = cpu->gpr[4];
    uint32_t t = ram_word(ram, sp + 16);
    uint64_t cyc = psx_get_cycle_count();
    uint32_t q = (uint32_t)(cyc - s_cycle);
    int ok = s_valid && psx_get_in_exception() == 0 &&
             sp == s_sp && a0 == s_a0 && cpu->gpr[3] == 0xFFFFFFFFu && cpu->gpr[2] == 1u &&
             t == s_t - 1u && g_guest_store_count == s_stores + 1u && g_mmio_access_count == s_mmio &&
             q > 0 && q < 4096u;
    s_streak = !ok ? 0 : (q == s_q ? s_streak + 1 : 1);
    s_q = ok ? q : 0;
    s_valid = 1;
    s_cycle = cyc; s_t = t; s_sp = sp; s_a0 = a0;
    s_stores = g_guest_store_count; s_mmio = g_mmio_access_count;
    if (s_streak < STREAK_MIN) return;

    /* Devices lag the CPU until their next deadline, and the distances below
     * count from where they stand: catch them up so the skip counts from now. */
    psx_devices_mmio_sync();
    uint32_t dist = psx_idle_cycles_to_next_observable_event();
    if (dist <= q) return;
    uint32_t k = (dist - 1u) / q;                         /* end strictly before the event */
    if ((int32_t)t <= 1) return;
    if (k > t - 1u) k = t - 1u;                           /* the timeout path stays reachable */
    if ((uint64_t)k * q > SKIP_MAX) k = SKIP_MAX / q;
    if (k == 0) return;

    ram_store(memory_get_ram_ptr(), sp + 16, t - k);
    g_vsync_skip_count++;
    g_vsync_skip_cycles += (uint64_t)k * q;
    psx_advance_cycles(k * q);
    /* Continue the snapshot from where the skip landed: the next real
     * iteration is checked against it like any other, so an interrupt or
     * anything else that changes the loop's behaviour ends the run. */
    s_cycle = psx_get_cycle_count();
    s_t = t - k;
}
