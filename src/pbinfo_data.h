/*
 * pbinfo_data.h
 *
 * Parameter-editing metadata for Jeff Minter's VLM-1 (Jaguar), hand-extracted
 * from the reverse-engineered 68k source:
 *   /Users/larsens/Github/vlm-ref/src/vlm/vlm.s
 *
 * Tables extracted:
 *   - pbinfo/editinginfo  (vlm.s:7722-8175)  64 records x 40 bytes
 *   - sineslookup         (vlm.s:7677-7685)  waveform -> wave table map
 *   - vars                (vlm.s:8179-8192)  effect type -> editable-param lists
 *   - per-effect param lists (vlm.s:8198-8263) and the editor screen lists
 *     symvars/dvfvars (vlm.s:8177-8178)
 *
 * pbinfo record layout (40 bytes, verified against consumers):
 *   +0  .. +29  display text: 29 chars + NUL (dc.b '...',0)
 *   +30         flags byte    (bit 0 set: text ends in ':' and the editor
 *                              appends the partner's axis label; see elcon,
 *                              vlm.s:1675 "btst #0,30(a4)")
 *   +31         partner index (pbinfo index of the complementary Y entry
 *                              shown with this X entry, 0 = none)
 *   +32         edvex index   (1-based index into edvex/editvex routine
 *                              tables, vlm.s:2520-2533: 1=crot 2=snglx
 *                              3=sisnglx 4=sidbl 5=dvect 6=rrts(bespoke init
 *                              routine in 'max') 7=wsshow 8=shspec.
 *                              0 = parameter not editable: elcon swaps the
 *                              edit routine for 'unquit', vlm.s:1659-1661)
 *   +33         type byte     (low bits: 1=byte, 2=word, 3=16.16 long;
 *                              bit 7 set = signed value)
 *   +34  word   param index   (index in 4-byte longs of the value inside the
 *                              fxobj; i.e. which of the 64 fxobj longs)
 *   +36  long   max value     (for edvex==6 records this is the address of
 *                              the bespoke init routine 'init_sym' in the
 *                              original ROM -- stored here as 0)
 *
 * Record stride of 40 bytes is confirmed by the index scaling in 'varadd'
 * (vlm.s:2509-2517) and 'elcon' (vlm.s:1651-1655): idx*8 + idx*32 = idx*40.
 * The three all-zero filler records at indices 39-41 and 62-63 are produced
 * in the assembly by '.dphrase' (align to 16) padding, which lands exactly on
 * the 40-byte record grid.
 */

#ifndef VLM_PBINFO_DATA_H
#define VLM_PBINFO_DATA_H

#include <stdint.h>

typedef struct {
    char     text[31];
    uint8_t  flags, partner, edvex, type;
    uint16_t param_index;
    uint32_t max;
} VlmPbInfo;

/* Type byte values (low 7 bits). Bit 7 (0x80) = signed. */
#define VLM_PB_TYPE_BYTE   0x01
#define VLM_PB_TYPE_WORD   0x02
#define VLM_PB_TYPE_LONG   0x03  /* 16.16 fixed point */
#define VLM_PB_TYPE_SIGNED 0x80

/* edvex routine indices (vlm.s:2520-2527, table at vlm.s:2533). */
#define VLM_EDVEX_NONE    0  /* not editable */
#define VLM_EDVEX_CROT    1
#define VLM_EDVEX_SNGLX   2
#define VLM_EDVEX_SISNGLX 3
#define VLM_EDVEX_SIDBL   4
#define VLM_EDVEX_DVECT   5
#define VLM_EDVEX_RRTS    6  /* bespoke init routine (init_sym) in 'max' */
#define VLM_EDVEX_WSSHOW  7
#define VLM_EDVEX_SHSPEC  8

/*
 * pbinfo / editinginfo -- vlm.s:7722-8175.
 * 64 records. Index into this array == parameter number used by the per-
 * effect parameter lists below.
 */
#define VLM_PBINFO_COUNT 64

static const VlmPbInfo vlm_pbinfo[VLM_PBINFO_COUNT] = {
    /*  0 */ { "Parameter not yet defined    ", 0x00, 0x00, 0x00, 0x00, 0x0000, 0x00000000 },
    /*  1 */ { "DVF window size: X           ", 0x01, 0x02, 0x04, 0x02, 0x0001, 0x01800000 },
    /*  2 */ { "DVF window size: Y           ", 0x00, 0x00, 0x04, 0x02, 0x0002, 0x01800000 },
    /*  3 */ { "DVF scale: X                 ", 0x01, 0x04, 0x04, 0x02, 0x0003, 0x03FFFFFF },
    /*  4 */ { "DVF scale: Y                 ", 0x00, 0x00, 0x04, 0x02, 0x0004, 0x03FFFFFF },
    /*  5 */ { "DVF rotate angle             ", 0x00, 0x00, 0x02, 0x82, 0x0005, 0x01FFFFFF },
    /*  6 */ { "DVF centre of rotation: X    ", 0x01, 0x07, 0x01, 0x02, 0x0006, 0x01800000 },
    /*  7 */ { "DVF centre of rotation: Y    ", 0x00, 0x00, 0x01, 0x02, 0x0007, 0x01800000 },
    /*  8 */ { "DVF Delta Intensity          ", 0x00, 0x00, 0x02, 0x02, 0x0008, 0x00FFFFFF },
    /*  9 */ { "Destination position: X      ", 0x01, 0x0A, 0x01, 0x02, 0x0009, 0x01800000 },
    /* 10 */ { "Destination position: Y      ", 0x00, 0x00, 0x01, 0x02, 0x000A, 0x01800000 },
    /* 11 */ { "DVF window centre: X         ", 0x01, 0x0C, 0x01, 0x02, 0x000B, 0x01800000 },
    /* 12 */ { "DVF window centre: Y         ", 0x00, 0x00, 0x01, 0x02, 0x000C, 0x01800000 },
    /* 13 */ { "Destination position: Z      ", 0x00, 0x00, 0x03, 0x02, 0x000D, 0x07FFFFFF },
    /* 14 */ { "Vector: X                    ", 0x01, 0x10, 0x05, 0x82, 0x000E, 0x00FFFFFF },
    /* 15 */ { "Destination Y offset         ", 0x00, 0x00, 0x03, 0x82, 0x000F, 0x01800000 },
    /* 16 */ { "Vector: Y                    ", 0x00, 0x00, 0x05, 0x82, 0x0010, 0x00FFFFFF },
    /* 17 */ { "Symmetry Types               ", 0x00, 0x00, 0x06, 0x03, 0x0011, 0x00000000 }, /* max = init_sym routine ptr in ROM */
    /* 18 */ { "Rotational Symmetry Order    ", 0x00, 0x00, 0x02, 0x01, 0x0012, 0x00000100 },
    /* 19 */ { "Rotational Angle Step        ", 0x00, 0x00, 0x02, 0x02, 0x0013, 0x00FFFFFF },
    /* 20 */ { "Rotational Angle Step Delta  ", 0x00, 0x00, 0x03, 0x82, 0x0014, 0x000FFFFF }, /* dc.l $0000FFFFF */
    /* 21 */ { "Intensity 1                  ", 0x00, 0x00, 0x02, 0x02, 0x0015, 0x0000FFFF },
    /* 22 */ { "Intensity 2                  ", 0x00, 0x00, 0x02, 0x02, 0x0016, 0x0000FFFF },
    /* 23 */ { "Intensity 3                  ", 0x00, 0x00, 0x02, 0x02, 0x0017, 0x0000FFFF },
    /* 24 */ { "Intensity 4                  ", 0x00, 0x00, 0x02, 0x02, 0x0018, 0x0000FFFF },
    /* 25 */ { "Intensity 5                  ", 0x00, 0x00, 0x02, 0x02, 0x0019, 0x0000FFFF },
    /* 26 */ { "Intensity 6                  ", 0x00, 0x00, 0x02, 0x02, 0x001A, 0x0000FFFF },
    /* 27 */ { "Z amplitude                  ", 0x00, 0x00, 0x02, 0x01, 0x001B, 0x0000FFFF },
    /* 28 */ { "Fixed point phase 1          ", 0x00, 0x00, 0x03, 0x82, 0x001C, 0x01FFFFFF },
    /* 29 */ { "Delta phase 1                ", 0x00, 0x00, 0x03, 0x82, 0x001D, 0x001FFFFF },
    /* 30 */ { "Rotational sym overall phase ", 0x00, 0x00, 0x03, 0x82, 0x001E, 0x01FFFFFF },
    /* 31 */ { "Fixed point phase 2          ", 0x00, 0x00, 0x03, 0x82, 0x001F, 0x01FFFFFF },
    /* 32 */ { "Number of iterations         ", 0x00, 0x00, 0x02, 0x02, 0x0020, 0x01000000 }, /* dc.b 0,0,2,2,0,' ',1,0,0,0 */
    /* 33 */ { "X amplitude                  ", 0x00, 0x00, 0x02, 0x01, 0x0021, 0x0000FFFF }, /* dc.b 0,0,2,1,0,'!',0,0,$FF,$FF */
    /* 34 */ { "Y amplitude                  ", 0x00, 0x00, 0x02, 0x01, 0x0022, 0x0000FFFF },
    /* 35 */ { "Number of other iterations   ", 0x00, 0x00, 0x02, 0x02, 0x0023, 0x01000000 },
    /* 36 */ { "Parameter not yet defined    ", 0x00, 0x00, 0x00, 0x00, 0x0000, 0x00000000 },
    /* 37 */ { "delta Z                      ", 0x00, 0x00, 0x02, 0x82, 0x0025, 0x00FFFFFF },
    /* 38 */ { "choice of Thang              ", 0x00, 0x00, 0x03, 0x02, 0x0026, 0x00FFFFFF },
    /* 39 */ { "Parameter not yet defined    ", 0x00, 0x00, 0x00, 0x00, 0x0000, 0x00000000 }, /* .dphrase pad, vlm.s:8011 */
    /* 40 */ { "Parameter not yet defined    ", 0x00, 0x00, 0x00, 0x00, 0x0000, 0x00000000 }, /* .dphrase pad, vlm.s:8014 */
    /* 41 */ { "Parameter not yet defined    ", 0x00, 0x00, 0x00, 0x00, 0x0000, 0x00000000 }, /* .dphrase pad, vlm.s:8018 */
    /* 42 */ { "Rotational Sym centre: X     ", 0x01, 0x2B, 0x01, 0x02, 0x002A, 0x01800000 }, /* dc.w $12B = flags/partner */
    /* 43 */ { "Rotational Sym centre: Y     ", 0x00, 0x00, 0x00, 0x02, 0x002B, 0x01800000 }, /* edvex 0: not directly editable */
    /* 44 */ { "Rotational Symmetry scale    ", 0x00, 0x00, 0x03, 0x81, 0x002C, 0x00003FFF },
    /* 45 */ { "Rotational scale delta: X    ", 0x01, 0x30, 0x04, 0x81, 0x002D, 0x000003FF }, /* dc.w $130 */
    /* 46 */ { "Colour generator vector: X   ", 0x01, 0x2F, 0x05, 0x82, 0x002E, 0x0007FFFF }, /* dc.w $12F; dc.l $00007FFFF */
    /* 47 */ { "Colour generator vector: Y   ", 0x00, 0x00, 0x05, 0x82, 0x002F, 0x0007FFFF },
    /* 48 */ { "Rotational scale delta: Y    ", 0x00, 0x00, 0x05, 0x81, 0x0030, 0x00001000 },
    /* 49 */ { "Rotational centre delta: X   ", 0x01, 0x32, 0x05, 0x82, 0x0031, 0x000FFFFF }, /* dc.w $132 */
    /* 50 */ { "Rotational centre delta: Y   ", 0x00, 0x00, 0x04, 0x82, 0x0032, 0x000FFFFF }, /* dc.l $0000FFFFF */
    /* 51 */ { "Delta phase 2                ", 0x00, 0x00, 0x03, 0x82, 0x0033, 0x001FFFFF },
    /* 52 */ { "Parameter not yet defined    ", 0x00, 0x00, 0x00, 0x00, 0x0000, 0x00000000 },
    /* 53 */ { "Radius                       ", 0x00, 0x00, 0x03, 0x82, 0x0035, 0x0FFFFFFF },
    /* 54 */ { "Base col generator vector: X ", 0x01, 0x37, 0x05, 0x82, 0x0036, 0x001FFFFF },
    /* 55 */ { "Base col generator vector: Y ", 0x00, 0x00, 0x05, 0x82, 0x0037, 0x001FFFFF },
    /* 56 */ { "Base colour: X               ", 0x01, 0x39, 0x05, 0x82, 0x0038, 0x001FFFFF },
    /* 57 */ { "Base colour: Y               ", 0x00, 0x00, 0x05, 0x82, 0x0039, 0x001FFFFF },
    /* 58 */ { "Destination plot routine     ", 0x00, 0x00, 0x06, 0x03, 0x003A, 0x00000000 }, /* max = init_sym routine ptr in ROM */
    /* 59 */ { "Maximum pixel size           ", 0x00, 0x00, 0x02, 0x02, 0x003B, 0x007FFFFF },
    /* 60 */ { "Parameter not yet defined    ", 0x00, 0x00, 0x00, 0x00, 0x0000, 0x00000000 },
    /* 61 */ { "Trigger mask                 ", 0x00, 0x00, 0x07, 0x03, 0x003D, 0x00000000 },
    /* 62 */ { "Parameter not yet defined    ", 0x00, 0x00, 0x00, 0x00, 0x0000, 0x00000000 }, /* .dphrase pad, vlm.s:8171 */
    /* 63 */ { "Parameter not yet defined    ", 0x00, 0x00, 0x00, 0x00, 0x0000, 0x00000000 }, /* .dphrase pad, vlm.s:8174 */
};

/*
 * sineslookup -- vlm.s:7677-7685.
 * Maps a source-waveform type (as listed in 'wts', vlm.s:7666-7675:
 *   0 Sine  1 Sawtooth  2 Square  3 Ramp  4 Rectified sine  5 Noise
 *   6 Constant  7 User control Y  8 User control X)
 * to the wavetable used by the DSP. Values below are wavetable ids:
 *   0=p_sines 1=p_saw 2=p_square 3=p_ramp 4=p_rect 5=rantab.
 * Note: entries 6-8 (Constant / User control Y / User control X) do not get
 * their own tables -- they fall back to p_sines; entry 5 (Noise) is the odd
 * one out, mapping to the random-number table 'rantab' rather than a
 * waveform table.
 */
#define VLM_WAVETABLE_SINES  0  /* p_sines  */
#define VLM_WAVETABLE_SAW    1  /* p_saw    */
#define VLM_WAVETABLE_SQUARE 2  /* p_square */
#define VLM_WAVETABLE_RAMP   3  /* p_ramp   */
#define VLM_WAVETABLE_RECT   4  /* p_rect   */
#define VLM_WAVETABLE_RANTAB 5  /* rantab   */

static const int vlm_sineslookup[9] = {
    VLM_WAVETABLE_SINES,   /* 0 Sine           -> p_sines */
    VLM_WAVETABLE_SAW,     /* 1 Sawtooth       -> p_saw */
    VLM_WAVETABLE_SQUARE,  /* 2 Square         -> p_square */
    VLM_WAVETABLE_RAMP,    /* 3 Ramp           -> p_ramp */
    VLM_WAVETABLE_RECT,    /* 4 Rectified sine -> p_rect */
    VLM_WAVETABLE_RANTAB,  /* 5 Noise          -> rantab */
    VLM_WAVETABLE_SINES,   /* 6 Constant       -> p_sines (fallback) */
    VLM_WAVETABLE_SINES,   /* 7 User control Y -> p_sines (fallback) */
    VLM_WAVETABLE_SINES,   /* 8 User control X -> p_sines (fallback) */
};

/*
 * Per-effect editable-parameter lists -- vlm.s:8198-8263.
 *
 * Each list is: a 29-char + NUL effect name (the same 30-byte text format as
 * pbinfo) followed by a 0xFF-terminated list of pbinfo indices naming the
 * parameters that are editable/relevant for that sub-effect type. 'elcon'
 * (vlm.s:1628-1676) walks the byte list until 0xFF.
 *
 * Here the names and index lists are split into separate C objects; the
 * index lists keep their 0xFF terminator so they can be walked the same way.
 */

/* polyvars, vlm.s:8198-8201: dc.w $921 / dc.l $221C1516,$1718191A / 26,3D,FF */
static const uint8_t vlm_polyvars_params[] = {
    0x09, 0x21, 0x22, 0x1C, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x26, 0x3D, 0xFF
};

/* sfvars, vlm.s:8202-8209 */
static const uint8_t vlm_sfvars_params[] = {
    0x09, 0x0D, 0x20, 0x3B, 0x38, 0x39, 0xFF
};

/* obvars, vlm.s:8211-8214: dc.w $090D / dc.l $3B0E2122,$38392E2F,$1C1D1F15,$161B2526 / FF */
static const uint8_t vlm_obvars_params[] = {
    0x09, 0x0D, 0x3B, 0x0E, 0x21, 0x22, 0x38, 0x39, 0x2E, 0x2F,
    0x1C, 0x1D, 0x1F, 0x15, 0x16, 0x1B, 0x25, 0x26, 0xFF
};

/* pmvars, vlm.s:8215-8218: dc.b $16 / dc.l $2023090D,$E251718,$1938152E / 33,FF */
static const uint8_t vlm_pmvars_params[] = {
    0x16, 0x20, 0x23, 0x09, 0x0D, 0x0E, 0x25, 0x17, 0x18, 0x19,
    0x38, 0x15, 0x2E, 0x33, 0xFF
};

/* ringvars, vlm.s:8219-8231 */
static const uint8_t vlm_ringvars_params[] = {
    0x09, 0x1C, 0x1F, 0x20, 0x35, 0x36, 0x38, 0x3B, 0x0E, 0x15, 0x16, 0xFF
};

/* dvf_vars, vlm.s:8233-8241 */
static const uint8_t vlm_dvf_vars_params[] = {
    0x01, 0x03, 0x05, 0x06, 0x08, 0x0B, 0x38, 0xFF
};

/* wsu_vars, vlm.s:8243-8245: dc.l $90D1C1D,$33202322,$E3B3638 / FF */
static const uint8_t vlm_wsu_vars_params[] = {
    0x09, 0x0D, 0x1C, 0x1D, 0x33, 0x20, 0x23, 0x22, 0x0E, 0x3B, 0x36, 0x38, 0xFF
};

/* monomapv, vlm.s:8246-8249: dc.b 9 / dc.l $2E363815 / 26,3D,FF */
static const uint8_t vlm_monomapv_params[] = {
    0x09, 0x2E, 0x36, 0x38, 0x15, 0x26, 0x3D, 0xFF
};

/* monomapv2, vlm.s:8250-8253: dc.b 9,$2E,$36 / dc.l $3815263D / FF (same list) */
static const uint8_t vlm_monomapv2_params[] = {
    0x09, 0x2E, 0x36, 0x38, 0x15, 0x26, 0x3D, 0xFF
};

/* plas1var, vlm.s:8254-8257: dc.b 9 / dc.l $2E363821 / 22,3D,FF */
static const uint8_t vlm_plas1var_params[] = {
    0x09, 0x2E, 0x36, 0x38, 0x21, 0x22, 0x3D, 0xFF
};

/* logovars, vlm.s:8258-8260 */
static const uint8_t vlm_logovars_params[] = {
    0x09, 0x38, 0x15, 0xFF
};

/* shuuvars, vlm.s:8261-8263: dc.b 9 / dc.l $38363B22,$FF... */
static const uint8_t vlm_shuuvars_params[] = {
    0x09, 0x38, 0x36, 0x3B, 0x22, 0xFF
};

/*
 * Editor screen lists used directly by the symmetry and DVF editing screens
 * (not via 'vars'): symvars/dvfvars, vlm.s:8177-8178, referenced at
 * vlm.s:3264 and vlm.s:3173. Same 0xFF-terminated pbinfo-index format.
 * symvars: dc.l $111E1213,$142A2B2C,$2D302E2F,$313206FF
 * dvfvars: dc.l $1030506,$80B38FF
 */
static const uint8_t vlm_symvars_params[] = {
    0x11, 0x1E, 0x12, 0x13, 0x14, 0x2A, 0x2B, 0x2C,
    0x2D, 0x30, 0x2E, 0x2F, 0x31, 0x32, 0x06, 0xFF
};

static const uint8_t vlm_dvfvars_params[] = {
    0x01, 0x03, 0x05, 0x06, 0x08, 0x0B, 0x38, 0xFF
};

/*
 * vars -- vlm.s:8179-8192.
 * Maps the fxobj 'info' field to the effect's metadata. In the original,
 * 'info' is used directly as a byte offset into the table of longword
 * pointers ("move.l info(a2),d6 ... movea.l (a3,d6.w),a3", vlm.s:2979-2983),
 * so info == effect_type * 4. Index the C table with info / 4.
 * Note entries 3, 7 and 8 all share dvf_vars in the original.
 */
typedef struct {
    const char    *name;    /* 29-char effect name, as in the assembly */
    const uint8_t *params;  /* 0xFF-terminated list of pbinfo indices */
} VlmEffectVars;

#define VLM_VARS_COUNT 14

static const VlmEffectVars vlm_vars[VLM_VARS_COUNT] = {
    /*  0 */ { "Draw a polygon object        ", vlm_polyvars_params  },
    /*  1 */ { "Draw 3D starfield            ", vlm_sfvars_params    },
    /*  2 */ { "Draw a ring of pixels        ", vlm_ringvars_params  },
    /*  3 */ { "Digital Video Feedback area  ", vlm_dvf_vars_params  },
    /*  4 */ { "Wave Surface Thang           ", vlm_wsu_vars_params  },
    /*  5 */ { "Draw mono bitmap coloured    ", vlm_monomapv_params  },
    /*  6 */ { "Draw mono bitmap i-shaded    ", vlm_monomapv2_params },
    /*  7 */ { "Digital Video Feedback area  ", vlm_dvf_vars_params  },
    /*  8 */ { "Digital Video Feedback area  ", vlm_dvf_vars_params  },
    /*  9 */ { "Colour plasma area type 1    ", vlm_plas1var_params  },
    /* 10 */ { "Big Jaguar hardware sprite   ", vlm_logovars_params  },
    /* 11 */ { "Draw particle object         ", vlm_obvars_params    },
    /* 12 */ { "Do particle motion           ", vlm_pmvars_params    },
    /* 13 */ { "Spectrum as intensities      ", vlm_shuuvars_params  },
};

#endif /* VLM_PBINFO_DATA_H */
