#pragma once
#include <stdint.h>
#include "display_aspect.h"

/* The shared desktop OpenGL presenter (desktop/opengl.c). util.h declares
 * OpenGLRenderer_Create itself; this header carries only the presentation
 * policy a host may need to override. */

#ifdef __cplusplus
extern "C" {
#endif

/* Force the swap interval instead of deriving it from config.
 *
 * Call BEFORE the renderer is created. A host that paces presentation on its
 * own clock (an FPS cap, or a split simulation/presentation rate) passes 0,
 * so SDL_GL_SwapWindow does not block on the display as well and fight that
 * pacing. Left unset, the presenter vsyncs unless config's DisableFrameDelay
 * is on. */
/* Swap interval for the GL presenter: 0 immediate, 1 wait for the panel,
 * -1 late-swap-tearing ("Adaptive"). Call before OpenGLRenderer_Create. */
void snesrecomp_opengl_set_vsync(int interval);
void snesrecomp_opengl_set_viewport(void (*compute)(int, int, int, int,
                                                   SnesDisplayViewport *));

/* Window-space overlay quads drawn after the frame and any GLSL shader, at
 * the drawable's resolution. Each quad is a w x h buffer of straight-alpha
 * 0xAARRGGBB words placed at (x, y) from the drawable's top-left. `slot`
 * (0..SNES_GL_OVERLAY_SLOTS-1) keeps one texture per buffer; the upload is
 * skipped while `generation` and the size stay the same. The callback also
 * receives where the frame_w x frame_h field lands (`frame_rect`: x, y from
 * the top-left, w, h) so it can anchor things to the picture. */
#define SNES_GL_OVERLAY_SLOTS 16
typedef struct SnesWindowOverlayQuad {
  const uint32_t *px;
  int x, y, w, h;
  uint32_t generation;
  int slot;
} SnesWindowOverlayQuad;
typedef int (*SnesOpenGLWindowOverlayFn)(int drawable_w, int drawable_h,
                                         const int frame_rect[4], int frame_w,
                                         int frame_h,
                                         const SnesWindowOverlayQuad **quads);
void snesrecomp_opengl_set_window_overlay(SnesOpenGLWindowOverlayFn fn);

/* Capture the next fully composited OpenGL frame to screenshots/*.bmp. */
void OpenGLRenderer_RequestScreenshot(void);

#ifdef __cplusplus
}
#endif
