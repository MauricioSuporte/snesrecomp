/*
 * host_chat.h -- the netplay room's chat, inside the running game.
 *
 * The launcher's lobby chat keeps working while a match runs: the lobby
 * WebSocket is pumped every frame by the netplay session (snes_netplay.c,
 * np_pump_session), so lines keep landing in the same ring the launcher
 * draws, and sending is allowed for as long as the client is in the room.
 * This module is the in-game face of that ring:
 *
 *   - bottom-left of the WINDOW, newest line at the bottom, older lines
 *     moving up;
 *   - a new line stays visible for HOST_CHAT_VISIBLE_SECONDS, then fades;
 *   - the chat key (T) opens a text bar and shows as much history as fits;
 *     Enter sends and closes, Enter on an empty bar or Escape just closes.
 *
 * NOTHING here touches guest state. The overlay is rasterized at the
 * window's own resolution and composited by the presenter after the frame
 * (and after any GLSL shader), so it is in no savestate, frame dump or
 * netplay digest. While the bar is open the host sends a neutral KEYBOARD
 * pad to the session (a gamepad keeps playing): typing is input the peers
 * agree on, not a divergence.
 *
 * The module is SDL- and GL-free on purpose: the host maps its key events
 * to HOST_CHAT_KEY_* and hands over text-input UTF-8, and the presenter
 * uploads the ARGB buffer it returns. That keeps it unit-testable.
 */
#ifndef SNES_HOST_CHAT_H
#define SNES_HOST_CHAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOST_CHAT_TEXT_LEN 256
#define HOST_CHAT_NAME_LEN 64
#define HOST_CHAT_ACCOUNT_LEN 40
#define HOST_CHAT_MAX_LINES 64
#define HOST_CHAT_VISIBLE_SECONDS 5.0
#define HOST_CHAT_FADE_SECONDS 0.35

typedef struct HostChatLine {
  char from[HOST_CHAT_NAME_LEN];
  char text[HOST_CHAT_TEXT_LEN];
  char account[HOST_CHAT_ACCOUNT_LEN];
  int is_local;   /* sent by this client */
  int is_system;  /* join/leave/notice, no sender */
  uint32_t seq;   /* the room ring's sequence number, increasing */
} HostChatLine;

/* Where lines come from and go to: the room's chat (the launcher backend's
 * chat_send / chat_count / chat_get). `hidden` is optional and answers 1 for
 * a line the player's ignore list hides. */
typedef struct HostChatSource {
  void *ctx;
  int (*send)(void *ctx, const char *text);              /* 0 = accepted */
  int (*count)(void *ctx);
  int (*get)(void *ctx, int index, HostChatLine *out);   /* 1 = filled */
  int (*hidden)(void *ctx, const HostChatLine *line);
} HostChatSource;

enum {
  HOST_CHAT_KEY_NONE = 0,
  HOST_CHAT_KEY_ENTER,
  HOST_CHAT_KEY_ESCAPE,
  HOST_CHAT_KEY_BACKSPACE,
  HOST_CHAT_KEY_OTHER, /* any other key while typing: swallowed */
};

/* Called every host frame. `active` is "a netplay match is running and the
 * room chat is wired". A transition to inactive closes the bar and forgets
 * the session; a transition to active marks the lines already in the ring
 * as history (they show when the bar opens, they do not pop up). */
void host_chat_tick(const HostChatSource *src, int active, double now);

int  host_chat_active(void);
int  host_chat_typing(void);

/* The chat key. Returns 1 when the bar opened (the host then starts text
 * input and drops the text event the same key produces). */
int  host_chat_open(double now);
void host_chat_close(void);

/* A key press while typing. Returns 1 when consumed (always, while typing). */
int  host_chat_key(int key, double now);

/* UTF-8 from the platform's text input (accents, IME). Control characters
 * are dropped; the line is capped at HOST_CHAT_TEXT_LEN - 1 bytes on a
 * code-point boundary. */
void host_chat_text(const char *utf8);

/* The line being typed (for tests and the host). */
const char *host_chat_input(void);

/* A one-line local notice (e.g. "chat unavailable"), shown like a new line. */
void host_chat_notice(const char *text, double now);

/* TrueType font for the overlay (Lato from the launcher's assets). Returns 1
 * on success. Without a font the module still tracks lines and input but
 * host_chat_render draws nothing. */
int  host_chat_load_font(const char *ttf_path);
int  host_chat_font_loaded(void);

/* Rasterize the overlay for a dw x dh drawable. Returns 1 when there is
 * something to draw: *px is a w x h buffer of straight-alpha ARGB words
 * (0xAARRGGBB), to be placed at (x, y) from the TOP-LEFT of the drawable.
 * *generation changes whenever the pixels change, so a presenter can skip
 * the upload. The buffer stays valid until the next call. */
int  host_chat_render(int dw, int dh, double now, const uint32_t **px,
                      int *x, int *y, int *w, int *h, uint32_t *generation);

/* ---- name tags -------------------------------------------------------------
 * The player's name over the bomber they control. The HOST names the seats
 * (lobby seat = session slot = controller port on SNES); the GAME, which
 * knows where each seat's character is and whether it is alive, places the
 * tags every frame in FRAME pixels (x = centre, y = bottom of the tag). */
#define HOST_CHAT_MAX_SEATS 8
void host_chat_set_seat_name(int seat, const char *name);
const char *host_chat_seat_name(int seat);
void host_chat_set_tag(int seat, int visible, int frame_x, int frame_y);
int  host_chat_tag_visible(int seat);

/* Everything the presenter composites at window resolution: name tags, then
 * the chat panel. `viewport` is where the frame_w x frame_h field lands in the
 * drawable (x, y from the top-left, w, h). Returns the quad count; *quads
 * stays valid until the next call. `slot` is stable per buffer (0 = chat,
 * 1 + seat = tags) so a presenter can keep one texture per slot and re-upload
 * only when `generation` changes. */
typedef struct HostOverlayQuad {
  const uint32_t *px; /* straight-alpha 0xAARRGGBB, w x h */
  int x, y, w, h;     /* drawable pixels, top-left origin */
  uint32_t generation;
  int slot;
} HostOverlayQuad;
int host_chat_overlay(int drawable_w, int drawable_h, const int viewport[4],
                      int frame_w, int frame_h, double now,
                      const HostOverlayQuad **quads);

/* Lines currently held (newest last), for tests. */
int  host_chat_line_count(void);
const HostChatLine *host_chat_line(int index);
/* 1 when line `index` is in its pop-up window (new and younger than
 * VISIBLE + FADE seconds). */
int  host_chat_line_popup(int index, double now);

#ifdef __cplusplus
}
#endif

#endif /* SNES_HOST_CHAT_H */
