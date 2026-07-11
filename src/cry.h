/*
 * CRY (Jaguar TOM 16bpp color) -> RGBA8888 conversion.
 *
 * A CRY16 pixel is CCCC RRRR YYYYYYYY:
 *   high byte = chroma index into a 16x16 (cyan x red) hardware table
 *   low byte  = intensity Y, scaling the chroma's max-RGB linearly.
 */
#ifndef VLM_CRY_H
#define VLM_CRY_H

#include <stdint.h>

/* Build the 65536-entry CRY16 -> RGBA8888 (little-endian ABGR word) LUT. */
void cry_init(void);

extern uint32_t cry_to_rgba[0x10000];

#endif
