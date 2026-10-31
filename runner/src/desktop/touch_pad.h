#pragma once
/* On-screen SNES controller for touch screens (Android by default).
 *
 * Layout in the spirit of mobile SNES emulators: an 8-way d-pad bottom left,
 * the A/B/X/Y diamond bottom right, L/R in the top corners and Select/Start
 * at the bottom centre, translucent over the picture. Multitouch; a finger
 * that slides keeps pressing whatever it is over, and a finger between two
 * face buttons presses both. The pad hides when a physical controller or the
 * keyboard is used and comes back on the next touch.
 *
 * Buttons come back in the runner's 12-bit seat layout (B 0x001, Y 0x002,
 * Select 0x004, Start 0x008, Up 0x010, Down 0x020, Left 0x040, Right 0x080,
 * A 0x100, X 0x200, L 0x400, R 0x800) for seat 0.
 *
 * Desktop testing: SNESRECOMP_TOUCH_PAD=1 enables it (0 disables it on
 * Android), and SNESRECOMP_TOUCH_PAD_TEST="x,y;x,y" holds fingers at those
 * normalized positions (from the SNESRECOMP_TOUCH_PAD_TEST_FROM-th drawn
 * frame on, default 0). */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum TouchPadButton {
  kTouchPad_B = 0x001, kTouchPad_Y = 0x002, kTouchPad_Select = 0x004,
  kTouchPad_Start = 0x008, kTouchPad_Up = 0x010, kTouchPad_Down = 0x020,
  kTouchPad_Left = 0x040, kTouchPad_Right = 0x080, kTouchPad_A = 0x100,
  kTouchPad_X = 0x200, kTouchPad_L = 0x400, kTouchPad_R = 0x800,
} TouchPadButton;

/* Pure layout + hit test (no SDL): the buttons a finger at (x, y) pixels
 * presses on a w x h output. Exposed for tests. */
uint32_t touch_pad_hit(float x, float y, int w, int h);

/* Call once after SDL video init. */
void touch_pad_init(void);
bool touch_pad_enabled(void);
/* Feeds an SDL_Event (const SDL_Event *). Returns true when it was a touch
 * event the pad consumed. */
bool touch_pad_handle_event(const void *sdl_event);
/* A physical controller or keyboard was used: hide until the next touch. */
void touch_pad_note_other_input(void);
/* Buttons held through the screen right now (0 when hidden or disabled). */
uint32_t touch_pad_buttons(void);
/* Draws the pad over the frame (SDL_Renderer *). */
void touch_pad_draw(void *sdl_renderer, int output_w, int output_h);

#ifdef __cplusplus
}
#endif
