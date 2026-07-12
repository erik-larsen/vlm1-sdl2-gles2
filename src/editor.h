/*
 * The hidden effects editor (docs/analysis-editor.md). One call per
 * frame: handles entry combo, menu/input state, widget displays and
 * parameter write-back; text goes to the board overlay.
 */
#ifndef VLM_EDITOR_H
#define VLM_EDITOR_H

#include <stdint.h>

#include "engine.h"

/* Run the editor for one frame. buttons = PAD_* mask; screen = the
 * current draw buffer (widgets render onto it like the GPU services
 * did). Returns nonzero while edit mode owns the keypad (the caller
 * must suppress normal bank/effect selection). */
int vlm_editor_frame(VlmEngine *e, uint32_t buttons, uint16_t *screen);

#endif
