/*
 * theta + epsilon: the particle system pair.
 *
 * theta (gpu/theta.gas): simulation — spawns into pool 0 on a frame
 * divider carried in dvf_ws_x, integrates velocity/gravity, decays
 * life. No drawing.
 *
 * epsilon (gpu/epsilon.gas): renderer — reads pool (thang>>16)&1,
 * scales each particle's integer position by j/k/zamp, tumbles it
 * through three chained rotations (phase1 on x/y, phase4 on z/y,
 * phase2<<4 on x/z), translates, perspects and plots through the
 * shared symmetry chain. Colour/intensity come from the particle
 * record (set at spawn time).
 *
 * Line references to the respective .gas files throughout.
 */
#include "fx.h"
#include "banks.h"
#include "waves.h"

/* ---- theta: particle motion (gpu/theta.gas) ------------------------- */

void vlm_fx_theta(VlmFxCtx *c)
{
    uint8_t *fx = c->fx;
    VlmParticlePool *pool = &c->pools[0]; /* theta always uses pool 0
                                             (theta.gas:83-90) */

    /* Frame divider in dvf_ws_x (theta.gas:230-258): decrement every
     * frame; on expiry reload from i>>16 and try to spawn. */
    int32_t div = (int32_t)vlm_be32(fx + FX_DVF_WS_X) - 1;
    vlm_wbe32(fx + FX_DVF_WS_X, (uint32_t)div);
    if (div < 0) {
        vlm_wbe32(fx + FX_DVF_WS_X, vlm_be32(fx + FX_ITER) >> 16);
        if (500 - pool->count >= 0) {
            pool->count++;
            /* First slot with life == 0 (theta.gas:269-296). The
             * original scan is unbounded; we stop at the pool edge. */
            for (int s = 0; s < VLM_PARTICLE_SLOTS; s++) {
                VlmParticle *p = &pool->p[s];
                if (p->life)
                    continue;
                p->x = (int32_t)vlm_be32(fx + FX_DSTOFFX) - 0xC00000;
                p->y = (int32_t)vlm_be32(fx + FX_DSTOFFY) - 0xC00000;
                p->z = (int32_t)vlm_be32(fx + FX_DSTOFFZ);
                /* wide-fold colour word (theta.gas:194-217) */
                int32_t a = (int32_t)(vlm_be32(fx + FX_COLX) & 0x1FFFFF)
                            - 0x100000;
                int32_t b = (int32_t)(vlm_be32(fx + FX_COLY) & 0x1FFFFF)
                            - 0x100000;
                if (a < 0)
                    a = -a;
                if (b < 0)
                    b = -b;
                p->colour = (uint16_t)(((((uint32_t)a >> 16) << 4)
                                        | ((uint32_t)b >> 16)) << 8);
                p->life = (uint16_t)(vlm_be32(fx + FX_I1) | 1);
                p->vx = ((int32_t)vlm_be32(fx + FX_VECTORX) >> 4)
                        + (int32_t)(vlm_be32(fx + FX_CVX) << 1);
                p->vy = ((int32_t)vlm_be32(fx + FX_VECTORY) >> 4)
                        + (int32_t)(vlm_be32(fx + FX_CVY) << 1);
                p->vz = ((int32_t)vlm_be32(fx + FX_DELTAZ) >> 4)
                        + ((int32_t)vlm_be32(fx + FX_PHASE5) >> 1);
                break;
            }
        }
    }

    /* Integrate (theta.gas:301-349): per-axis gravity _i3/_i4/_i5 -
     * 32768, life decay _i2>>4; early exit once all live particles
     * (by the running count) are processed. */
    int32_t g3 = (int32_t)vlm_be32(fx + FX_I3) - 32768;
    int32_t g4 = (int32_t)vlm_be32(fx + FX_I4) - 32768;
    int32_t g5 = (int32_t)vlm_be32(fx + FX_I5) - 32768;
    uint32_t decay = vlm_be32(fx + FX_I2) >> 4;
    int32_t remaining = pool->count;
    int32_t kills = 0;

    for (int s = 0; s < VLM_PARTICLE_SLOTS; s++) {
        VlmParticle *p = &pool->p[s];
        if (p->life) {
            p->x += p->vx;
            p->vx += g3;
            p->y += p->vy;
            p->vy += g4;
            p->z += p->vz;
            p->vz += g5;
            int32_t life = (int32_t)p->life - (int32_t)decay;
            if (life < 1) {
                life = 0;
                kills++;
            }
            p->life = (uint16_t)life;
            remaining--;
        }
        if (remaining < 0)
            break;
    }
    pool->count -= kills;
}

/* ---- epsilon: particle renderer (gpu/epsilon.gas) -------------------- */

void vlm_fx_epsilon(VlmFxCtx *c)
{
    uint8_t *fx = c->fx;
    VlmParticlePool *pool =
        &c->pools[(vlm_be32(fx + FX_THANG) >> 16) & 1];

    c->asym = vlm_be32(fx + FX_ASYM_FLA);
    c->plot_mode = (int)vlm_be32(fx + FX_PLOT_MOD);
    c->pixscale = vlm_be32(fx + FX_PIXSIZE) >> 16;

    const uint8_t *tab = vlm_wave_table_from_addr(vlm_be32(fx + FX_SINE_BAS));
    /* three sin/cos pairs; cosine = phase + $400000 (epsilon.gas:72-193) */
    int32_t s4 = vlm_fx_wave_signed(tab, vlm_be32(fx + FX_PHASE4));
    int32_t c4 = vlm_fx_wave_signed(tab, vlm_be32(fx + FX_PHASE4) + 0x400000);
    uint32_t ph2 = vlm_be32(fx + FX_PHASE2) << 4;
    int32_t s2 = vlm_fx_wave_signed(tab, ph2);
    int32_t c2 = vlm_fx_wave_signed(tab, ph2 + 0x400000);
    int32_t s1 = vlm_fx_wave_signed(tab, vlm_be32(fx + FX_PHASE1));
    int32_t c1 = vlm_fx_wave_signed(tab, vlm_be32(fx + FX_PHASE1) + 0x400000);

    int32_t ox = (int32_t)vlm_be32(fx + FX_DSTOFFX) - 0xC00000;
    int32_t oy = (int32_t)vlm_be32(fx + FX_DSTOFFY) - 0xC00000;
    int32_t oz = (int32_t)vlm_be32(fx + FX_DSTOFFZ);
    uint32_t sj = vlm_be32(fx + FX_J);
    uint32_t sk = vlm_be32(fx + FX_K);
    uint32_t sz = vlm_be32(fx + FX_ZAMP);

    /* Render `count` live particles; dead slots don't count against
     * the budget (epsilon.gas:265-274, 634-636). Bounded by the pool
     * size where the hardware would walk on into adjacent RAM. */
    int32_t budget = pool->count;
    if (budget < 0)
        return;
    for (int s = 0; s < VLM_PARTICLE_SLOTS; s++) {
        const VlmParticle *p = &pool->p[s];
        if (!p->life)
            continue;
        /* the decrement-and-test sits after the render on hardware,
         * so the particle that takes the budget negative still draws */

        int32_t x = vlm_fx_imult(sj, (uint32_t)(p->x >> 16)) << 8;
        int32_t y = vlm_fx_imult(sk, (uint32_t)(p->y >> 16)) << 8;
        int32_t z = vlm_fx_imult(sz, (uint32_t)(p->z >> 16)) << 8;
        c->cc_pix = p->colour;
        c->intensity = p->life;

        /* rot 1: (x,y) by phase1 (epsilon.gas:275-391) */
        int32_t nx = vlm_fx_fmult(c1, x) - vlm_fx_fmult(s1, y);
        int32_t ny = vlm_fx_fmult(s1, x) + vlm_fx_fmult(c1, y);
        x = nx;
        y = ny;
        /* rot 2: (z,y) by phase4 (epsilon.gas:392-507) */
        int32_t nz = vlm_fx_fmult(c4, z) - vlm_fx_fmult(s4, y);
        ny = vlm_fx_fmult(s4, z) + vlm_fx_fmult(c4, y);
        z = nz;
        y = ny;
        /* rot 3: (x,z) by phase2<<4 (epsilon.gas:508-623) */
        nx = vlm_fx_fmult(c2, x) - vlm_fx_fmult(s2, z);
        nz = vlm_fx_fmult(s2, x) + vlm_fx_fmult(c2, z);
        x = nx;
        z = nz;

        x += ox;
        y += oy;
        z += oz;

        /* perspective (epsilon.gas:1053-1078); a z-rejected live
         * particle still consumes render budget */
        z >>= 14;
        if (z >= 1) {
            int32_t sx = (vlm_fx_sdiv(x << 14, (uint32_t)z) >> 7) + 0xC0;
            int32_t sy = (vlm_fx_sdiv(y << 14, (uint32_t)z) >> 7) + 0xC0;
            vlm_fx_symmetry_plot(c, sx, sy);
        }
        if (--budget < 0)
            break;
    }

    /* Echo-run self-update (epsilon.gas:638-676). */
    if (c->echo_run) {
        vlm_wbe32(fx + FX_J,
                  sj + (uint32_t)((int32_t)vlm_be32(fx + FX_VECTORX) >> 12));
        vlm_wbe32(fx + FX_K,
                  sk + (uint32_t)((int32_t)vlm_be32(fx + FX_VECTORY) >> 12));
        vlm_wbe32(fx + FX_ZAMP,
                  sz + (uint32_t)((int32_t)vlm_be32(fx + FX_DELTAZ) >> 12));
        int32_t d = (int32_t)(vlm_be32(fx + FX_I2) << 16) >> 19;
        vlm_wbe32(fx + FX_I1, vlm_be32(fx + FX_I1) + (uint32_t)d);
    }
}
