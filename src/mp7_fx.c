/*
 * mp7_fx.c - shared stereo reverb bus and its control API.
 *
 * The reverb is a bank of damped comb lines summed into four series allpass
 * diffusers per channel. Voices feed it through their per-instrument send;
 * the wet return is added to the dry mix. The delay memory is a single block
 * sized once for 48 kHz; active line lengths follow the sample rate and the
 * room size, so the bus never allocates after context creation.
 *
 * The bus is optional. LIBPOPS_FX_REVERB compiles the reverb and the delay
 * memory it works on. Without it no memory is allocated and the render loop
 * carries no reverb code; the control API stays available so a host needs no
 * conditional compilation, but the effect never renders.
 *
 * The bus is bypassed at run time when the wet level is zero or when no
 * instrument sends to it. `fx_active` caches that decision, so the render
 * loop takes the dry path without touching the reverb.
 */

#include <stdlib.h>
#include <string.h>

#include "mp7_internal.h"

#define POPS_FX_FULL ((int32_t)0x7FFFFFFF) /* Q31 unity */

#if MP7_FX_REVERB

/* The damping one-pole coefficient is derived from the Damping parameter. */
#define POPS_FX_DAMP_BASE_Q16 49152 /* 0.75 */
#define POPS_FX_DAMP_RANGE_Q16 39322 /* 0.60 */

/* The room size scales the comb lines from 0.6x to 2.0x. */
#define POPS_FX_ROOM_BASE_Q16 39321
#define POPS_FX_ROOM_RANGE_Q16 91750

/* Feedback of every comb line, 0.84 in Q31 (round half up). */
#define POPS_FX_FB_Q31 ((int32_t)1804886016)

/* Comb and allpass tuning at 44.1 kHz, in samples. */
static const uint16_t pops_fx_comb_base[POPS_REVERB_COMBS] = {
    1116u, 1188u, 1277u, 1356u, 1422u, 1491u, 1557u, 1617u
};

static const uint16_t pops_fx_allpass_base[POPS_REVERB_ALLPASSES] = {
    556u, 441u, 341u, 225u
};

/* Saturate an int64 into int32. */
static int32_t pops_fx_sat_i32(int64_t v)
{
    if (v > (int64_t)INT32_MAX) {
        return INT32_MAX;
    }
    if (v < (int64_t)INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)v;
}

/* Q31 x Q31 -> Q31, rounded and saturated. */
static int32_t pops_fx_mul_q31(int32_t a, int32_t b)
{
    int64_t p = (int64_t)a * (int64_t)b + ((int64_t)1 << 30);

    return pops_fx_sat_i32(p >> 31);
}

/* Saturate an int32 into int16. */
static int16_t pops_fx_clip_i32_to_i16(int32_t v)
{
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return (int16_t)v;
}

/* Q16 control value in 0..1 -> Q31, clamping the top to unity. */
static int32_t pops_fx_q16_to_q31_full(int32_t v)
{
    const int64_t t = (int64_t)v << 15;

    if (t > (int64_t)POPS_FX_FULL) {
        return POPS_FX_FULL;
    }
    if (t < 0) {
        return 0;
    }
    return (int32_t)t;
}

static uint32_t pops_fx_comb_alloc(uint32_t base)
{
    return (uint32_t)(((uint64_t)base * 2u * 48000u + 44100u - 1u) / 44100u);
}

static uint32_t pops_fx_allpass_alloc(uint32_t base)
{
    return (uint32_t)(((uint64_t)base * 48000u + 44100u - 1u) / 44100u);
}

int pops_reverb_init(struct pops_reverb *reverb)
{
    size_t total = 0u;
    int ch;
    int line;
    int16_t *cursor;

    memset(reverb, 0, sizeof(*reverb));

    /* The two channels have the same line lengths, so each line's size is
     * counted twice instead of walking the per-channel tables. */
    for (line = 0; line < POPS_REVERB_COMBS; line++) {
        total += 2u * (size_t)pops_fx_comb_alloc(pops_fx_comb_base[line]);
    }
    for (line = 0; line < POPS_REVERB_ALLPASSES; line++) {
        total += 2u * (size_t)pops_fx_allpass_alloc(pops_fx_allpass_base[line]);
    }

    reverb->block = (int16_t *)calloc(total, sizeof(int16_t));
    if (reverb->block == 0) {
        return -1;
    }
    reverb->block_size = total * sizeof(int16_t);

    cursor = reverb->block;
    for (ch = 0; ch < 2; ch++) {
        for (line = 0; line < POPS_REVERB_COMBS; line++) {
            struct pops_reverb_comb *c = &reverb->comb[ch][line];

            c->alloc = pops_fx_comb_alloc(pops_fx_comb_base[line]);
            c->pos = 0u;
            c->lp = 0;
            c->line = cursor;
            cursor += c->alloc;
        }
    }
    for (ch = 0; ch < 2; ch++) {
        for (line = 0; line < POPS_REVERB_ALLPASSES; line++) {
            struct pops_reverb_allpass *a = &reverb->allpass[ch][line];

            a->alloc = pops_fx_allpass_alloc(pops_fx_allpass_base[line]);
            a->pos = 0u;
            a->line = cursor;
            cursor += a->alloc;
        }
    }

    return 0;
}

void pops_reverb_free(struct pops_reverb *reverb)
{
    free(reverb->block);
    reverb->block = 0;
    reverb->block_size = 0u;
}

void pops_reverb_configure(struct pops_reverb *reverb,
                            uint32_t sample_rate,
                            int32_t room_q16,
                            int32_t damping_q16,
                            int32_t width_q16,
                            int32_t wet_q16)
{
    int32_t scale_q16;
    int ch;
    int line;

    if (reverb->block == 0) {
        return;
    }

    /* The room size scales the comb lines from 0.6x to 2.0x. The lengths
     * depend on the line index only, so they are held once per line rather
     * than once per line and channel. */
    scale_q16 = POPS_FX_ROOM_BASE_Q16 +
        (int32_t)(((int64_t)room_q16 * POPS_FX_ROOM_RANGE_Q16) >> 16);
    for (line = 0; line < POPS_REVERB_COMBS; line++) {
        int64_t len = (int64_t)pops_fx_comb_base[line] * scale_q16
            * (int64_t)sample_rate;

        len /= (65536LL * 44100LL);
        if (len < 16) {
            len = 16;
        }
        if (len > (int64_t)reverb->comb[0][line].alloc) {
            len = (int64_t)reverb->comb[0][line].alloc;
        }
        reverb->comb_len[line] = (uint32_t)len;
    }
    for (line = 0; line < POPS_REVERB_ALLPASSES; line++) {
        int64_t len = (int64_t)pops_fx_allpass_base[line]
            * (int64_t)sample_rate / 44100LL;

        if (len < 16) {
            len = 16;
        }
        if (len > (int64_t)reverb->allpass[0][line].alloc) {
            len = (int64_t)reverb->allpass[0][line].alloc;
        }
        reverb->allpass_len[line] = (uint32_t)len;
        for (ch = 0; ch < 2; ch++) {
            if (reverb->allpass[ch][line].pos >= reverb->allpass_len[line]) {
                reverb->allpass[ch][line].pos = 0u;
            }
        }
    }

    /* The damping coefficient is the one-pole's share of the new sample:
     * the larger it is, the closer the filter follows the comb's output
     * and the brighter the tail stays. The parameter therefore runs the
     * other way round: damping 0 is the bright tail, damping 1 the dark
     * one. */
    reverb->damp_q16 = POPS_FX_DAMP_BASE_Q16 -
        (int32_t)(((int64_t)damping_q16 * POPS_FX_DAMP_RANGE_Q16) >> 16);

    reverb->width_q31 = pops_fx_q16_to_q31_full(width_q16);
    reverb->wet_q31 = pops_fx_q16_to_q31_full(wet_q16);
}

void pops_reverb_clear(struct pops_reverb *reverb)
{
    int ch;
    int line;

    if (reverb->block != 0) {
        memset(reverb->block, 0, reverb->block_size);
    }
    for (ch = 0; ch < 2; ch++) {
        for (line = 0; line < POPS_REVERB_COMBS; line++) {
            reverb->comb[ch][line].pos = 0u;
            reverb->comb[ch][line].lp = 0;
        }
        for (line = 0; line < POPS_REVERB_ALLPASSES; line++) {
            reverb->allpass[ch][line].pos = 0u;
        }
    }
}

/* The reverb of one channel: the damped comb lines summed into the series
 * allpass diffusers. */
static int32_t pops_fx_reverb_channel(struct pops_reverb *reverb, int ch,
                                     int32_t in)
{
    int64_t acc = 0;
    int32_t x;
    int line;

    for (line = 0; line < POPS_REVERB_COMBS; line++) {
        struct pops_reverb_comb *c = &reverb->comb[ch][line];
        uint32_t len = reverb->comb_len[line];
        uint32_t wp = c->pos;
        uint32_t rp = (wp >= len) ? wp - len : wp + c->alloc - len;
        int32_t y = (int32_t)c->line[rp] * 65536;
        int32_t fb;
        int64_t w;

        c->lp += (int32_t)((((int64_t)y - c->lp) * reverb->damp_q16) >> 16);
        fb = (int32_t)(((int64_t)c->lp * POPS_FX_FB_Q31) >> 31);
        w = (int64_t)in + (int64_t)fb;
        c->line[wp] = pops_fx_clip_i32_to_i16(pops_fx_sat_i32(w >> 16));

        wp++;
        if (wp >= c->alloc) {
            wp = 0u;
        }
        c->pos = wp;
        acc += y;
    }

    x = pops_fx_sat_i32(acc >> 3);

    for (line = 0; line < POPS_REVERB_ALLPASSES; line++) {
        struct pops_reverb_allpass *a = &reverb->allpass[ch][line];
        uint32_t wp = a->pos;
        int32_t v_old = (int32_t)a->line[wp] * 65536;
        int32_t v = pops_fx_sat_i32((int64_t)x + (v_old >> 1));
        int64_t next = (int64_t)v_old - (v >> 1);

        a->line[wp] = pops_fx_clip_i32_to_i16(v >> 16);
        wp++;
        if (wp >= reverb->allpass_len[line]) {
            wp = 0u;
        }
        a->pos = wp;
        x = pops_fx_sat_i32(next);
    }

    return x;
}

void pops_reverb_process(struct pops_reverb *reverb,
                          int64_t in_l,
                          int64_t in_r,
                          int32_t *out_l,
                          int32_t *out_r)
{
    int32_t il;
    int32_t ir;
    int32_t al;
    int32_t ar;
    int64_t mid;
    int32_t diff;
    int32_t side;
    int32_t wl;
    int32_t wr;

    if (reverb->block == 0) {
        *out_l = 0;
        *out_r = 0;
        return;
    }

    il = pops_fx_sat_i32(in_l);
    ir = pops_fx_sat_i32(in_r);

    al = pops_fx_reverb_channel(reverb, 0, il);
    ar = pops_fx_reverb_channel(reverb, 1, ir);

    mid = ((int64_t)al + (int64_t)ar) >> 1;
    diff = pops_fx_sat_i32((int64_t)al - (int64_t)ar);
    side = pops_fx_mul_q31(diff, reverb->width_q31) >> 1;
    wl = pops_fx_sat_i32(mid + (int64_t)side);
    wr = pops_fx_sat_i32(mid - (int64_t)side);

    *out_l = pops_fx_mul_q31(wl, reverb->wet_q31);
    *out_r = pops_fx_mul_q31(wr, reverb->wet_q31);
}

#endif /* MP7_FX_REVERB */

/* The reverb parameters are fixed at build time, so there is no parameter
 * table and no run-time setter. mp7_fx_defaults() only clears the sends,
 * which are the per-instrument control. */
void mp7_fx_defaults(struct mp7_context_s *ctx)
{
    unsigned int i;

    for (i = 0u; i < (unsigned int)POPS_INSTR_COUNT; i++) {
        ctx->instr_fx_send_q16[i] = 0;
    }
    ctx->fx_active = 0u;
}

void mp7_fx_apply(struct mp7_context_s *ctx)
{
#if MP7_FX_REVERB
    unsigned int i;
    int any_send = 0;

    pops_reverb_configure(&ctx->reverb,
                           ctx->sample_rate,
                           MP7_FX_ROOM_SIZE_Q16,
                           MP7_FX_DAMPING_Q16,
                           MP7_FX_WIDTH_Q16,
                           MP7_FX_LEVEL_Q16);

    if ((ctx->reverb.block != 0) && (MP7_FX_LEVEL_Q16 > 0)) {
        for (i = 0u; i < (unsigned int)POPS_INSTR_COUNT; i++) {
            if (ctx->instr_fx_send_q16[i] > 0) {
                any_send = 1;
                break;
            }
        }
    }

    ctx->fx_active = (uint8_t)any_send;
#else
    ctx->fx_active = 0u;
#endif
}

void mp7_fx_init(struct mp7_context_s *ctx)
{
    mp7_fx_defaults(ctx);
    mp7_fx_apply(ctx);
}

void mp7_fx_free(struct mp7_context_s *ctx)
{
#if MP7_FX_REVERB
    pops_reverb_free(&ctx->reverb);
#else
    (void)ctx;
#endif
}

void pops_set_instr_fx_send(pops_context ctx, pops_instr instr,
                           int32_t send_q16)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;

    if ((c == 0) || ((unsigned int)instr >= (unsigned int)POPS_INSTR_COUNT)) {
        return;
    }

    c->instr_fx_send_q16[instr] =
        pops_clamp_i32(send_q16, 0, POPS_Q16_ONE);
    mp7_fx_apply(c);
}

int32_t pops_get_instr_fx_send(const void *ctx, pops_instr instr)
{
    const struct mp7_context_s *c = (const struct mp7_context_s *)ctx;

    if ((c == 0) || ((unsigned int)instr >= (unsigned int)POPS_INSTR_COUNT)) {
        return 0;
    }

    return c->instr_fx_send_q16[instr];
}
