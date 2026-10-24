#include "snes_host_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desktop/sdl_compat.h"

#if defined(RECOMP_LAUNCHER) || defined(SNES_HOST_HAS_RECOMP_UI)

void snes_host_app_apply_launch(const RecompLauncherCNetplayLaunch *net,
                                SnesHostLaunchResult *out)
{
  if (!out)
    return;
  memset(out, 0, sizeof(*out));
  out->caps_ws_extra = -1;
  if (!net || !net->enabled)
    return;
  out->netplay_enabled = 1;
  out->from_lobby = 1;
  snes_netplay_config_defaults(&out->net_cfg);
  out->net_cfg.enabled = 1;
  out->net_cfg.local_slot = net->local_slot;
  /* The match's seat count. It used to be left at the config default of
   * two, so a lobby launch with three or more players opened a two-seat
   * session: the host found no ICE agent for its own slot ("no connected
   * agents to take (slot 0 of 2)") and never started, and every guest sat
   * behind a black screen. SNES_NET_SLOTS (apply_env below) still wins. */
  if (net->player_count >= 2)
    out->net_cfg.slot_count = net->player_count;
  out->net_cfg.spectator = net->is_spectator ? 1 : 0;
  out->net_cfg.spectator_wire_slot = net->spectator_wire_slot;
  out->net_cfg.input_player =
      (net->input_player == 0 || net->input_player == 1) ? net->input_player
                                                         : -1;
  out->net_cfg.session_id = net->session_id ? net->session_id : 1u;
  out->net_cfg.transport = 0;
  snprintf(out->net_cfg.bind_hostport, sizeof(out->net_cfg.bind_hostport), "%s",
           net->bind_hostport);
  snprintf(out->net_cfg.peer_hostport, sizeof(out->net_cfg.peer_hostport), "%s",
           net->peer_hostport);
  /* Room-settled mode from recomp-ui (its model defaults rollback ON).
   * Before apply_env so SNES_NET_MODE stays the operator override. */
  out->net_cfg.rollback = net->rollback ? 1 : 0;
  snes_netplay_apply_env(&out->net_cfg);
  if (net->input_delay >= 0 && net->input_delay <= 20)
    out->net_cfg.input_delay = net->input_delay;
  /* Invent runway (P = 4 + D, computed by recomp-ui and published in the
   * launch struct). This was being dropped on the floor: SNES fell back to
   * the engine default of 8, so a session at D=9 ran with a runway SHORTER
   * than its own delay — pred_depth walked to the cap on every relay stall
   * and tripped pcap FREEZE. Measured over a hotspot/TURN session: 10
   * RUNWAY_EMPTY events, one 209ms debt spike, mispredict never above 2. */
  if (net->input_prediction >= 2 && net->input_prediction <= 32)
    out->net_cfg.input_prediction = net->input_prediction;
  out->net_cfg.force_turn = 0;
  out->net_cfg.force_input_relay = net->force_input_relay ? 1 : 0;
#if defined(RECOMP_LAUNCHER_HAS_HOST_RELAY)
  out->net_cfg.transport_host = net->transport_host ? 1 : 0;
#endif
#if defined(RECOMP_LAUNCHER_HAS_HOST_RELAY_ICE)
  out->net_cfg.transport_ice_hub = net->transport_ice_hub ? 1 : 0;
#endif
  {
    const SnesLobbyMatchCaps *caps = snes_lobby_match_caps();
    if (caps && caps->valid) {
      out->caps_ws_extra = caps->ws_extra;
      if (caps->force_turn)
        out->net_cfg.force_turn = 1;
      /* Host-published settlement outranks the local UI mirror; the env
       * override was already applied above and is not revisited. */
      if (!getenv("SNES_NET_MODE"))
        out->net_cfg.rollback = caps->rollback ? 1 : 0;
    }
  }
}

void snes_host_app_begin_soft_return(RecompLauncherCGameInfo *gi,
                                     int set_resume_room)
{
  snes_host_lobby_prepare_rematch();
  snes_netplay_clear_return_to_lobby();
  if (!gi || !set_resume_room)
    return;
  gi->resume_netplay_room = 1;
  {
    const char *ep = snes_host_lobby_resume_endpoint();
    if (ep && ep[0])
      gi->resume_netplay_endpoint = ep;
  }
}

#endif /* RECOMP_LAUNCHER */

const char *snes_host_connect_timeout_error_code(int is_ice)
{
  return is_ice ? "connect_timeout_ice" : "connect_timeout_lan";
}

const char *snes_host_connect_timeout_message(int is_ice)
{
  return is_ice
             ? "Could not establish an online connection to the other "
               "player within 30 seconds.\n\nAllow the game through the "
               "Windows firewall, make sure both players are still in the "
               "lobby, then rejoin and retry."
             : "Could not establish a direct connection to the other "
               "player within 30 seconds.\n\nCheck the lobby address, "
               "firewall, and that both players are still connected, then "
               "rejoin and retry.";
}

static void barrier_soft_exit(int from_lobby, int *running, const char *origin,
                              int *desync_logged, int *wait_logged);

#define SNES_STARVATION_ENTER_DEFAULT 4
#define SNES_STARVATION_EXIT_DEFAULT 3
#define SNES_STARVATION_EXIT_HR_LEAD_DEFAULT 0
#define SNES_STARVATION_GRACE_TICKS 60
/* Default 0: after starvation clears, resume ~1 sim/wall frame and let
 * remote_lead rebuild toward D instead of a turbo recovery burst. */
#define SNES_STARVATION_RECOVERY_BURST_DEFAULT 0
#define SNES_CATCHUP_CAP_DEFAULT 0
/* Host-authority guests: up to this many extra frames per wall tick while
 * rows are queued past the delay runway. The host never waits, so without a
 * catch-up every guest stall (a lost datagram's resend, a slow frame, a
 * window drag) became permanent lag that only grew, until the guest trailed
 * the host by more than its history and froze for good. */
#define SNES_AUTH_CATCHUP_CAP_DEFAULT 3
/* A guest whose PC cannot hold 60 fps falls behind a little every second and
 * the 3-frame catch-up, each frame rendered in full, never closed the gap
 * (2026-10-23: a guest drifted ~3 frames/s behind until it was 100+ frames
 * late and the host's window refused its inputs). Catch-up frames are now
 * simulated without composing their picture (ppu_setSkipLineDraw, host
 * loop), and once the guest is this far behind the cap opens wider so a slow
 * PC still converges. SNES_NET_CATCHUP_CAP / SNES_NET_CATCHUP_CAP_FAR pin
 * them. */
#define SNES_AUTH_CATCHUP_FAR_ROWS 12
#define SNES_AUTH_CATCHUP_CAP_FAR_DEFAULT 8
/* Paced catch-up (2.2.4). 2.2.3 drained any surplus at once: on a fast PC
 * whose host delivers rows in pairs, nearly every other frame became a
 * two-frame step (200-290 skipped pictures per 5 s in the maintainer's log),
 * so motion judders and sprites the game multiplexes on alternate frames
 * vanish (the step always lands on the same parity). A small surplus is now
 * spread out: one extra frame at most every fourth presented frame below
 * SNES_AUTH_CATCHUP_MID_ROWS, every third below SNES_AUTH_CATCHUP_FAR_ROWS;
 * only a guest really far behind runs multi-frame bursts. */
#define SNES_AUTH_CATCHUP_MID_ROWS 4
#define SNES_AUTH_CATCHUP_NEAR_EVERY 4
#define SNES_AUTH_CATCHUP_MID_EVERY 3
#define SNES_AUTH_CATCHUP_FAST_ROWS 30

static int g_catchup_since; /* presented frames since the last extra frame */

static struct {
  int latched;
  int enter_run;
  int exit_run;
  int recovery_amount;
  int pending_consume;
  int latch_logged;
  int just_cleared;
} g_starv;

/* Host-authority guest jitter buffer state (see starv_runway). */
static struct {
  int inited;
  int target;
  int pinned;
  uint32_t last_starve_ms;
  uint32_t calm_since_ms;
} g_auth_buf;

static int starv_env_int(const char *name, int def)
{
  const char *v = getenv(name);
  long n;
  char *end;

  if (!v || !v[0])
    return def;
  n = strtol(v, &end, 10);
  if (end == v || *end != '\0' || n < 0 || n > 64)
    return def;
  return (int)n;
}

static void starv_reset(void)
{
  memset(&g_starv, 0, sizeof(g_starv));
  g_auth_buf.inited = 0; /* next match starts from the default buffer */
  g_catchup_since = 0;
}

/*
 * Host-authority guest jitter buffer.
 *
 * Under host authority the room's input delay D is lockstep's figure: the
 * host does not schedule guest input D ticks ahead, it folds in whatever has
 * arrived. The runway below used to be D for every peer, so after the first
 * hiccup a guest's starvation latch rebuilt D queued host rows and the
 * catch-up kept them: the guest ran D frames behind rows it already had,
 * on top of its round trip. A guest only needs enough rows to ride out
 * network jitter. It starts at one row, grows by one (to
 * SNES_AUTH_BUFFER_MAX) when it starves twice within
 * SNES_AUTH_BUFFER_GROW_MS, and gives one back after SNES_AUTH_BUFFER_CALM_MS
 * without starving; the catch-up drains the surplus. SNES_NET_AUTH_BUFFER
 * pins it. The host never waits either way, so this only moves the guest.
 */
#define SNES_AUTH_BUFFER_START 1
/* Never shrink below one row (2.2.3 allowed 0): with no row in hand every
 * late datagram stalls the guest a frame and the next arrival is repaid as a
 * double step -- a 30 Hz-looking judder that also hides objects the game
 * flickers on alternate frames. One row costs ~16 ms of input lag. */
#define SNES_AUTH_BUFFER_MIN 1
#define SNES_AUTH_BUFFER_MAX 4
#define SNES_AUTH_BUFFER_GROW_MS 3000u
#define SNES_AUTH_BUFFER_CALM_MS 10000u


static void auth_buf_init(void)
{
  const char *v;
  uint32_t now;
  if (g_auth_buf.inited)
    return;
  memset(&g_auth_buf, 0, sizeof(g_auth_buf));
  g_auth_buf.inited = 1;
  g_auth_buf.target = SNES_AUTH_BUFFER_START;
  v = getenv("SNES_NET_AUTH_BUFFER");
  if (v && v[0]) {
    char *end;
    long n = strtol(v, &end, 10);
    if (end != v && *end == '\0' && n >= 0 && n <= 16) {
      g_auth_buf.target = (int)n;
      g_auth_buf.pinned = 1;
    }
  }
  now = (uint32_t)snes_netplay_clock_ms();
  g_auth_buf.calm_since_ms = now;
}

static void auth_buf_log(const char *why)
{
  fprintf(stderr,
          "snes_netplay: host_authority guest buffer=%d rows (%s) sim=%u lead=%d\n",
          g_auth_buf.target, why, (unsigned)snes_netplay_sim_tick(),
          snes_netplay_remote_lead());
}

/* A guest starvation (latch) event: grow when it repeats soon. */
static void auth_buf_note_starve(void)
{
  uint32_t now;
  auth_buf_init();
  now = (uint32_t)snes_netplay_clock_ms();
  if (!g_auth_buf.pinned && g_auth_buf.last_starve_ms &&
      now - g_auth_buf.last_starve_ms < SNES_AUTH_BUFFER_GROW_MS &&
      g_auth_buf.target < SNES_AUTH_BUFFER_MAX) {
    g_auth_buf.target++;
    auth_buf_log("grew: repeated starvation");
  }
  g_auth_buf.last_starve_ms = now;
  g_auth_buf.calm_since_ms = now;
}

int snes_host_auth_buffer_rows(void)
{
  uint32_t now;
  auth_buf_init();
  now = (uint32_t)snes_netplay_clock_ms();
  if (!g_auth_buf.pinned && g_auth_buf.target > SNES_AUTH_BUFFER_MIN &&
      now - g_auth_buf.calm_since_ms >= SNES_AUTH_BUFFER_CALM_MS) {
    g_auth_buf.target--;
    g_auth_buf.calm_since_ms = now;
    auth_buf_log("shrank: steady connection");
  }
  return g_auth_buf.target;
}

/* Rows this peer keeps queued: the jitter buffer for a host-authority
 * guest, the input delay D for everything else (lockstep, observers). */
static int starv_runway(void)
{
  int delay;
  if (snes_netplay_host_authority_guest())
    return snes_host_auth_buffer_rows();
  delay = snes_netplay_input_delay();
  return delay < 0 ? 0 : delay;
}

int snes_host_auth_row_ready(void)
{
  if (!snes_netplay_host_authority_guest())
    return 0;
  return snes_netplay_remote_lead() >= snes_host_auth_buffer_rows();
}

static int starv_runway_ok(void)
{
  int lead = snes_netplay_remote_lead();
  int delay = starv_runway();
  int hr_lead = starv_env_int("SNES_NET_STARVATION_EXIT_HR_LEAD",
                              SNES_STARVATION_EXIT_HR_LEAD_DEFAULT);

  if (delay < 0)
    delay = 0;
  return lead >= delay + hr_lead;
}

static void barrier_soft_exit(int from_lobby, int *running, const char *origin,
                              int *desync_logged, int *wait_logged)
{
  starv_reset();
  snes_netplay_soft_exit_to_lobby(origin, from_lobby);
  snes_netplay_connect_wait_reset();
  if (desync_logged)
    *desync_logged = 0;
  if (wait_logged)
    *wait_logged = 0;
  if (running)
    *running = 0;
}

/*
 * The match was REFUSED: leave it. The one consumer of the rollback driver's
 * request_return_to_lobby while a session is up.
 *
 * A refusal (boot digest mismatch, mod-set refusal) raises the return-to-lobby
 * request with the session still running. Nothing in the runner read it: the
 * desktop host's admit loop never checked, so its match played on -- every
 * episode forking at its baseline after a boot fork, or two mod sets in one
 * match (NETPLAY.md section 4: abort rather than degrade). A game host that
 * checked the flag itself (Gundam's main.c) left, but only because it had
 * copied the check. So it is consumed HERE, in the admit pump every host
 * calls, and a host inherits it by pumping admission.
 *
 * The session being up is what separates a refusal from a request this file
 * made itself: every soft exit below shuts the session down first. The reason
 * is logged, and handed to the launcher's last_error so the waiting room that
 * reopens says why (recomp-ui docs/HOST_NETPLAY.md, "soft-return"). From a
 * lobby, the host then soft-returns both players to the room; each peer
 * refuses on its own evidence, and one that has not refused yet sees our
 * BYE and leaves on it.
 */
static int barrier_refused(int from_lobby, int *running, int *desync_logged,
                           int *wait_logged)
{
  const char *why;

  if (!snes_netplay_return_to_lobby_requested())
    return 0;
  why = snes_netplay_refusal();
  if (!why || !why[0])
    why = "refused";
  fprintf(stderr,
          "snes_netplay: match refused (%s) at sim=%u — leaving the match%s\n",
          why, (unsigned)snes_netplay_sim_tick(),
          from_lobby ? "; returning both players to the lobby" : "");
  if (from_lobby)
    snes_host_lobby_set_runtime_error(why);
  barrier_soft_exit(from_lobby, running, why, desync_logged, wait_logged);
  return 1;
}

static int barrier_poll_admit(int enter_need)
{
  if (snes_netplay_poll_admit()) {
    g_starv.enter_run = 0;
    if (g_starv.just_cleared) {
      int burst = starv_env_int("SNES_NET_STARVATION_RECOVERY_BURST",
                                SNES_STARVATION_RECOVERY_BURST_DEFAULT);
      g_starv.just_cleared = 0;
      g_starv.recovery_amount = burst;
      g_starv.pending_consume = burst > 0 ? 1 : 0;
      if (burst > 0) {
        fprintf(stderr,
                "snes_netplay: delay_sync_starvation cleared sim=%u lead=%d "
                "runway=%d — recovery burst %d\n",
                (unsigned)snes_netplay_sim_tick(), snes_netplay_remote_lead(),
                starv_runway(), burst);
      } else {
        fprintf(stderr,
                "snes_netplay: delay_sync_starvation cleared sim=%u lead=%d "
                "runway=%d — resume 1:1 (rebuild input buffer)\n",
                (unsigned)snes_netplay_sim_tick(), snes_netplay_remote_lead(),
                starv_runway());
      }
    }
    return 1;
  }

  g_starv.just_cleared = 0;
  /* Save/load/SRAM probe+xfer freezes admit by design — do not treat as
   * delay-sync input starvation (that latch held the post-load freeze). */
  if (snes_netplay_state_barrier()) {
    g_starv.enter_run = 0;
    return 0;
  }

  g_starv.enter_run++;
  if (g_starv.enter_run >= enter_need) {
    g_starv.latched = 1;
    g_starv.enter_run = 0;
    if (snes_netplay_host_authority_guest())
      auth_buf_note_starve();
    if (!g_starv.latch_logged) {
      fprintf(stderr,
              "snes_netplay: delay_sync_starvation latched sim=%u lead=%d runway=%d "
              "(enter=%d)\n",
              (unsigned)snes_netplay_sim_tick(), snes_netplay_remote_lead(),
              starv_runway(), enter_need);
      g_starv.latch_logged = 1;
    }
  }
  return 0;
}

int snes_host_barrier_admit(int from_lobby, int *running,
                            const SnesHostBarrierHooks *hooks)
{
  static int desync_logged;
  static int wait_logged;
  static int ice_fail_logged;
  uint32_t peer_ms;
  uint32_t connect_ms;
  uint32_t dt = 0, lh = 0, rh = 0;
  int want_soft = 0;
  uint16_t pad;
  const char *soft_origin;

  if (!snes_netplay_active()) {
    starv_reset();
    return 0;
  }
  if (!hooks || !hooks->capture_local_pad)
    return 0;

  peer_ms = hooks->peer_timeout_ms ? hooks->peer_timeout_ms : 1500u;
  connect_ms = hooks->connect_timeout_ms;

  /* A refusal raised since the last call (by whatever the driver read after
   * its admission: finish_frame, the wire). Before every other exit, so the log names the refusal
   * rather than whatever the peer's own departure looks like. */
  if (barrier_refused(from_lobby, running, &desync_logged, &wait_logged))
    return 0;

  /* A coordinated stop (SIGUSR1 -> rollback drain). Checked BEFORE the
   * peer-gone exit: the peer that finishes draining first leaves at once, and
   * its BYE made this side take the peer_disconnect exit below with its own
   * drain one poll from done -- no "RB quiesced", and the harness ledger
   * ungradable. So when the peer has gone mid-drain, let the driver read what
   * the peer left in the queue and finish first. */
  if (snes_netplay_draining() && snes_netplay_peer_disconnected(peer_ms))
    (void)snes_netplay_poll_admit();
  /* Likewise a peer that refused the match and left before we read why: its
   * BYE lands right behind the tick-0 digest or identity that makes US refuse
   * too, so let the driver read the queue once before calling it a
   * disconnect. Both peers then log the refusal, not one refusal and one
   * "peer_disconnect". */
  else if (snes_netplay_rollback_active() &&
           snes_netplay_peer_disconnected(peer_ms))
    (void)snes_netplay_poll_admit();
  if (barrier_refused(from_lobby, running, &desync_logged, &wait_logged))
    return 0;
  if (snes_netplay_quiesced()) {
    fprintf(stderr, "snes_netplay: rollback drained — exiting\n");
    barrier_soft_exit(from_lobby, running, "quiesced", &desync_logged,
                      &wait_logged);
    return 0;
  }

  if (snes_netplay_peer_disconnected(peer_ms)) {
    snes_netplay_log_peer_disconnect(peer_ms);
    barrier_soft_exit(from_lobby, running, "peer_disconnect", &desync_logged,
                      &wait_logged);
    ice_fail_logged = 0;
    return 0;
  }

  if (connect_ms && !snes_netplay_is_running()) {
    if (!wait_logged) {
      fprintf(stderr,
              "snes_netplay: waiting for peer transport=%s timeout=%ums\n",
              snes_netplay_transport_name(), (unsigned)connect_ms);
      wait_logged = 1;
    }
    snes_netplay_wait_report();
    if (snes_netplay_ice_failed() && !ice_fail_logged) {
      fprintf(stderr,
              "snes_netplay: ICE FAILED while waiting for peer — "
              "STUN/TURN path unusable (check Coturn / firewall)\n");
      ice_fail_logged = 1;
    }
    if (snes_netplay_connect_timed_out(connect_ms)) {
      if (hooks->on_connect_timeout)
        hooks->on_connect_timeout(hooks->ctx);
      barrier_soft_exit(from_lobby, running, "connect_timeout",
                        &desync_logged, &wait_logged);
      ice_fail_logged = 0;
      return 0;
    }
  } else {
    wait_logged = 0;
    ice_fail_logged = 0;
  }

  if (snes_netplay_input_desync(&dt, &lh, &rh)) {
    if (!desync_logged) {
      fprintf(stderr,
              "snes_netplay: INPUT desync tick=%u local=%08x remote=%08x — "
              "stalled\n",
              (unsigned)dt, (unsigned)lh, (unsigned)rh);
      desync_logged = 1;
    }
    if (hooks->poll_events)
      hooks->poll_events(hooks->ctx, &want_soft);
    if (want_soft) {
      soft_origin = (want_soft == 2) ? "sdl_quit" : "escape";
      barrier_soft_exit(from_lobby, running, soft_origin, &desync_logged,
                        &wait_logged);
      return 0;
    }
    return 0; /* skip sim; host presents held frame */
  }
  desync_logged = 0;

  pad = hooks->capture_local_pad(hooks->ctx);
  snes_netplay_stage_local(pad);

  if (hooks->poll_events)
    hooks->poll_events(hooks->ctx, &want_soft);
  if (want_soft) {
    soft_origin = (want_soft == 2) ? "sdl_quit" : "escape";
    barrier_soft_exit(from_lobby, running, soft_origin, &desync_logged,
                      &wait_logged);
    return 0;
  }

  if (g_starv.pending_consume) {
    g_starv.recovery_amount = 0;
    g_starv.pending_consume = 0;
  }

  /*
   * Rollback owns its own pacing. The starvation latch below is delay-sync
   * policy: it stalls when remote_lead drops under D, which under rollback is
   * the ordinary running state rather than a fault — inventing past missing
   * remote input is the mechanism, and retcomm-rbengine's scheduler already
   * decides when to wait instead. Leaving the latch armed would throttle
   * rollback straight back into lockstep.
   */
  if (snes_netplay_rollback_active()) {
    int admitted = snes_netplay_poll_admit();
    /* The refusal can be raised inside this very poll (the boot-digest gate
     * settles here). The driver then admits nothing, but leave now rather
     * than a frame later. */
    if (barrier_refused(from_lobby, running, &desync_logged, &wait_logged))
      return 0;
    return admitted;
  }

  if (snes_netplay_auth_guest_lost()) {
    const char *why =
        "Fell too far behind the host (the connection stalled for more than "
        "about two seconds). Rejoin the room to play again.";
    if (from_lobby)
      snes_host_lobby_set_runtime_error(why);
    barrier_soft_exit(from_lobby, running, "host_authority_guest_lost",
                      &desync_logged, &wait_logged);
    return 0;
  }

  {
    uint32_t sim = snes_netplay_sim_tick();
    int enter_need = starv_env_int("SNES_NET_STARVATION_ENTER_FRAMES",
                                   SNES_STARVATION_ENTER_DEFAULT);
    int exit_need = starv_env_int("SNES_NET_STARVATION_EXIT_FRAMES",
                                  SNES_STARVATION_EXIT_DEFAULT);

    if (sim < SNES_STARVATION_GRACE_TICKS) {
      if (snes_netplay_poll_admit())
        return 1;
      return 0;
    }

    if (g_starv.latched) {
      snes_netplay_pump();
      /* Drop a stale latch if we entered a deliberate state barrier (load). */
      if (snes_netplay_state_barrier()) {
        g_starv.latched = 0;
        g_starv.exit_run = 0;
        g_starv.enter_run = 0;
        g_starv.latch_logged = 0;
        g_starv.just_cleared = 0;
        return 0;
      }
      if (starv_runway_ok()) {
        g_starv.exit_run++;
        if (g_starv.exit_run >= exit_need) {
          g_starv.latched = 0;
          g_starv.exit_run = 0;
          g_starv.latch_logged = 0;
          g_starv.just_cleared = 1;
        } else {
          return 0;
        }
      } else {
        g_starv.exit_run = 0;
        return 0;
      }
    }

    if (barrier_poll_admit(enter_need))
      return 1;
  }
  return 0; /* input starvation / confirm stall — present held, retry next wall tick */
}

/* Catch-up frames allowed in this burst with `lead` rows queued. */
static int catchup_budget_at(int lead)
{
  int delay;
  int extra;
  int budget;
  int cap;
  int guest;

  if (!snes_netplay_active())
    return 0;
  /* No delay-sync catch-up burst under rollback: the scheduler paces admit,
   * and a burst here would race it. */
  if (snes_netplay_rollback_active())
    return 0;
  guest = snes_netplay_host_authority_guest();
  cap = starv_env_int("SNES_NET_CATCHUP_CAP",
                      guest ? SNES_AUTH_CATCHUP_CAP_DEFAULT
                            : SNES_CATCHUP_CAP_DEFAULT);
  if (cap <= 0 && g_starv.recovery_amount <= 0)
    return 0;
  delay = starv_runway();
  if (guest && cap > 0 && lead - delay >= SNES_AUTH_CATCHUP_FAR_ROWS) {
    int far = starv_env_int("SNES_NET_CATCHUP_CAP_FAR",
                            SNES_AUTH_CATCHUP_CAP_FAR_DEFAULT);
    if (far > cap)
      cap = far;
  }
  /* Only spend surplus above the runway (D, or a host-authority guest's
   * jitter buffer); keep the runway intact. */
  extra = lead - delay;
  if (extra < 0)
    extra = 0;
  budget = extra;
  if (g_starv.recovery_amount > budget)
    budget = g_starv.recovery_amount;
  if (budget > cap)
    budget = cap;
  return budget;
}

int snes_host_catchup_budget(void)
{
  return catchup_budget_at(snes_netplay_remote_lead());
}

/* Extra frames to run after the frame the barrier just admitted, decided
 * once per presented frame. Every frame but the last of the burst is never
 * shown, so its picture can be skipped (ppu_setSkipLineDraw).
 *
 * Lockstep peers keep the plain budget. A host-authority guest paces it by
 * how far it is past its jitter buffer (rows queued beyond this frame):
 *   1..3   one extra frame at most every 4th presented frame (~15 rows/s)
 *   4..11  one extra frame at most every 3rd (~20 rows/s)
 *   12..29 up to 3 extra per frame (SNES_NET_CATCHUP_CAP), a quarter of the gap
 *   30+    up to 8 (SNES_NET_CATCHUP_CAP_FAR): a slow PC or a long stall
 * SNES_NET_CATCHUP_CAP=0 turns catch-up off. */
int snes_host_catchup_plan(void)
{
  int lead, surplus, cap, far, extra = 0;

  if (!snes_netplay_host_authority_guest())
    return snes_host_catchup_budget();
  cap = starv_env_int("SNES_NET_CATCHUP_CAP", SNES_AUTH_CATCHUP_CAP_DEFAULT);
  if (cap <= 0)
    return 0;
  far = starv_env_int("SNES_NET_CATCHUP_CAP_FAR", SNES_AUTH_CATCHUP_CAP_FAR_DEFAULT);
  if (far < cap)
    far = cap;
  lead = snes_netplay_remote_lead();
  surplus = lead - 1 - starv_runway();
  if (surplus <= 0)
    extra = 0;
  else if (surplus < SNES_AUTH_CATCHUP_MID_ROWS)
    extra = g_catchup_since + 1 >= SNES_AUTH_CATCHUP_NEAR_EVERY ? 1 : 0;
  else if (surplus < SNES_AUTH_CATCHUP_FAR_ROWS)
    extra = g_catchup_since + 1 >= SNES_AUTH_CATCHUP_MID_EVERY ? 1 : 0;
  else {
    extra = surplus / 4;
    if (extra < 1)
      extra = 1;
    if (extra > (surplus >= SNES_AUTH_CATCHUP_FAST_ROWS ? far : cap))
      extra = surplus >= SNES_AUTH_CATCHUP_FAST_ROWS ? far : cap;
  }
  if (extra > surplus)
    extra = surplus;
  g_catchup_since = extra > 0 ? 0 : g_catchup_since + 1;
  return extra;
}

int snes_host_catchup_skip_draw(int burst, int plan)
{
  if (!snes_netplay_host_authority_guest())
    return 0;
  if (starv_env_int("SNES_NET_CATCHUP_DRAW_ALL", 0))
    return 0;
  return burst < plan;
}
