#include "engine.h"
#include "waves.h"
#include "platform.h" /* PAD_* for ADSR buttons */
#include "pbinfo_data.h"
#include "fx.h"

#include <string.h>

/*
 * Offsets within the 108-byte pixcon..delayn block (fxobj+896;
 * vlm.s:8432-8451).
 */
enum {
    PC_ADSRA = 56, PC_ADSRB = 66, PC_ADSRC = 76,
    PC_PY = 88, PC_PX = 92,
    PC_DELAYF = 96, PC_DELAYP = 98, PC_DLINE = 100,
    PC_DELAYT = 104, PC_DELAYN = 106,
};

/* avbank defaults: {start_bin, end_bin, trig_min, release_min}
 * (vlm.s:8522-8534). */
static const uint16_t avbank[5][4] = {
    {  1,  6, 0x7000, 0x1000 },
    {  7, 10, 0x6000, 0x1000 },
    { 11, 24, 0x5000, 0x1000 },
    { 25, 40, 0x4000, 0x1000 },
    { 41, 63, 0x3000, 0x1000 },
};

static void init_param_meta(void);

/* Sign-magnitude 16.16 multiply (fmult macro, omega.gas:71). */
static int32_t fmult(int32_t a, int32_t b)
{
    int neg = (a < 0) ^ (b < 0);
    uint32_t ua = a < 0 ? 0u - (uint32_t)a : (uint32_t)a;
    uint32_t ub = b < 0 ? 0u - (uint32_t)b : (uint32_t)b;
    uint32_t r = (uint32_t)(((uint64_t)ua * ub) >> 16);
    return neg ? -(int32_t)r : (int32_t)r;
}

void vlm_engine_init(VlmEngine *e)
{
    memset(e, 0, sizeof *e);
    vlm_waves_init();
    vlm_init_refblock();
    init_param_meta();
    for (int b = 0; b < 5; b++)
        e->band_trigmin[b] = avbank[b][2];
    e->effect = 1;
}

void vlm_engine_select(VlmEngine *e, int bank, int effect)
{
    if (bank != e->bank || !e->fx[0]) {
        e->bank = bank;
        vlm_load_bank(bank, e->matrix);
    }
    e->effect = effect;

    /* skidoo (vlm.s:4595): point slots into matrix, NULL where the
     * fxobj's info field is 0; restore the pixcon block from slot 0. */
    uint8_t *base = e->matrix + (effect - 1) * VLM_EFFECT_SIZE;
    for (int s = 0; s < VLM_SUBFX; s++) {
        uint8_t *obj = base + s * VLM_FXOBJ_SIZE;
        e->fx[s] = vlm_be32(obj + FX_INFO) ? obj : NULL;
    }
    memcpy(e->pixcon, base + 896, sizeof e->pixcon);
    /* (zapdel: delay-line rings reset — delay lines not yet ported) */
}

/* ---- ADSR (doadsr, vlm.s:906-1023) ---------------------------------- */

/* One channel; block = 10/12-byte adsr record inside e->pixcon (rates
 * and state word live in the record, as on the Jaguar). envval_idx:
 * A->7, B->6, C->5. */
static void do_adsr(VlmEngine *e, int block_off, int pressed,
                    int envval_idx)
{
    uint8_t *blk = e->pixcon + block_off;
    uint16_t state = vlm_be16(blk + 8);
    uint32_t level = e->envvals[envval_idx];

    switch (state) {
    case 0: /* trigwait */
        if (pressed)
            state = 1;
        break;
    case 1: /* attack */
        if (!pressed) {
            state = 4;
            break;
        }
        level += vlm_be16(blk + 0) >> 2;
        if (level > 0xFFFF) {
            level = 0xFFFF;
            state = 2;
        }
        break;
    case 2: /* decay */
        if (!pressed) {
            state = 4;
            break;
        }
        level -= vlm_be16(blk + 2) >> 2;
        if ((int32_t)level < (int32_t)vlm_be16(blk + 4)) {
            level = vlm_be16(blk + 4);
            state = 3;
        }
        break;
    case 3: /* sustain */
        if (!pressed)
            state = 4;
        break;
    case 4: /* release */
        if (pressed) {
            state = 1;
            break;
        }
        if (level >= (uint32_t)(vlm_be16(blk + 6) >> 2)) {
            level -= vlm_be16(blk + 6) >> 2;
        } else {
            level = 0;
            state = 0;
        }
        break;
    }
    blk[8] = (uint8_t)(state >> 8);
    blk[9] = (uint8_t)state;
    e->envvals[envval_idx] = (uint16_t)level;
}

/* ---- psi.gas: spectrum peak-hold, band envelopes, triggers ---------- */

void vlm_engine_spectrum(VlmEngine *e, const uint32_t *fft67)
{
    /* Peak-hold spectrum (psi.gas:42-99). */
    for (int b = 0; b < 64; b++) {
        uint32_t s = 0;
        if (fft67)
            s = (fft67[b] + fft67[b + 1] + fft67[b + 2] + fft67[b + 3]) / 4;
        uint32_t decayed = e->absdelta[b] > 0x3000 ?
                           e->absdelta[b] - 0x3000 : 0;
        e->absdelta[b] = decayed > s ? decayed : s;
    }
    for (int b = 0; b < 62; b++)
        e->peakflag[b] = e->absdelta[b + 1] > e->absdelta[b] &&
                         e->absdelta[b + 1] > e->absdelta[b + 2];

    /* Band envelopes + triggers (psi.gas routine65a). */
    uint8_t trigbits = (uint8_t)(e->trig & 0xFF);
    uint8_t toggles = (uint8_t)(e->trig >> 8);
    for (int b = 0; b < 5; b++) {
        uint32_t peak = 0;
        for (int k = avbank[b][0]; k <= avbank[b][1]; k++)
            if (e->absdelta[k] > peak)
                peak = e->absdelta[k];
        peak *= 2;
        if (peak > 0xFFFF)
            peak = 0xFFFF;

        uint32_t env = e->envvals[b] > 0x200 ? e->envvals[b] - 0x200u : 0;
        e->envvals[b] = (uint16_t)(peak > env ? peak : env);

        if (peak > e->band_trigmin[b]) {
            if (!(trigbits & (1 << b)))
                toggles ^= (uint8_t)(1 << b);
            trigbits |= (uint8_t)(1 << b);
            e->band_trigmin[b] = (uint16_t)peak;
        } else {
            e->band_trigmin[b] = e->band_trigmin[b] > 0x300 ?
                                 (uint16_t)(e->band_trigmin[b] - 0x300) : 0;
        }
        if (avbank[b][3] >= peak)
            trigbits &= (uint8_t)~(1 << b);
    }
    e->trig = (uint16_t)((toggles << 8) | trigbits);
}

/* ---- omega.gas: oscillators + modulation matrix --------------------- */

static const uint8_t *osc_table(const uint8_t *osc)
{
    int type = vlm_be16(osc + 14);
    if (type == 6) {
        /* skidoo leaves type-6 pointers untouched: the table comes
         * from the bank data itself (vlm.s:4616-4625). */
        return vlm_wave_table_from_addr(vlm_be32(osc + 8));
    }
    return vlm_wave_table(type > 8 ? 0 : type);
}

/* UpdateOSCBank + OSCValsLoop (omega.gas:307-382). */
static void run_oscillators(VlmEngine *e, uint8_t *fx)
{
    for (int k = 0; k < 8; k++) {
        uint8_t *osc = fx + 768 + k * 16;
        int type = vlm_be16(osc + 14);

        uint32_t phase = vlm_be32(osc + 0);
        if (type != WAVE_CONST) {
            phase += vlm_be32(osc + 4);
            vlm_wbe32(osc + 0, phase);
        }

        uint16_t out;
        if (type >= WAVE_USER_Y) {
            out = type == WAVE_USER_Y ? e->py : e->px;
        } else if (type == WAVE_RAMP) {
            out = (uint16_t)(phase >> 8);
        } else {
            const uint8_t *t = osc_table(osc);
            uint32_t idx = (phase >> 16) & 0xFF;
            int a = t[idx], b = t[idx + 1];
            out = (uint16_t)((a << 8) + (b - a) * (int)((phase >> 8) & 0xFF));
        }
        e->oscvals[k] = out;
    }
}

/* Parameter write-back metadata from pbinfo (vlm.s:7722), keyed by
 * the parameter long-index: word-type (2) params use (max*mod)>>16,
 * everything else fmult. */
typedef struct { uint8_t type; uint32_t max; } ParamMeta;
static ParamMeta param_meta[64];

static void init_param_meta(void)
{
    for (int r = 0; r < VLM_PBINFO_COUNT; r++) {
        const VlmPbInfo *p = &vlm_pbinfo[r];
        if (p->param_index < 64 && (p->type || p->max)) {
            param_meta[p->param_index].type = p->type;
            param_meta[p->param_index].max = p->max;
        }
    }
}

/* BigLoop64 (omega.gas:400-518). */
static void run_modulation(VlmEngine *e, uint8_t *fx)
{
    for (int i = 0; i < 64; i++) {
        uint32_t link = vlm_be32(fx + 256 + 4 * i);
        uint32_t sel = link >> 16;
        if (!sel)
            continue;

        int32_t acc = 0, w = 0x7FFF;
        uint32_t env = 0xFFFE;
        for (int k = 0; k < 8; k++) {
            if (sel & (1u << k)) {
                w = (w * (int32_t)(e->oscvals[k] - 0x8000)) >> 16;
                acc += w;
            }
            if (sel & (1u << (k + 8)))
                env = e->envvals[k];
        }

        uint32_t amp = (link & 0xFFFF) >> 1;
        uint32_t scale = (env * amp) >> 16;
        acc = (int32_t)((int64_t)acc * (int32_t)scale >> 15);
        uint16_t mod = (uint16_t)(acc + 0x8000);

        uint32_t M = param_meta[i].max;
        int32_t v;
        if ((param_meta[i].type & 0x7F) == 2)
            v = (int32_t)(((uint64_t)M * mod) >> 16);
        else
            v = fmult((int32_t)M, (int32_t)mod);
        v -= (int32_t)(M >> 1);
        v += (int32_t)vlm_be32(fx + 512 + 4 * i);

        vlm_wbe32(fx + 4 * i, (uint32_t)v);
        e->results[i] = mod;
    }
}

/* Dispatch a drawing module by the fxobj gpu field (gpumods order,
 * vlmgpu.s:113: 0 alpha, 1 beta, 2 gamma, 3 psi, 4 delta, 5 epsilon,
 * 6 theta, 7 sigma, 8 tau, 9 shu, 10 dbeast). Unported modules are
 * no-ops for now. */
static void run_module(VlmEngine *e, uint8_t *fx, uint16_t *screen,
                       const uint16_t *cscreen)
{
    uint32_t gpu = vlm_be32(fx + FX_GPU);
    VlmFxCtx c = {
        .screen = screen,
        .cscreen = cscreen,
        .fx = fx,
        .submode = gpu & 0xFFFF,
        .echo_run = 0,
        .skale = e->skale,
        .absdelta = e->absdelta,
    };
    switch (gpu >> 16) {
    case 0:
        vlm_fx_alpha(&c);
        break;
    case 1:
        vlm_fx_beta(&c);
        break;
    case 4:
        vlm_fx_delta(&c);
        break;
    case 9:
        vlm_fx_shu(&c);
        break;
    default:
        break;
    }
    e->skale = c.skale;
}

void vlm_engine_frame(VlmEngine *e, uint16_t *screen,
                      const uint16_t *cscreen)
{
    /* 68k VI side: ADSR envelopes -> envvals[5..7]. */
    do_adsr(e, PC_ADSRC, (e->buttons & PAD_C) != 0, 5);
    do_adsr(e, PC_ADSRB, (e->buttons & PAD_B) != 0, 6);
    do_adsr(e, PC_ADSRA, (e->buttons & PAD_A) != 0, 7);

    /* GPU omega, per fxobj: live module run with trig stored into
     * `height` first (omega.gas:264-293), then the oscillator +
     * modulation pass. Delay-line echo runs (dbeast) not yet ported. */
    for (int s = 0; s < VLM_SUBFX; s++) {
        uint8_t *fx = e->fx[s];
        if (!fx)
            continue;
        vlm_wbe32(fx + FX_HEIGHT, e->trig);
        if (screen)
            run_module(e, fx, screen, cscreen);
        run_oscillators(e, fx);
        run_modulation(e, fx);
    }
    /* psi runs after all fxobjs; the caller feeds spectrum data via
     * vlm_engine_spectrum() once per frame. */
}
