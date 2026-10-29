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

void __wrap_psx_idle_note_check(CPUState *cpu, uint32_t check_pc)
{
    const uint8_t *ram = memory_get_ram_ptr();
    if ((check_pc & 0x1FE00000u) != 0) return;            /* RAM only */
    if (check_pc != s_pc) {
        if (check_pc == s_pc + 0x48) return;              /* the poll check of the same loop */
        if (ram_word(ram, check_pc) != 0x8FA20010 || !is_v_wait_loop(ram, check_pc)) return;
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
