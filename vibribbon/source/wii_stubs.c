/* Host-side hooks the PSX runtime expects from its desktop frontend: debug
 * server, netplay, tracing and presentation counters. None of them do
 * anything on the Wii. */
#include <stdint.h>
#include <string.h>

#include "cpu_state.h"
#include "psx_bios_backend.h"

/* ---- BIOS registry: OpenBIOS only (MIT licensed, shipped beside the app) ---- */
extern const PsxBiosBackend OpenBIOS_psx_bios_backend;
const PsxBiosBackend *const psx_bios_registry[] = { &OpenBIOS_psx_bios_backend };
const uint32_t psx_bios_registry_count = 1u;

/* ---- globals normally owned by main.cpp / debug_server.c ---- */
uint32_t g_debug_last_store_pc;
uint32_t g_debug_current_func_addr;
uint64_t s_frame_count;
int      g_ws_bd_stretch_on = 1;
int      g_ws_bd_stretch_pct;
CPUState *debug_cpu_ptr;
int      g_ram_read_watch_active;
uint32_t g_psx_recursion_func;
uint32_t g_psx_recent_fn_i;
uint32_t g_psx_recent_fn[64];
int      g_present_vsync_disabled;
uint32_t g_present_slow_count;

/* ---- debug server ---- */
void debug_server_poll(void) {}
void debug_server_trace_write_check(uint32_t phys, uint32_t old_val, uint32_t new_val, uint8_t width)
{ (void)phys; (void)old_val; (void)new_val; (void)width; }
void debug_server_trace_mmio_write(uint32_t a, uint32_t v, uint8_t w) { (void)a; (void)v; (void)w; }
void debug_server_trace_mmio_read(uint32_t a, uint32_t v, uint8_t w) { (void)a; (void)v; (void)w; }
void debug_server_trace_ram_read_watch(uint32_t p, uint32_t v) { (void)p; (void)v; }
void debug_server_log_sio_write(uint32_t a, uint32_t v, uint8_t w) { (void)a; (void)v; (void)w; }
void debug_server_log_thread_event(uint32_t kind, CPUState *cpu, uint32_t cur, uint32_t tgt, uint32_t pc)
{ (void)kind; (void)cpu; (void)cur; (void)tgt; (void)pc; }
uint64_t debug_server_get_tcp_stall_ms(void) { return 0; }
uint32_t debug_server_get_tcp_drops(void) { return 0; }
void debug_server_get_status(int *listening, int *port, int *error)
{ if (listening) *listening = 0; if (port) *port = 0; if (error) *error = 0; }
uint32_t debug_guest_sp(void) { return debug_cpu_ptr ? debug_cpu_ptr->gpr[29] : 0; }
uint32_t debug_guest_ra(void) { return debug_cpu_ptr ? debug_cpu_ptr->gpr[31] : 0; }

typedef struct {
    uint64_t seq;
    uint32_t addr, phys, ra, a0, a1, frame, pad;
} UnknownDispatchEntry;
UnknownDispatchEntry crash_trace_unknown_get(uint64_t seq)
{ UnknownDispatchEntry e; memset(&e, 0, sizeof(e)); (void)seq; return e; }
uint64_t crash_trace_unknown_seq_get(void) { return 0; }
uint32_t crash_trace_dispatch_ring_get(int idx) { (void)idx; return 0; }
uint64_t crash_trace_dispatch_seq_get(void) { return 0; }

int stack_profile_json(char *out, int cap) { if (out && cap > 0) out[0] = 0; return 0; }
int ce_profile_json(char *out, int cap) { if (out && cap > 0) out[0] = 0; return 0; }

/* freeze-dump JSON producers (debug_server.c) */
#define FREEZE_STUB(name) int name(char *out, int cap) { if (out && cap > 0) out[0] = 0; return 0; }
FREEZE_STUB(debug_server_freeze_dump_wtrace_json)
FREEZE_STUB(debug_server_freeze_dump_wtrace_all_json)
FREEZE_STUB(debug_server_freeze_dump_thread_trace_json)
FREEZE_STUB(debug_server_freeze_dump_sio_pc_json)
FREEZE_STUB(debug_server_freeze_dump_restore_trace_json)
FREEZE_STUB(debug_server_freeze_dump_mdec_trace_json)
FREEZE_STUB(debug_server_freeze_dump_mdec_state_json)
FREEZE_STUB(debug_server_freeze_dump_irq_trace_json)
FREEZE_STUB(debug_server_freeze_dump_frame_history_json)
FREEZE_STUB(debug_server_freeze_dump_fn_entry_json)
FREEZE_STUB(debug_server_freeze_dump_dma_trace_json)
FREEZE_STUB(debug_server_freeze_dump_dma_state_json)
FREEZE_STUB(debug_server_freeze_dump_dirty_block_json)

/* ---- netplay: permanently offline ---- */
int      psx_netplay_active(void) { return 0; }
int      psx_netplay_is_resimulating(void) { return 0; }
int      psx_netplay_cd_bisect_active(void) { return 0; }
uint32_t psx_netplay_sim_tick(void) { return 0; }
uint32_t psx_netplay_rb_sticky_bb_pc(void) { return 0; }
int      psx_netplay_rb_recover_null_pc(struct CPUState *cpu, uint32_t *out_pc)
{ (void)cpu; (void)out_pc; return 0; }

/* ---- savestate frontend notifications ---- */
void psx_frontend_on_savestate_notify(int is_load, int slot, int ok)
{ (void)is_load; (void)slot; (void)ok; }
void psx_frontend_on_savestate_loaded(void) {}

/* ---- text translation / disc patches / mods: unused ---- */
int psx_mod_set_load_acceleration(uint32_t mult, uint32_t a, uint32_t b) { (void)mult; (void)a; (void)b; return 0; }
int psx_mod_set_disc_speed(uint32_t divisor, uint32_t a) { (void)divisor; (void)a; return 0; }

/* 32-bit PPC has no 8-byte atomics; the runtime uses them for counters read
 * from the same thread, so plain accesses under an interrupt lock are enough. */
#include <ogc/machine/processor.h>
uint64_t __atomic_load_8(const volatile void *p, int mo)
{ (void)mo; uint32_t l; _CPU_ISR_Disable(l); uint64_t v = *(const volatile uint64_t *)p; _CPU_ISR_Restore(l); return v; }
void __atomic_store_8(volatile void *p, uint64_t v, int mo)
{ (void)mo; uint32_t l; _CPU_ISR_Disable(l); *(volatile uint64_t *)p = v; _CPU_ISR_Restore(l); }

uint64_t __atomic_fetch_add_8(volatile void *p, uint64_t v, int mo)
{ (void)mo; uint32_t l; _CPU_ISR_Disable(l); uint64_t o = *(volatile uint64_t *)p; *(volatile uint64_t *)p = o + v; _CPU_ISR_Restore(l); return o; }

/* ---- more debug/trace/netplay/overlay hooks ---- */
uint64_t g_dispatch_static_hits;
volatile uint32_t g_psx_last_fn_entry;
void psx_unknown_dispatch_record(uint32_t addr, uint32_t phys, uint32_t ra, uint32_t a0, uint32_t a1)
{ (void)addr; (void)phys; (void)ra; (void)a0; (void)a1; }
void debug_server_wait_if_paused(void) {}
void debug_server_trace_dispatch(uint32_t f) { (void)f; }
void debug_server_log_restore_event(uint32_t k, uint32_t pc, uint32_t v) { (void)k; (void)pc; (void)v; }
void psx_netplay_poll_snap(struct CPUState *cpu, uint32_t pc) { (void)cpu; (void)pc; }
void psx_frontend_on_rb_snap_loaded(void) {}
uint32_t netplay_spad_digest(void) { return 0; }
uint32_t netplay_dma_digest(void) { return 0; }
uint32_t netplay_cdrom_digest(void) { return 0; }
uint32_t netplay_av_digest(void) { return 0; }
uint32_t netplay_aux_digest(void) { return 0; }
void netplay_sio_digest_parts(void *out) { (void)out; }
void netplay_core_digest_parts(const CPUState *cpu, void *out) { (void)cpu; (void)out; }

/* Native overlay libraries are a desktop (dlopen) feature. Everything the game
 * runs here is recompiled ahead of time. */
void *psx_overlay_posix_library_open(const char *path, char *error, size_t cap)
{ (void)path; if (error && cap) error[0] = 0; return 0; }
void *psx_overlay_posix_library_symbol(void *h, const char *n) { (void)h; (void)n; return 0; }
void *psx_overlay_posix_library_entry(void *h, uint32_t e) { (void)h; (void)e; return 0; }
void psx_overlay_posix_library_close(void *h) { (void)h; }

/* Host fibers back only the legacy LLE scheduler; the HLE TCB scheduler is the
 * default and is what runs here. */
#include <stdlib.h>
typedef void *psx_fiber_t;
psx_fiber_t psx_fiber_current(void) { abort(); }
void psx_fiber_switch(psx_fiber_t t) { (void)t; abort(); }
psx_fiber_t psx_fiber_create(size_t sz, void (*entry)(void *), void *arg) { (void)sz; (void)entry; (void)arg; abort(); }
void psx_fiber_destroy(psx_fiber_t f) { (void)f; }
psx_fiber_t psx_fiber_convert_thread(void) { abort(); }
