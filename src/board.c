#include "board.h"
#include "font_data.h"
#include "banks.h" /* vlm_be16 */

#include <string.h>

uint8_t vlm_board[VLM_BOARD_STRIDE * VLM_BOARD_H];
int vlm_board_visible;

void vlm_board_clear(void)
{
    memset(vlm_board, 0, sizeof vlm_board);
}

void vlm_board_char(int col, int row, uint8_t ch, int inverse)
{
    if (ch >= 142)
        return;
    uint32_t off = vlm_be16(vlm_font + 4 + (uint32_t)ch * 2);
    /* char 4's index (1040) points past the glyph data; give it the
     * hollow-box glyph as the doc recommends */
    if (off + 8 > VLM_FONT_SIZE)
        off = 1032;
    const uint8_t *glyph = vlm_font + off;
    int x = col * 8, y = row * 9;
    for (int r = 0; r < 8; r++) {
        int by = y + r;
        if (by < 0 || by >= VLM_BOARD_H)
            continue;
        uint8_t bits = inverse ? (uint8_t)~glyph[r] : glyph[r];
        for (int bit = 0; bit < 8; bit++) {
            int bx = x + bit;
            if (bx < 0 || bx >= VLM_BOARD_W)
                continue;
            if (bits & (0x80 >> bit))
                vlm_board[by * VLM_BOARD_STRIDE + (bx >> 3)] |=
                    (uint8_t)(0x80 >> (bx & 7));
            else
                vlm_board[by * VLM_BOARD_STRIDE + (bx >> 3)] &=
                    (uint8_t)~(0x80 >> (bx & 7));
        }
    }
}

void vlm_board_composite(uint16_t *view, int view_w, int view_h)
{
    if (!vlm_board_visible)
        return;
    const int ox = 32, oy = 30;
    for (int y = 0; y < VLM_BOARD_H; y++) {
        int vy = oy + y;
        if (vy < 0 || vy >= view_h)
            continue;
        const uint8_t *row = vlm_board + y * VLM_BOARD_STRIDE;
        for (int x = 0; x < VLM_BOARD_W; x++) {
            if (!(row[x >> 3] & (0x80 >> (x & 7))))
                continue;
            int vx = ox + x;
            if (vx >= 0 && vx < view_w)
                view[vy * view_w + vx] = 0x00FF; /* CLUT $FFFF-ish: white */
        }
    }
}
