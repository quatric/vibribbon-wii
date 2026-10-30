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

/* Pure poll loops, such as the title program's wait for a CD read
 * (lw/beq on flags its interrupt handler sets) or a TestEvent() loop: between
 * two visits to the same check PC, with calls and returns in between, no
 * store and no MMIO access, and every register the same at both. The next
 * iteration then repeats this one exactly until an interrupt handler or DMA
 * changes memory, which only happens when devices are serviced: whole
 * iterations are skipped up to just before the next device deadline. */
static uint32_t s_ppc, s_pq, s_pstreak, s_pcool, s_pvisits;
static int s_psnap;                    /* s_pgpr holds the registers at s_ppc */
static uint64_t s_pcycle, s_pstores, s_pmmio;
static uint32_t s_pgpr[32], s_phi, s_plo;

uint64_t g_poll_skip_count, g_poll_skip_cycles, g_cdsync_skip_count, g_cdsync_skip_cycles;

static void poll_reset(uint32_t check_pc)
{
    s_ppc = check_pc;
    s_pstores = g_guest_store_count;
    s_pmmio = g_mmio_access_count;
    s_psnap = 0; s_pstreak = 0; s_pvisits = 0; s_pcool = 0;
}

static void poll_loop_check(CPUState *cpu, uint32_t check_pc)
{
    if (g_guest_store_count != s_pstores || g_mmio_access_count != s_pmmio) {
        poll_reset(check_pc);
        return;
    }
    if (check_pc != s_ppc) {               /* inside the iteration */
        if (++s_pvisits > 64) poll_reset(check_pc);
        return;
    }
    s_pvisits = 0;
    if (s_pcool) {                         /* a loop that changes registers: look again later */
        if (--s_pcool == 0) s_psnap = 0;
        return;
    }
    if (psx_get_in_exception() != 0) { poll_reset(check_pc); return; }
    uint64_t cyc = psx_get_cycle_count(), last = s_pcycle;
    s_pcycle = cyc;
    if (!s_psnap) {
        memcpy(&s_pgpr[1], &cpu->gpr[1], 31 * 4);
        s_phi = cpu->hi; s_plo = cpu->lo;
        s_psnap = 1; s_pstreak = 0;
        return;
    }
    if (cpu->hi != s_phi || cpu->lo != s_plo || memcmp(&cpu->gpr[1], &s_pgpr[1], 31 * 4) != 0) {
        s_psnap = 0; s_pstreak = 0; s_pcool = 256;
        return;
    }
    uint32_t q = (uint32_t)(cyc - last);
    if (q == 0 || q >= 65536u) s_pstreak = 0;
    else s_pstreak = q == s_pq ? s_pstreak + 1 : 1;
    s_pq = q;
    if (s_pstreak < STREAK_MIN) return;

    /* Stop short of the next device deadline rather than the next event:
     * devices then catch up at the same cycles as they would have, which the
     * runtime's device timing depends on, and nothing can happen before it. */
    if (psx_in_device_service || psx_next_service_cycle <= cyc) return;
    uint32_t k = (uint32_t)((psx_next_service_cycle - cyc - 1u) / q);
    if (k == 0) return;
    g_poll_skip_count++;
    g_poll_skip_cycles += (uint64_t)k * q;
    psx_advance_cycles(k * q);
    s_pcycle = psx_get_cycle_count();
    s_pstores = g_guest_store_count;
    s_pmmio = g_mmio_access_count;
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

void __wrap_psx_idle_note_check(CPUState *cpu, uint32_t check_pc)
{
    const uint8_t *ram = memory_get_ram_ptr();
    if ((check_pc & 0x1FE00000u) != 0) return;            /* RAM only */
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
