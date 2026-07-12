/*
 * The `board`: the VLM's 320x240 1bpp text/UI overlay (ObType 1,
 * transparent zeros, white ones via CLUT entries set to $FFFF at
 * startup). Displayed by beastie 2 at display X=24, Y=60 half-lines
 * (vlm.s:354-366).
 */
#ifndef VLM_BOARD_H_INCLUDED
#define VLM_BOARD_H_INCLUDED

#include <stdint.h>

#define VLM_BOARD_W 320
#define VLM_BOARD_H 240
#define VLM_BOARD_STRIDE 40 /* bytes per row */

extern uint8_t vlm_board[VLM_BOARD_STRIDE * VLM_BOARD_H];
extern int vlm_board_visible;

void vlm_board_clear(void);

/* Blit one 8x8 glyph at pixel position (x, y) (charblit). */
void vlm_board_char(int x, int y, uint8_t ch);

/* Composite the board over a 384x240 CRY view buffer (view position:
 * display X+8 = 32, Y/2 = 30). */
void vlm_board_composite(uint16_t *view, int view_w, int view_h);

#endif
