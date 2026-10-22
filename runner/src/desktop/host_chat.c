/*
 * host_chat.c -- the netplay room's chat inside the running game.
 * See host_chat.h for the contract; this file is state, layout and a small
 * TrueType rasterizer (stb_truetype, as vendored by recomp-ui's Dear ImGui).
 */
#include "host_chat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/imgui/imstb_truetype.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* ---- state -------------------------------------------------------------- */

typedef struct ChatEntry {
  HostChatLine line;
  double arrival; /* < 0: history (was in the ring before this match) */
} ChatEntry;

static ChatEntry g_lines[HOST_CHAT_MAX_LINES];
static int g_count;
static int g_active;
static int g_typing;
static uint32_t g_seen_seq;
static int g_src_count;
static uint32_t g_src_tail;
static HostChatSource g_src;
static char g_input[HOST_CHAT_TEXT_LEN];
static size_t g_input_len;
static double g_open_at;
static char g_notice[HOST_CHAT_TEXT_LEN];
static double g_notice_at = -1.0;

static void clear_tags(void);

static void reset_session(void) {
  clear_tags();
  g_active = 0;
  g_typing = 0;
  g_count = 0;
  g_seen_seq = 0;
  g_src_count = 0;
  g_src_tail = 0;
  g_input[0] = '\0';
  g_input_len = 0;
  g_notice[0] = '\0';
  g_notice_at = -1.0;
  memset(&g_src, 0, sizeof(g_src));
}

void host_chat_tick(const HostChatSource *src, int active, double now) {
  ChatEntry fresh[HOST_CHAT_MAX_LINES];
  int first, n, start, fc = 0;
  uint32_t tail = 0, max_seq;
  if (!active || !src || !src->count || !src->get) {
    if (g_active) reset_session();
    return;
  }
  first = !g_active;
  g_active = 1;
  g_src = *src;
  n = src->count(src->ctx);
  if (n < 0) n = 0;
  if (n > 0) {
    HostChatLine t;
    memset(&t, 0, sizeof(t));
    if (src->get(src->ctx, n - 1, &t)) tail = t.seq;
  }
  if (!first && n == g_src_count && tail == g_src_tail) return;
  start = n > HOST_CHAT_MAX_LINES ? n - HOST_CHAT_MAX_LINES : 0;
  max_seq = g_seen_seq;
  for (int i = start; i < n; ++i) {
    ChatEntry e;
    memset(&e, 0, sizeof(e));
    if (!src->get(src->ctx, i, &e.line)) continue;
    e.line.from[HOST_CHAT_NAME_LEN - 1] = '\0';
    e.line.text[HOST_CHAT_TEXT_LEN - 1] = '\0';
    e.line.account[HOST_CHAT_ACCOUNT_LEN - 1] = '\0';
    if (src->hidden && src->hidden(src->ctx, &e.line)) continue;
    e.arrival = -1.0;
    for (int k = 0; k < g_count; ++k)
      if (g_lines[k].line.seq == e.line.seq && e.line.seq != 0) {
        e.arrival = g_lines[k].arrival;
        goto known;
      }
    if (!first && e.line.seq > g_seen_seq) e.arrival = now;
  known:
    if (e.line.seq > max_seq) max_seq = e.line.seq;
    fresh[fc++] = e;
  }
  memcpy(g_lines, fresh, sizeof(fresh[0]) * (size_t)fc);
  g_count = fc;
  g_seen_seq = max_seq;
  g_src_count = n;
  g_src_tail = tail;
}

int host_chat_active(void) { return g_active; }
int host_chat_typing(void) { return g_typing; }
const char *host_chat_input(void) { return g_input; }
int host_chat_line_count(void) { return g_count; }

const HostChatLine *host_chat_line(int index) {
  return index >= 0 && index < g_count ? &g_lines[index].line : NULL;
}

int host_chat_line_popup(int index, double now) {
  if (index < 0 || index >= g_count || g_lines[index].arrival < 0) return 0;
  return now - g_lines[index].arrival < HOST_CHAT_VISIBLE_SECONDS + HOST_CHAT_FADE_SECONDS;
}

int host_chat_open(double now) {
  if (!g_active || g_typing) return 0;
  g_typing = 1;
  g_input[0] = '\0';
  g_input_len = 0;
  g_open_at = now;
  return 1;
}

void host_chat_close(void) {
  g_typing = 0;
  g_input[0] = '\0';
  g_input_len = 0;
}

void host_chat_notice(const char *text, double now) {
  snprintf(g_notice, sizeof(g_notice), "%s", text ? text : "");
  g_notice_at = g_notice[0] ? now : -1.0;
}

int host_chat_key(int key, double now) {
  if (!g_typing) return 0;
  switch (key) {
  case HOST_CHAT_KEY_ENTER: {
    size_t a = 0, b = g_input_len;
    while (a < b && g_input[a] == ' ') a++;
    while (b > a && g_input[b - 1] == ' ') b--;
    if (b > a) {
      char line[HOST_CHAT_TEXT_LEN];
      int rc = -1;
      memcpy(line, g_input + a, b - a);
      line[b - a] = '\0';
      if (g_src.send) rc = g_src.send(g_src.ctx, line);
      if (rc != 0) host_chat_notice("Chat is not available in this match.", now);
    }
    host_chat_close();
    return 1;
  }
  case HOST_CHAT_KEY_ESCAPE:
    host_chat_close();
    return 1;
  case HOST_CHAT_KEY_BACKSPACE:
    if (g_input_len > 0) {
      do {
        g_input_len--;
      } while (g_input_len > 0 && ((unsigned char)g_input[g_input_len] & 0xC0) == 0x80);
      g_input[g_input_len] = '\0';
    }
    return 1;
  default:
    return 1;
  }
}

/* Length of the UTF-8 sequence at p, 0 when invalid. */
static int utf8_len(const unsigned char *p) {
  int n;
  if (p[0] < 0x80) return 1;
  if ((p[0] & 0xE0) == 0xC0) n = 2;
  else if ((p[0] & 0xF0) == 0xE0) n = 3;
  else if ((p[0] & 0xF8) == 0xF0) n = 4;
  else return 0;
  for (int i = 1; i < n; ++i)
    if ((p[i] & 0xC0) != 0x80) return 0;
  return n;
}

void host_chat_text(const char *utf8) {
  const unsigned char *p = (const unsigned char *)utf8;
  if (!g_typing || !p) return;
  while (*p) {
    int n = utf8_len(p);
    if (n == 0) { p++; continue; }
    if (n == 1 && (p[0] < 0x20 || p[0] == 0x7F)) { p++; continue; }
    if (g_input_len + (size_t)n > HOST_CHAT_TEXT_LEN - 1) break;
    memcpy(g_input + g_input_len, p, (size_t)n);
    g_input_len += (size_t)n;
    g_input[g_input_len] = '\0';
    p += n;
  }
}

static uint32_t utf8_next(const char **s) {
  const unsigned char *p = (const unsigned char *)*s;
  int n = utf8_len(p);
  uint32_t cp;
  if (n == 0) { *s += 1; return '?'; }
  if (n == 1) cp = p[0];
  else if (n == 2) cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
  else if (n == 3) cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
  else cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
            ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
  *s += n;
  return cp;
}

/* ---- font --------------------------------------------------------------- */

typedef struct Glyph {
  unsigned char *bmp;
  int w, h, xoff, yoff;
  float adv;
  int ready;
} Glyph;

#define GLYPH_LATIN 224 /* U+0020..U+00FF */
#define GLYPH_PUNCT 24  /* U+2010..U+2027 */
#define GLYPH_SLOTS (GLYPH_LATIN + GLYPH_PUNCT)

typedef struct FontCache {
  Glyph glyphs[GLYPH_SLOTS];
  int px;
  float scale;
  int ascent, descent;
} FontCache;

static unsigned char *g_ttf;
static stbtt_fontinfo g_font;
static int g_font_ok;
/* Two sizes live at once: the chat panel's and the name tags'. */
static FontCache g_caches[2];
static FontCache *g_fc = &g_caches[0];
#define g_glyphs (g_fc->glyphs)
#define g_glyph_px (g_fc->px)
#define g_scale (g_fc->scale)
#define g_ascent (g_fc->ascent)
#define g_descent (g_fc->descent)

static void free_cache(FontCache *c) {
  for (int i = 0; i < GLYPH_SLOTS; ++i)
    if (c->glyphs[i].bmp) stbtt_FreeBitmap(c->glyphs[i].bmp, NULL);
  memset(c, 0, sizeof(*c));
}

int host_chat_load_font(const char *path) {
  FILE *f;
  long size;
  unsigned char *data;
  if (!path || !path[0]) return 0;
  f = fopen(path, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0 || size > (32L << 20)) { fclose(f); return 0; }
  data = (unsigned char *)malloc((size_t)size);
  if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
    fclose(f);
    free(data);
    return 0;
  }
  fclose(f);
  if (!stbtt_InitFont(&g_font, data, stbtt_GetFontOffsetForIndex(data, 0))) {
    free(data);
    return 0;
  }
  free_cache(&g_caches[0]);
  free_cache(&g_caches[1]);
  free(g_ttf);
  g_ttf = data;
  g_font_ok = 1;
  return 1;
}

int host_chat_font_loaded(void) { return g_font_ok; }

static void set_font_px(int which, int px) {
  int asc, desc, gap;
  g_fc = &g_caches[which & 1];
  if (px == g_glyph_px) return;
  free_cache(g_fc);
  g_glyph_px = px;
  g_scale = stbtt_ScaleForPixelHeight(&g_font, (float)px);
  stbtt_GetFontVMetrics(&g_font, &asc, &desc, &gap);
  g_ascent = (int)lroundf((float)asc * g_scale);
  g_descent = (int)lroundf((float)-desc * g_scale);
}

static int glyph_slot(uint32_t cp) {
  if (cp >= 0x20 && cp <= 0xFF) return (int)(cp - 0x20);
  if (cp >= 0x2010 && cp <= 0x2027) return GLYPH_LATIN + (int)(cp - 0x2010);
  return -1;
}

static const Glyph *glyph(uint32_t cp) {
  int slot = glyph_slot(cp);
  Glyph *g;
  if (slot < 0 || (cp != ' ' && stbtt_FindGlyphIndex(&g_font, (int)cp) == 0)) {
    cp = '?';
    slot = glyph_slot(cp);
  }
  g = &g_glyphs[slot];
  if (!g->ready) {
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&g_font, (int)cp, &adv, &lsb);
    g->adv = (float)adv * g_scale;
    g->bmp = stbtt_GetCodepointBitmap(&g_font, 0, g_scale, (int)cp, &g->w, &g->h,
                                      &g->xoff, &g->yoff);
    g->ready = 1;
  }
  return g;
}

/* ---- layout ------------------------------------------------------------- */

#define COLOR_TEXT   0xF5F1E6u
#define COLOR_SYSTEM 0x7CE38Bu
#define COLOR_NOTICE 0xFFB347u
#define COLOR_SELF   0xFFD54Fu
#define COLOR_INPUT  0xFFFFFFu

static const uint32_t kNameColors[] = {
  0x4FC3F7u, 0xBA68C8u, 0x81C784u, 0xF06292u,
  0x4DD0E1u, 0x9FA8FFu, 0xFF7A7Au, 0xC5E1A5u,
};

typedef struct Cell { uint32_t cp, rgb; } Cell;
#define MAX_CELLS 320
#define MAX_ROWS 96

typedef struct Row {
  const Cell *cells;
  int n;
  int width;
  float alpha;
  int input;
} Row;

static Cell g_cells[HOST_CHAT_MAX_LINES + 2][MAX_CELLS];
static Row g_rows[MAX_ROWS];

static uint32_t name_color(const HostChatLine *l) {
  const char *key = l->account[0] ? l->account : l->from;
  uint32_t h = 2166136261u;
  if (l->is_local) return COLOR_SELF;
  for (; *key; ++key) h = (h ^ (unsigned char)*key) * 16777619u;
  return kNameColors[h % (sizeof(kNameColors) / sizeof(kNameColors[0]))];
}

static int push_text(Cell *cells, int n, const char *s, uint32_t rgb) {
  while (s && *s && n < MAX_CELLS) {
    cells[n].cp = utf8_next(&s);
    cells[n].rgb = rgb;
    n++;
  }
  return n;
}

static int line_cells(Cell *cells, const HostChatLine *l) {
  int n = 0;
  if (l->is_system || !l->from[0]) return push_text(cells, 0, l->text, COLOR_SYSTEM);
  n = push_text(cells, n, l->from, name_color(l));
  n = push_text(cells, n, ": ", name_color(l));
  return push_text(cells, n, l->text, COLOR_TEXT);
}

static int cells_width(const Cell *c, int n) {
  float x = 0;
  while (n > 0 && c[n - 1].cp == ' ') n--;
  for (int i = 0; i < n; ++i) x += glyph(c[i].cp)->adv;
  return (int)ceilf(x);
}

/* Greedy word wrap into at most `cap` rows; returns the row count. */
static int wrap(const Cell *c, int n, int max_w, Row *out, int cap, float alpha) {
  int rows = 0, start = 0;
  while (start < n && rows < cap) {
    float x = 0;
    int i = start, brk = -1, end;
    while (i < n) {
      float adv = glyph(c[i].cp)->adv;
      if (x + adv > (float)max_w && i > start) break;
      if (c[i].cp == ' ') brk = i;
      x += adv;
      i++;
    }
    end = i;
    if (i < n && brk > start) end = brk + 1;
    out[rows].cells = c + start;
    out[rows].n = end - start;
    out[rows].width = cells_width(c + start, end - start);
    out[rows].alpha = alpha;
    out[rows].input = 0;
    rows++;
    start = end;
    while (start < n && c[start].cp == ' ') start++;
  }
  return rows;
}

/* ---- raster ------------------------------------------------------------- */

static uint32_t *g_buf;
static size_t g_buf_cap;
static int g_buf_w, g_buf_h;
static uint64_t g_last_key;
static uint32_t g_generation;
static uint32_t g_gen_counter; /* shared by every buffer handed out */

static inline void blend(uint32_t *d, uint32_t rgb, int a) {
  uint32_t dst = *d;
  int da = (int)(dst >> 24), oa, inv;
  if (a <= 0) return;
  if (a >= 255 || da == 0) {
    *d = ((uint32_t)(a > 255 ? 255 : a) << 24) | (rgb & 0xFFFFFFu);
    return;
  }
  inv = da * (255 - a) / 255;
  oa = a + inv;
  {
    uint32_t r = (((rgb >> 16) & 0xFF) * (uint32_t)a + ((dst >> 16) & 0xFF) * (uint32_t)inv) / (uint32_t)oa;
    uint32_t g = (((rgb >> 8) & 0xFF) * (uint32_t)a + ((dst >> 8) & 0xFF) * (uint32_t)inv) / (uint32_t)oa;
    uint32_t b = ((rgb & 0xFF) * (uint32_t)a + (dst & 0xFF) * (uint32_t)inv) / (uint32_t)oa;
    *d = ((uint32_t)oa << 24) | (r << 16) | (g << 8) | b;
  }
}

static void fill_rect(int x, int y, int w, int h, uint32_t rgb, int a) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > g_buf_w) w = g_buf_w - x;
  if (y + h > g_buf_h) h = g_buf_h - y;
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i) blend(&g_buf[(size_t)(y + j) * (size_t)g_buf_w + (size_t)(x + i)], rgb, a);
}

static void draw_glyph(const Glyph *g, int ox, int oy, uint32_t rgb, int a) {
  if (!g->bmp) return;
  for (int j = 0; j < g->h; ++j) {
    int y = oy + g->yoff + j;
    if (y < 0 || y >= g_buf_h) continue;
    for (int i = 0; i < g->w; ++i) {
      int x = ox + g->xoff + i, cov = g->bmp[j * g->w + i];
      if (x < 0 || x >= g_buf_w || cov == 0) continue;
      blend(&g_buf[(size_t)y * (size_t)g_buf_w + (size_t)x], rgb, cov * a / 255);
    }
  }
}

static void draw_cells(const Cell *c, int n, int x0, int baseline, int alpha, int shadow) {
  float x = (float)x0;
  for (int i = 0; i < n; ++i) {
    const Glyph *g = glyph(c[i].cp);
    int gx = (int)lroundf(x);
    draw_glyph(g, gx + shadow, baseline + shadow, 0x000000u, alpha * 220 / 255);
    draw_glyph(g, gx, baseline, c[i].rgb, alpha);
    x += g->adv;
  }
}

static uint64_t mix(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h;
}

static float popup_alpha(double arrival, double now) {
  double age = now - arrival;
  if (arrival < 0 || age < 0) return arrival < 0 ? 0.0f : 1.0f;
  if (age < HOST_CHAT_VISIBLE_SECONDS) return 1.0f;
  age -= HOST_CHAT_VISIBLE_SECONDS;
  if (age >= HOST_CHAT_FADE_SECONDS) return 0.0f;
  return (float)(1.0 - age / HOST_CHAT_FADE_SECONDS);
}

int host_chat_render(int dw, int dh, double now, const uint32_t **px,
                     int *ox, int *oy, int *ow, int *oh, uint32_t *generation) {
  int font_px, line_h, pad_x, margin_x, margin_b, panel_w, text_w, shadow;
  int top_limit, rows = 0, input_row = -1, y, total_h;
  int candidates[HOST_CHAT_MAX_LINES + 1];
  float cand_alpha[HOST_CHAT_MAX_LINES + 1];
  int nc = 0;
  uint64_t key = 1469598103934665603ull;
  Row input;

  if (!g_active || !g_font_ok || dw < 64 || dh < 64) return 0;

  font_px = (int)lround(dh * 0.03);
  if (font_px < 12) font_px = 12;
  if (font_px > 48) font_px = 48;
  set_font_px(0, font_px);
  line_h = (int)lround(font_px * 1.3);
  pad_x = font_px * 3 / 10 > 3 ? font_px * 3 / 10 : 3;
  margin_x = font_px * 45 / 100 > 6 ? font_px * 45 / 100 : 6;
  margin_b = font_px * 6 / 10 > 6 ? font_px * 6 / 10 : 6;
  shadow = font_px / 16 > 1 ? font_px / 16 : 1;
  panel_w = (int)(dw * 0.36);
  if (panel_w < font_px * 14) panel_w = font_px * 14;
  if (panel_w > dw - 2 * margin_x) panel_w = dw - 2 * margin_x;
  text_w = panel_w - 2 * pad_x;
  top_limit = g_typing ? dh / 4 : dh / 2;

  /* Which lines: all of them while typing, else the ones still popping up. */
  for (int i = 0; i < g_count; ++i) {
    float a = g_typing ? 1.0f : popup_alpha(g_lines[i].arrival, now);
    if (a <= 0.0f) continue;
    candidates[nc] = i;
    cand_alpha[nc++] = a;
  }
  if (g_notice_at >= 0) {
    float a = popup_alpha(g_notice_at, now);
    if (a > 0.0f) {
      candidates[nc] = -1;
      cand_alpha[nc++] = a;
    } else {
      g_notice_at = -1.0;
    }
  }
  if (nc == 0 && !g_typing) return 0;

  key = mix(key, (uint64_t)dw);
  key = mix(key, (uint64_t)dh);
  key = mix(key, (uint64_t)g_typing);

  /* Input bar: the tail of what is typed that fits, plus a blinking caret. */
  memset(&input, 0, sizeof(input));
  if (g_typing) {
    Cell *c = g_cells[HOST_CHAT_MAX_LINES + 1];
    int n = push_text(c, 0, g_input, COLOR_INPUT), first = 0;
    int caret_on = ((long)((now - g_open_at) * 2.0) % 2) == 0;
    if (caret_on && n < MAX_CELLS) { c[n].cp = '_'; c[n].rgb = COLOR_INPUT; n++; }
    while (first < n && cells_width(c + first, n - first) > text_w) first++;
    input.cells = c + first;
    input.n = n - first;
    input.width = panel_w - 2 * pad_x;
    input.alpha = 1.0f;
    input.input = 1;
    key = mix(key, (uint64_t)caret_on);
    for (const char *s = g_input; *s; ++s) key = mix(key, (unsigned char)*s);
  }

  /* Rows bottom-up: newest message last, older ones moving up. */
  y = dh - margin_b - (g_typing ? line_h + line_h / 4 : 0);
  for (int k = nc - 1; k >= 0 && rows < MAX_ROWS; --k) {
    Row tmp[MAX_ROWS];
    Cell *c = g_cells[k < HOST_CHAT_MAX_LINES ? k : HOST_CHAT_MAX_LINES];
    int n, r;
    if (candidates[k] >= 0) {
      n = line_cells(c, &g_lines[candidates[k]].line);
      key = mix(key, g_lines[candidates[k]].line.seq);
    } else {
      n = push_text(c, 0, g_notice, COLOR_NOTICE);
      key = mix(key, 0xFFFFFFFFull);
    }
    r = wrap(c, n, text_w, tmp, MAX_ROWS - rows, cand_alpha[k]);
    key = mix(key, (uint64_t)(cand_alpha[k] * 64.0f));
    /* Keep the rows that fit, from the message's last row upwards. */
    for (int j = r - 1; j >= 0; --j) {
      if (y - line_h < top_limit || rows >= MAX_ROWS) goto laid_out;
      g_rows[rows++] = tmp[j];
      y -= line_h;
    }
  }
laid_out:
  if (rows == 0 && !g_typing) return 0;
  key = mix(key, (uint64_t)rows);

  total_h = (dh - margin_b) - y;
  if (key == g_last_key && g_buf && g_buf_w == panel_w && g_buf_h == total_h) {
    *px = g_buf; *ox = margin_x; *oy = y; *ow = g_buf_w; *oh = g_buf_h;
    *generation = g_generation;
    return 1;
  }
  if ((size_t)panel_w * (size_t)total_h > g_buf_cap) {
    free(g_buf);
    g_buf_cap = (size_t)panel_w * (size_t)total_h;
    g_buf = (uint32_t *)malloc(g_buf_cap * sizeof(uint32_t));
    if (!g_buf) { g_buf_cap = 0; return 0; }
  }
  g_buf_w = panel_w;
  g_buf_h = total_h;
  memset(g_buf, 0, (size_t)panel_w * (size_t)total_h * sizeof(uint32_t));

  {
    const int text_h = g_ascent + g_descent;
    const int base_off = (line_h - text_h) / 2 + g_ascent;
    int ry = 0;
    /* g_rows[0] is the bottom row; draw top-down. */
    for (int i = rows - 1; i >= 0; --i) {
      const Row *r = &g_rows[i];
      int a = (int)lroundf(r->alpha * 255.0f);
      int bw = r->width + 2 * pad_x;
      if (bw > panel_w) bw = panel_w;
      fill_rect(0, ry, bw, line_h, 0x000000u, 165 * a / 255);
      draw_cells(r->cells, r->n, pad_x, ry + base_off, a, shadow);
      ry += line_h;
    }
    if (g_typing) {
      int iy = total_h - line_h;
      fill_rect(0, iy, panel_w, line_h, 0x000000u, 200);
      fill_rect(0, iy + line_h - 2, panel_w, 2, COLOR_SELF, 200);
      draw_cells(input.cells, input.n, pad_x, iy + base_off, 255, shadow);
      input_row = iy;
    }
    (void)input_row;
  }

  g_last_key = key;
  g_generation = ++g_gen_counter;
  *px = g_buf; *ox = margin_x; *oy = y; *ow = g_buf_w; *oh = g_buf_h;
  *generation = g_generation;
  return 1;
}

/* ---- name tags ------------------------------------------------------------ */

typedef struct TagState {
  char name[HOST_CHAT_NAME_LEN];
  int visible;
  int frame_x, frame_y;
  /* cached bitmap */
  char drawn_name[HOST_CHAT_NAME_LEN];
  int drawn_px;
  uint32_t *buf;
  int w, h, ox, oy; /* ox: anchor x inside the bitmap; oy: bitmap bottom */
  uint32_t generation;
} TagState;

#define HOST_CHAT_TAG_ALPHA 191u /* 75% */

static TagState g_tags[HOST_CHAT_MAX_SEATS];

static void clear_tags(void) {
  for (int i = 0; i < HOST_CHAT_MAX_SEATS; ++i) {
    g_tags[i].visible = 0;
    g_tags[i].name[0] = '\0';
  }
}

void host_chat_set_seat_name(int seat, const char *name) {
  if (seat < 0 || seat >= HOST_CHAT_MAX_SEATS) return;
  snprintf(g_tags[seat].name, sizeof(g_tags[seat].name), "%s", name ? name : "");
}

const char *host_chat_seat_name(int seat) {
  return seat >= 0 && seat < HOST_CHAT_MAX_SEATS ? g_tags[seat].name : "";
}

void host_chat_set_tag(int seat, int visible, int frame_x, int frame_y) {
  if (seat < 0 || seat >= HOST_CHAT_MAX_SEATS) return;
  g_tags[seat].visible = visible ? 1 : 0;
  g_tags[seat].frame_x = frame_x;
  g_tags[seat].frame_y = frame_y;
}

int host_chat_tag_visible(int seat) {
  return seat >= 0 && seat < HOST_CHAT_MAX_SEATS && g_tags[seat].visible &&
         g_tags[seat].name[0];
}

/* White name with a dark outline, like a nameplate over the sprite. */
static int render_tag(TagState *t, int px) {
  Cell cells[HOST_CHAT_NAME_LEN];
  int n, tw, outline, w, h, base;
  if (t->buf && t->drawn_px == px && strcmp(t->drawn_name, t->name) == 0) return 1;
  set_font_px(1, px);
  n = push_text(cells, 0, t->name, 0xFFFFFFu);
  if (n > 16) n = 16; /* a long name stays a tag, not a banner */
  tw = cells_width(cells, n);
  outline = px / 8 > 1 ? px / 8 : 1;
  w = tw + 2 * outline + 2;
  h = g_ascent + g_descent + 2 * outline + 1;
  free(t->buf);
  t->buf = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
  if (!t->buf) return 0;
  {
    uint32_t *save = g_buf;
    int sw = g_buf_w, sh = g_buf_h;
    g_buf = t->buf; g_buf_w = w; g_buf_h = h;
    base = outline + g_ascent;
    for (int dy = -outline; dy <= outline; ++dy)
      for (int dx = -outline; dx <= outline; ++dx) {
        float x = (float)(outline + 1 + dx);
        if (!dx && !dy) continue;
        for (int i = 0; i < n; ++i) {
          const Glyph *g = glyph(cells[i].cp);
          draw_glyph(g, (int)lroundf(x), base + dy, 0x101010u, 235);
          x += g->adv;
        }
      }
    draw_cells(cells, n, outline + 1, base, 255, 0);
    g_buf = save; g_buf_w = sw; g_buf_h = sh;
  }
  /* The whole plate at 75% opacity: readable, without hiding the field. */
  for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) {
    const uint32_t a = t->buf[i] >> 24;
    t->buf[i] = (t->buf[i] & 0x00FFFFFFu) | ((a * HOST_CHAT_TAG_ALPHA / 255u) << 24);
  }
  t->w = w;
  t->h = h;
  t->ox = w / 2;
  t->oy = h;
  t->drawn_px = px;
  snprintf(t->drawn_name, sizeof(t->drawn_name), "%s", t->name);
  t->generation = ++g_gen_counter;
  return 1;
}

int host_chat_overlay(int dw, int dh, const int viewport[4], int frame_w, int frame_h,
                      double now, const HostOverlayQuad **out) {
  static HostOverlayQuad quads[HOST_CHAT_MAX_SEATS + 1];
  int n = 0;
  *out = quads;
  if (!g_active || !g_font_ok) return 0;
  /* Name tags first: the chat draws over them. */
  if (viewport && viewport[2] > 0 && viewport[3] > 0 && frame_w > 0 && frame_h > 0) {
    const float sx = (float)viewport[2] / (float)frame_w;
    const float sy = (float)viewport[3] / (float)frame_h;
    int px = (int)lroundf(sy * 7.5f);
    if (px < 11) px = 11;
    if (px > 44) px = 44;
    for (int i = 0; i < HOST_CHAT_MAX_SEATS; ++i) {
      TagState *t = &g_tags[i];
      if (!t->visible || !t->name[0] || !render_tag(t, px)) continue;
      quads[n].px = t->buf;
      quads[n].w = t->w;
      quads[n].h = t->h;
      quads[n].x = viewport[0] + (int)lroundf((float)t->frame_x * sx) - t->ox;
      if (quads[n].x > viewport[0] + viewport[2] - t->w) quads[n].x = viewport[0] + viewport[2] - t->w;
      if (quads[n].x < viewport[0]) quads[n].x = viewport[0];
      quads[n].y = viewport[1] + (int)lroundf((float)t->frame_y * sy) - t->oy;
      quads[n].generation = t->generation;
      quads[n].slot = 1 + i;
      n++;
    }
  }
  {
    const uint32_t *px;
    int x, y, w, h;
    uint32_t gen;
    if (host_chat_render(dw, dh, now, &px, &x, &y, &w, &h, &gen)) {
      quads[n].px = px;
      quads[n].x = x; quads[n].y = y; quads[n].w = w; quads[n].h = h;
      quads[n].generation = gen;
      quads[n].slot = 0;
      n++;
    }
  }
  return n;
}
