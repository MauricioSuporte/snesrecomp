/* Host-authority guest jitter buffer (snes_host_app.c starv_runway).
 *
 * A host-authority guest used to keep the room's lockstep input delay D of
 * host rows queued after its first starvation, so it ran D frames behind rows
 * it already had. It now keeps a small adaptive buffer: 1 row to start, +1 on
 * repeated starvation (max 4), -1 after a calm stretch (min 0), pinned by
 * SNES_NET_AUTH_BUFFER. Lockstep peers keep D. Real barrier code, stubbed
 * session. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* The barrier uses no SDL functions; keep this unit independent of SDL linkage. */
#define SNESRECOMP_DESKTOP_SDL_COMPAT_H
#include "netplay/snes_host_app.c"

static int auth_guest = 1, lead, admit_ok, polls;
static uint64_t clock_ms = 100000;

int snes_netplay_active(void) { return 1; }
int snes_netplay_rollback_active(void) { return 0; }
int snes_netplay_draining(void) { return 0; }
int snes_netplay_quiesced(void) { return 0; }
int snes_netplay_peer_disconnected(uint32_t timeout) { (void)timeout; return 0; }
int snes_netplay_poll_admit(void) { ++polls; return admit_ok; }
void snes_netplay_log_peer_disconnect(uint32_t timeout) { (void)timeout; }
void snes_netplay_soft_exit_to_lobby(const char *origin, int from_lobby)
{ (void)origin; (void)from_lobby; assert(!"unexpected soft exit"); }
void snes_netplay_connect_wait_reset(void) {}
int snes_netplay_return_to_lobby_requested(void) { return 0; }
const char *snes_netplay_refusal(void) { return NULL; }
uint32_t snes_netplay_sim_tick(void) { return 1000; }
void snes_host_lobby_set_runtime_error(const char *why) { (void)why; }
int snes_netplay_remote_lead(void) { return lead; }
int snes_netplay_input_delay(void) { return 7; }
int snes_netplay_state_barrier(void) { return 0; }
int snes_netplay_is_running(void) { return 1; }
const char *snes_netplay_transport_name(void) { return "ice"; }
int snes_netplay_ice_failed(void) { return 0; }
void snes_netplay_wait_report(void) {}
int snes_netplay_connect_timed_out(uint32_t timeout) { (void)timeout; return 0; }
int snes_netplay_input_desync(uint32_t *t, uint32_t *l, uint32_t *r)
{ (void)t; (void)l; (void)r; return 0; }
void snes_netplay_stage_local(uint16_t buttons) { (void)buttons; }
void snes_netplay_pump(void) {}
int snes_netplay_host_authority_guest(void) { return auth_guest; }
int snes_netplay_auth_guest_lost(void) { return 0; }
uint64_t snes_netplay_clock_ms(void) { return clock_ms; }
static uint16_t capture(void *ctx) { (void)ctx; return 0; }

static void env_set(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

static int admit(void)
{
    SnesHostBarrierHooks hooks = {0};
    int running = 1;
    hooks.capture_local_pad = capture;
    return snes_host_barrier_admit(0, &running, &hooks);
}

/* Four failed polls latch starvation (SNES_STARVATION_ENTER_DEFAULT). */
static void starve(void)
{
    int i;
    admit_ok = 0;
    lead = -1;
    for (i = 0; i < 4; ++i)
        assert(admit() == 0);
    assert(g_starv.latched);
}

/* Rows the latch needs before the guest runs again. */
static int rows_to_unlatch(void)
{
    int i, l;
    admit_ok = 1;
    for (l = -1; l <= 16; ++l) {
        lead = l;
        for (i = 0; i < 3 && g_starv.latched; ++i)
            (void)admit();
        if (!g_starv.latched)
            return l;
    }
    return 99;
}

int main(void)
{
    env_set("SNES_NET_AUTH_BUFFER", NULL);
    env_set("SNES_NET_CATCHUP_CAP", NULL);

    /* Lockstep peer: the runway is still D. */
    auth_guest = 0;
    starv_reset();
    starve();
    assert(rows_to_unlatch() == 7);

    /* Host-authority guest: one row, not D. */
    auth_guest = 1;
    starv_reset();
    assert(snes_host_auth_buffer_rows() == 1);
    starve();
    assert(rows_to_unlatch() == 1);
    /* Catch-up drains everything above the buffer (cap 3), so a guest
     * holding D rows no longer keeps them. */
    lead = 7;
    assert(snes_host_catchup_budget() == 3);
    lead = 2;
    assert(snes_host_catchup_budget() == 1);
    lead = 1;
    assert(snes_host_catchup_budget() == 0);
    lead = 1;
    assert(snes_host_auth_row_ready());
    lead = 0;
    assert(!snes_host_auth_row_ready());

    /* Paced catch-up (buffer 1; surplus = lead - 1 - buffer). */
    env_set("SNES_NET_CATCHUP_CAP_FAR", NULL);
    env_set("SNES_NET_CATCHUP_DRAW_ALL", NULL);
    {
        int k, extras;
        /* Nothing past the buffer: never an extra frame. */
        lead = 2; g_catchup_since = 0;
        for (k = 0; k < 10; ++k) assert(snes_host_catchup_plan() == 0);
        /* Small surplus (1..3): one extra frame every 4th present. */
        lead = 3; g_catchup_since = 0; extras = 0;
        for (k = 0; k < 12; ++k) extras += snes_host_catchup_plan();
        assert(extras == 3);
        lead = 3; g_catchup_since = 0;
        assert(snes_host_catchup_plan() == 0);
        assert(snes_host_catchup_plan() == 0);
        assert(snes_host_catchup_plan() == 0);
        assert(snes_host_catchup_plan() == 1);
        /* Mid surplus (4..11): every 3rd present. */
        lead = 8; g_catchup_since = 0; extras = 0;
        for (k = 0; k < 12; ++k) extras += snes_host_catchup_plan();
        assert(extras == 4);
        /* Far (12..29): a quarter of the gap, capped at 3, every present. */
        lead = 18; g_catchup_since = 0;   /* surplus 16 */
        assert(snes_host_catchup_plan() == 3);
        assert(snes_host_catchup_plan() == 3);
        lead = 14;                        /* surplus 12 */
        assert(snes_host_catchup_plan() == 3);
        /* Very far (30+): up to 8. */
        lead = 42;                        /* surplus 40 */
        assert(snes_host_catchup_plan() == 8);
        env_set("SNES_NET_CATCHUP_CAP_FAR", "5");
        assert(snes_host_catchup_plan() == 5);
        env_set("SNES_NET_CATCHUP_CAP_FAR", NULL);
        /* Only the burst's last frame is drawn. */
        assert(snes_host_catchup_skip_draw(0, 3));
        assert(snes_host_catchup_skip_draw(2, 3));
        assert(!snes_host_catchup_skip_draw(3, 3));
        assert(!snes_host_catchup_skip_draw(0, 0));
        env_set("SNES_NET_CATCHUP_DRAW_ALL", "1");
        assert(!snes_host_catchup_skip_draw(0, 3));
        env_set("SNES_NET_CATCHUP_DRAW_ALL", NULL);
        /* SNES_NET_CATCHUP_CAP=0 turns catch-up off, far or not. */
        env_set("SNES_NET_CATCHUP_CAP", "0");
        lead = 42; assert(snes_host_catchup_plan() == 0);
        env_set("SNES_NET_CATCHUP_CAP", NULL);
        /* Lockstep peers keep the plain budget (cap 0 by default) and never
         * skip pictures. */
        auth_guest = 0;
        lead = 42; assert(snes_host_catchup_plan() == 0);
        assert(!snes_host_catchup_skip_draw(0, 3));
        auth_guest = 1;
    }

    /* Repeated starvation grows the buffer, never past 4. */
    clock_ms += 1000;
    starve();
    assert(snes_host_auth_buffer_rows() == 2);
    assert(rows_to_unlatch() == 2);
    clock_ms += 1000; starve(); (void)rows_to_unlatch();
    clock_ms += 1000; starve(); (void)rows_to_unlatch();
    clock_ms += 1000; starve(); (void)rows_to_unlatch();
    assert(snes_host_auth_buffer_rows() == 4);
    /* Starvation far apart does not grow it. */
    clock_ms += 9000;
    starve(); (void)rows_to_unlatch();
    assert(snes_host_auth_buffer_rows() == 4);

    /* A calm stretch gives rows back, down to one. */
    clock_ms += 10000; assert(snes_host_auth_buffer_rows() == 3);
    clock_ms += 10000; assert(snes_host_auth_buffer_rows() == 2);
    clock_ms += 10000; assert(snes_host_auth_buffer_rows() == 1);
    /* Never below one row (SNES_AUTH_BUFFER_MIN). */
    clock_ms += 10000; assert(snes_host_auth_buffer_rows() == 1);
    clock_ms += 10000; assert(snes_host_auth_buffer_rows() == 1);
    lead = 0;
    assert(!snes_host_auth_row_ready());

    /* A new match starts over. */
    starv_reset();
    assert(snes_host_auth_buffer_rows() == 1);

    /* Pinned buffer. */
    env_set("SNES_NET_AUTH_BUFFER", "0");
    starv_reset();
    assert(snes_host_auth_buffer_rows() == 0);
    clock_ms += 100; starve(); (void)rows_to_unlatch();
    clock_ms += 100; starve(); (void)rows_to_unlatch();
    assert(snes_host_auth_buffer_rows() == 0);
    env_set("SNES_NET_AUTH_BUFFER", NULL);

    puts("auth guest buffer: PASS");
    return 0;
}
