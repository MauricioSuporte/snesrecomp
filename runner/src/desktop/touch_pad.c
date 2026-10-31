/* touch_pad.c — on-screen SNES controller for touch screens. See touch_pad.h.
 *
 * Everything is laid out from the output height (u = h), so the pad keeps its
 * physical size on any landscape screen. Drawn with SDL_RenderGeometry only
 * (filled circles, rectangles and an authored 3x5 letter font), so it works
 * the same on SDL2 (Android) and SDL3 (desktop testing). */
#include "touch_pad.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdl_compat.h"

#ifndef SNESRECOMP_SDL3
#define SNESRECOMP_SDL3 0
#endif

/* ── Layout ───────────────────────────────────────────────────────────────── */

typedef struct PadLayout {
  float dpad_x, dpad_y, dpad_r;
  float face_x, face_y, face_r, face_off;
  float lr_w, lr_h, l_x, r_x, lr_y;
  float pill_w, pill_h, pill_y, sel_x, start_x;   /* sel_x/start_x: left edges */
} PadLayout;

/* Sizes come from the short side (u), so a portrait phone does not get
 * landscape-height buttons. Landscape: L/R in the top corners and
 * Select/Start at the bottom centre, beside the picture. Portrait: the
 * picture sits above, so both rows stack above the d-pad and face buttons. */
static PadLayout pad_layout(int w, int h) {
  PadLayout L;
  const bool portrait = h > w;
  const float u = (float)(portrait ? w : h), m = 0.045f * u;
  L.dpad_r = 0.20f * u;
  L.dpad_x = m + L.dpad_r;
  L.dpad_y = (float)h - m - L.dpad_r;
  L.face_r = 0.075f * u;
  L.face_off = 0.13f * u;
  L.face_x = (float)w - m - L.face_off - L.face_r;
  L.face_y = (float)h - m - L.face_off - L.face_r;
  L.lr_w = 0.30f * u;
  L.lr_h = 0.11f * u;
  L.pill_w = 0.19f * u;
  L.pill_h = 0.065f * u;
  if (!portrait) {
    L.l_x = 0.025f * u;
    L.r_x = (float)w - 0.025f * u - L.lr_w;
    L.lr_y = 0.025f * u;
    L.pill_y = (float)h - 0.035f * u - L.pill_h;
    L.sel_x = w * 0.5f - 0.12f * u - L.pill_w * 0.5f;
    L.start_x = w * 0.5f + 0.12f * u - L.pill_w * 0.5f;
  } else {
    const float top = L.dpad_y - L.dpad_r - 0.05f * u;   /* above the clusters */
    L.pill_y = top - L.pill_h;
    L.sel_x = w * 0.5f - 0.04f * u - L.pill_w;
    L.start_x = w * 0.5f + 0.04f * u;
    L.lr_y = L.pill_y - 0.06f * u - L.lr_h;
    L.l_x = m;
    L.r_x = (float)w - m - L.lr_w;
  }
  return L;
}

typedef struct FaceButton { uint32_t bit; float dx, dy; char label; } FaceButton;
static const FaceButton kFace[4] = {
  { kTouchPad_A, 1, 0, 'A' }, { kTouchPad_B, 0, 1, 'B' },
  { kTouchPad_Y, -1, 0, 'Y' }, { kTouchPad_X, 0, -1, 'X' },
};

static bool in_rect(float x, float y, float rx, float ry, float rw, float rh,
                    float pad) {
  return x >= rx - pad && x <= rx + rw + pad && y >= ry - pad && y <= ry + rh + pad;
}

uint32_t touch_pad_hit(float x, float y, int w, int h) {
  if (w <= 0 || h <= 0) return 0;
  const PadLayout L = pad_layout(w, h);
  uint32_t b = 0;

  /* D-pad: eight 45-degree sectors around the centre, small dead zone. A
   * generous outer radius so a thumb that drifts off keeps steering. */
  {
    const float dx = x - L.dpad_x, dy = y - L.dpad_y;
    const float d = sqrtf(dx * dx + dy * dy);
    if (d <= L.dpad_r * 1.35f && d >= L.dpad_r * 0.18f) {
      const float a = atan2f(-dy, dx) * 57.2957795f;   /* 0 = right, 90 = up */
      const float s = a < 0 ? a + 360.0f : a;
      const int sector = (int)((s + 22.5f) / 45.0f) % 8;
      static const uint32_t kSector[8] = {
        kTouchPad_Right, kTouchPad_Right | kTouchPad_Up, kTouchPad_Up,
        kTouchPad_Up | kTouchPad_Left, kTouchPad_Left,
        kTouchPad_Left | kTouchPad_Down, kTouchPad_Down,
        kTouchPad_Down | kTouchPad_Right,
      };
      b |= kSector[sector];
    }
  }
  /* Face buttons: slightly enlarged circles, so a finger between two
   * neighbours presses both (Bomberman: drop a bomb while punching). */
  for (int i = 0; i < 4; ++i) {
    const float cx = L.face_x + kFace[i].dx * L.face_off;
    const float cy = L.face_y + kFace[i].dy * L.face_off;
    const float dx = x - cx, dy = y - cy;
    if (dx * dx + dy * dy <= (L.face_r * 1.45f) * (L.face_r * 1.45f))
      b |= kFace[i].bit;
  }
  const float lr_pad = L.lr_h * 0.25f, pill_pad = L.pill_h * 0.4f;
  if (in_rect(x, y, L.l_x, L.lr_y, L.lr_w, L.lr_h, lr_pad)) b |= kTouchPad_L;
  if (in_rect(x, y, L.r_x, L.lr_y, L.lr_w, L.lr_h, lr_pad)) b |= kTouchPad_R;
  if (in_rect(x, y, L.sel_x, L.pill_y, L.pill_w, L.pill_h, pill_pad))
    b |= kTouchPad_Select;
  if (in_rect(x, y, L.start_x, L.pill_y, L.pill_w, L.pill_h, pill_pad))
    b |= kTouchPad_Start;
  return b;
}

/* ── State ────────────────────────────────────────────────────────────────── */

#define kMaxFingers 10
typedef struct Finger { int64_t id; float x, y; bool down; } Finger;

static bool s_enabled, s_visible;
static Finger s_fingers[kMaxFingers];
static float s_test[kMaxFingers][2];
static int s_test_count;
static long s_test_from, s_draws;   /* test fingers start after this many draws */
static int s_out_w = 1, s_out_h = 1;

void touch_pad_init(void) {
  const char *e = getenv("SNESRECOMP_TOUCH_PAD");
#if defined(__ANDROID__)
  s_enabled = !(e && e[0] == '0');
#else
  s_enabled = e && e[0] && e[0] != '0';
#endif
  s_visible = s_enabled;
  const char *from = getenv("SNESRECOMP_TOUCH_PAD_TEST_FROM");
  s_test_from = from ? strtol(from, NULL, 0) : 0;
  const char *t = getenv("SNESRECOMP_TOUCH_PAD_TEST");
  s_test_count = 0;
  while (t && *t && s_test_count < kMaxFingers) {
    float x, y;
    if (sscanf(t, "%f,%f", &x, &y) != 2) break;
    s_test[s_test_count][0] = x;
    s_test[s_test_count][1] = y;
    ++s_test_count;
    t = strchr(t, ';');
    if (t) ++t;
  }
  if (s_enabled)
    fprintf(stderr, "[touch_pad] on-screen controller enabled%s\n",
            s_test_count ? " (test fingers)" : "");
}

bool touch_pad_enabled(void) { return s_enabled; }

void touch_pad_note_other_input(void) {
  if (s_test_count) return;
  s_visible = false;
  for (int i = 0; i < kMaxFingers; ++i) s_fingers[i].down = false;
}

static Finger *finger_slot(int64_t id, bool create) {
  Finger *free_slot = NULL;
  for (int i = 0; i < kMaxFingers; ++i) {
    if (s_fingers[i].down && s_fingers[i].id == id) return &s_fingers[i];
    if (!s_fingers[i].down && !free_slot) free_slot = &s_fingers[i];
  }
  return create ? free_slot : NULL;
}

bool touch_pad_handle_event(const void *ev) {
  const SDL_Event *e = (const SDL_Event *)ev;
  if (!s_enabled || !e) return false;
#if SNESRECOMP_SDL3
  const bool down = e->type == SDL_EVENT_FINGER_DOWN;
  const bool up = e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED;
  const bool motion = e->type == SDL_EVENT_FINGER_MOTION;
  const int64_t id = (int64_t)e->tfinger.fingerID;
#else
  const bool down = e->type == SDL_FINGERDOWN;
  const bool up = e->type == SDL_FINGERUP;
  const bool motion = e->type == SDL_FINGERMOTION;
  const int64_t id = (int64_t)e->tfinger.fingerId;
#endif
  if (!down && !up && !motion) return false;
  s_visible = true;
  Finger *f = finger_slot(id, down);
  if (!f) return true;
  if (up) {
    f->down = false;
    return true;
  }
  f->id = id;
  f->down = true;
  f->x = e->tfinger.x;
  f->y = e->tfinger.y;
  return true;
}

uint32_t touch_pad_buttons(void) {
  if (!s_enabled || !s_visible) return 0;
  uint32_t b = 0;
  for (int i = 0; i < kMaxFingers; ++i)
    if (s_fingers[i].down)
      b |= touch_pad_hit(s_fingers[i].x * s_out_w, s_fingers[i].y * s_out_h,
                         s_out_w, s_out_h);
  for (int i = 0; s_draws >= s_test_from && i < s_test_count; ++i)
    b |= touch_pad_hit(s_test[i][0] * s_out_w, s_test[i][1] * s_out_h,
                       s_out_w, s_out_h);
  return b;
}

/* ── Drawing ──────────────────────────────────────────────────────────────── */

#define kMaxVerts 6144
static SDL_Vertex s_v[kMaxVerts];
static int s_nv;

static void vtx(float x, float y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (s_nv >= kMaxVerts) return;
  SDL_Vertex *v = &s_v[s_nv++];
  memset(v, 0, sizeof *v);
  v->position.x = x;
  v->position.y = y;
#if SNESRECOMP_SDL3
  v->color.r = r / 255.0f; v->color.g = g / 255.0f;
  v->color.b = b / 255.0f; v->color.a = a / 255.0f;
#else
  v->color.r = r; v->color.g = g; v->color.b = b; v->color.a = a;
#endif
}

typedef struct Rgba { uint8_t r, g, b, a; } Rgba;
static void tri(float x0, float y0, float x1, float y1, float x2, float y2, Rgba c) {
  vtx(x0, y0, c.r, c.g, c.b, c.a);
  vtx(x1, y1, c.r, c.g, c.b, c.a);
  vtx(x2, y2, c.r, c.g, c.b, c.a);
}
static void rect(float x, float y, float w, float h, Rgba c) {
  tri(x, y, x + w, y, x + w, y + h, c);
  tri(x, y, x + w, y + h, x, y + h, c);
}
static void ring(float cx, float cy, float r0, float r1, Rgba c) {
  const int n = 40;
  for (int i = 0; i < n; ++i) {
    const float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
    const float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
    if (r0 <= 0.0f) {
      tri(cx, cy, cx + c0 * r1, cy + s0 * r1, cx + c1 * r1, cy + s1 * r1, c);
    } else {
      tri(cx + c0 * r0, cy + s0 * r0, cx + c0 * r1, cy + s0 * r1,
          cx + c1 * r1, cy + s1 * r1, c);
      tri(cx + c0 * r0, cy + s0 * r0, cx + c1 * r1, cy + s1 * r1,
          cx + c1 * r0, cy + s1 * r0, c);
    }
  }
}
static void pill(float x, float y, float w, float h, Rgba c) {
  /* One convex outline fanned from the centre: overlapping pieces would
   * double the alpha where they meet. */
  const float r = h * 0.5f, cx = x + w * 0.5f, cy = y + r;
  const int n = 16;
  float px[2 * (n + 1)], py[2 * (n + 1)];
  int k = 0;
  for (int i = 0; i <= n; ++i, ++k) {   /* right cap, top to bottom */
    const float a = -1.5707963f + 3.1415926f * i / n;
    px[k] = x + w - r + cosf(a) * r; py[k] = cy + sinf(a) * r;
  }
  for (int i = 0; i <= n; ++i, ++k) {   /* left cap, bottom to top */
    const float a = 1.5707963f + 3.1415926f * i / n;
    px[k] = x + r + cosf(a) * r; py[k] = cy + sinf(a) * r;
  }
  for (int i = 0; i < k; ++i) {
    const int j = (i + 1) % k;
    tri(cx, cy, px[i], py[i], px[j], py[j], c);
  }
}

/* Authored 3x5 capitals (rows top to bottom, bit 2 = left column). */
static uint16_t glyph(char ch) {
  switch (ch) {
  case 'A': return 0x2 << 12 | 0x5 << 9 | 0x7 << 6 | 0x5 << 3 | 0x5;
  case 'B': return 0x6 << 12 | 0x5 << 9 | 0x6 << 6 | 0x5 << 3 | 0x6;
  case 'C': return 0x3 << 12 | 0x4 << 9 | 0x4 << 6 | 0x4 << 3 | 0x3;
  case 'E': return 0x7 << 12 | 0x4 << 9 | 0x6 << 6 | 0x4 << 3 | 0x7;
  case 'L': return 0x4 << 12 | 0x4 << 9 | 0x4 << 6 | 0x4 << 3 | 0x7;
  case 'R': return 0x6 << 12 | 0x5 << 9 | 0x6 << 6 | 0x5 << 3 | 0x5;
  case 'S': return 0x3 << 12 | 0x4 << 9 | 0x2 << 6 | 0x1 << 3 | 0x6;
  case 'T': return 0x7 << 12 | 0x2 << 9 | 0x2 << 6 | 0x2 << 3 | 0x2;
  case 'X': return 0x5 << 12 | 0x5 << 9 | 0x2 << 6 | 0x5 << 3 | 0x5;
  case 'Y': return 0x5 << 12 | 0x5 << 9 | 0x2 << 6 | 0x2 << 3 | 0x2;
  default: return 0;
  }
}
static void text(const char *s, float cx, float cy, float px, Rgba c) {
  const int n = (int)strlen(s);
  float x = cx - (n * 4 - 1) * px * 0.5f;
  const float y = cy - 2.5f * px;
  for (int i = 0; i < n; ++i, x += 4 * px) {
    const uint16_t g = glyph(s[i]);
    for (int row = 0; row < 5; ++row)
      for (int col = 0; col < 3; ++col)
        if (g >> ((4 - row) * 3 + (2 - col)) & 1)
          rect(x + col * px, y + row * px, px, px, c);
  }
}

void touch_pad_draw(void *renderer, int w, int h) {
  if (w > 0 && h > 0) { s_out_w = w; s_out_h = h; }
  ++s_draws;
  if (!s_enabled || !s_visible || !renderer) return;
  const PadLayout L = pad_layout(w, h);
  const uint32_t held = touch_pad_buttons();
  const Rgba base = { 255, 255, 255, 46 }, edge = { 255, 255, 255, 110 },
             on = { 160, 200, 255, 150 }, ink = { 255, 255, 255, 190 };
  const float px = 0.012f * (h < w ? h : w);
  s_nv = 0;

  /* D-pad: disc, cross, lit arms. */
  ring(L.dpad_x, L.dpad_y, 0, L.dpad_r, base);
  ring(L.dpad_x, L.dpad_y, L.dpad_r - 0.06f * L.dpad_r, L.dpad_r, edge);
  {
    const float arm = L.dpad_r * 0.36f, len = L.dpad_r * 0.86f;
    rect(L.dpad_x - arm * 0.5f, L.dpad_y - len, arm, 2 * len, base);
    rect(L.dpad_x - len, L.dpad_y - arm * 0.5f, 2 * len, arm, base);
    if (held & kTouchPad_Up) rect(L.dpad_x - arm * 0.5f, L.dpad_y - len, arm, len - arm * 0.5f, on);
    if (held & kTouchPad_Down) rect(L.dpad_x - arm * 0.5f, L.dpad_y + arm * 0.5f, arm, len - arm * 0.5f, on);
    if (held & kTouchPad_Left) rect(L.dpad_x - len, L.dpad_y - arm * 0.5f, len - arm * 0.5f, arm, on);
    if (held & kTouchPad_Right) rect(L.dpad_x + arm * 0.5f, L.dpad_y - arm * 0.5f, len - arm * 0.5f, arm, on);
  }
  /* Face buttons. */
  for (int i = 0; i < 4; ++i) {
    const float cx = L.face_x + kFace[i].dx * L.face_off;
    const float cy = L.face_y + kFace[i].dy * L.face_off;
    ring(cx, cy, 0, L.face_r, (held & kFace[i].bit) ? on : base);
    ring(cx, cy, L.face_r - 0.13f * L.face_r, L.face_r, edge);
    const char s[2] = { kFace[i].label, 0 };
    text(s, cx, cy, px * 1.6f, ink);
  }
  /* Shoulders. */
  {
    pill(L.l_x, L.lr_y, L.lr_w, L.lr_h, (held & kTouchPad_L) ? on : base);
    pill(L.r_x, L.lr_y, L.lr_w, L.lr_h, (held & kTouchPad_R) ? on : base);
    text("L", L.l_x + L.lr_w * 0.5f, L.lr_y + L.lr_h * 0.5f, px * 1.4f, ink);
    text("R", L.r_x + L.lr_w * 0.5f, L.lr_y + L.lr_h * 0.5f, px * 1.4f, ink);
  }
  /* Select / Start. */
  {
    const float sx = L.sel_x, tx = L.start_x;
    pill(sx, L.pill_y, L.pill_w, L.pill_h, (held & kTouchPad_Select) ? on : base);
    pill(tx, L.pill_y, L.pill_w, L.pill_h, (held & kTouchPad_Start) ? on : base);
    text("SELECT", sx + L.pill_w * 0.5f, L.pill_y + L.pill_h * 0.5f, px * 0.7f, ink);
    text("START", tx + L.pill_w * 0.5f, L.pill_y + L.pill_h * 0.5f, px * 0.7f, ink);
  }

  SDL_Renderer *r = (SDL_Renderer *)renderer;
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  SDL_RenderGeometry(r, NULL, s_v, s_nv, NULL, 0);
}
