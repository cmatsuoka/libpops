/*
 * mp7_voice.c - per-instrument state and fixed parameter constants.
 *
 * The panel does not expose synthesis parameters to the user, so the
 * values below are compiled in. Frequencies are in Hz, times in microseconds,
 * and resonance in Q8.8. Level and Pan are fixed here as well, but they are
 * mixer inputs rather than synthesis parameters: the mixer applies them
 * together with the panel's Volume, Balance and per-instrument gains.
 */

#include <string.h>

#include "mp7_internal.h"

struct mp7_voice_config {
    uint8_t engine;
    uint16_t freq_hz;
    uint16_t freq2_hz;
    uint16_t rate_hz;
    uint16_t highpass_hz;
    uint32_t attack_us;
    uint32_t decay_us;
    uint32_t decay2_us;
    uint32_t density_us;
    uint16_t res_q88;
    int32_t level2_q16;
    int32_t tick_q16;
};

/* Short names for the cut-offs the extended build exposes, so the table
 * below stays in columns. The values themselves live in mp7_internal.h,
 * so the table and the defaults cannot drift apart. */
#define SNARE1_BP MP7_SNARE1_BANDPASS_HZ
#define SNARE2_BP MP7_SNARE2_BANDPASS_HZ
#define HIHAT_BP  MP7_HIHAT_BANDPASS_HZ
#define HIHAT_HP  MP7_HIHAT_HIGHPASS_HZ

static const struct mp7_voice_config voice_config[POPS_INSTR_COUNT] = {
    /* Bass */
    { MP7_ENGINE_SINE_AD  ,       65u,    0u,  0u,       0u,  1000u, 200000u,    0u,    0u,    0u,     0,     0 },
    /* Snare 1 */
    { MP7_ENGINE_NOISE    , SNARE1_BP,    0u,  0u,     800u,  1000u, 400000u,    0u,    0u,  102u,     0, 32768 },
    /* Snare 2 */
    { MP7_ENGINE_NOISE    , SNARE2_BP,    0u,  0u,    1400u,  1000u,  80000u,    0u,    0u,   77u,     0, 13107 },
    /* Small Bongo */
    { MP7_ENGINE_SINE_AD  ,      600u,    0u,  0u,       0u,  1000u,  50000u,    0u,    0u,    0u,     0,     0 },
    /* Large Bongo */
    { MP7_ENGINE_SINE_AD  ,      400u,    0u,  0u,       0u,  1000u,  50000u,    0u,    0u,    0u,     0,     0 },
    /* Conga */
    { MP7_ENGINE_DUAL_TONE,      195u, 4600u,  0u,       0u,  1000u, 250000u, 4000u,    0u,    0u, 65536,     0 },
    /* Cowbell */
    { MP7_ENGINE_DUAL_TONE,      860u, 4600u,  0u,       0u,  1000u,  50000u, 4000u,    0u,    0u, 65536,     0 },
    /* Claves */
    { MP7_ENGINE_SINE_AD  ,     2120u,    0u,  0u,       0u,  1000u,   4000u,    0u,    0u,    0u,     0,     0 },
    /* Hihat */
    { MP7_ENGINE_NOISE    ,  HIHAT_BP,    0u,  0u, HIHAT_HP, 10000u,  20000u,    0u,    0u,  512u,     0,     0 },
    /* Rimshot */
    { MP7_ENGINE_SINE_AD  ,     1650u,    0u,  0u,       0u,  1000u,   1000u,    0u,    0u,    0u,     0,     0 },
    /* Cymbal 1 */
    { MP7_ENGINE_NOISE    ,     5600u,    0u,  0u,    5500u,  1000u, 200000u,    0u,    0u,  154u,     0,     0 },
    /* Cymbal 2 */
    { MP7_ENGINE_NOISE    ,     5600u,    0u,  0u,    5500u,  1000u, 200000u,    0u,    0u,   77u,     0,     0 },
    /* Tambourine */
    { MP7_ENGINE_NOISE    ,     4600u,    0u,  0u,    6600u, 20000u,  80000u,    0u,    0u, 2560u,     0,     0 },
    /* Guiro */
    { MP7_ENGINE_GUIRO    ,     4100u,    0u, 65u,       0u,  1000u,   4000u,    0u,  600u,    0u,     0,     0 },
    /* Quijada */
    { MP7_ENGINE_QUIJADA  ,     2700u,    0u, 25u,       0u, 50000u, 300000u,    0u, 3600u,    0u,     0,     0 }
};

#ifdef LIBPOPS_EXTRA
/* What the extended build exposes for each instrument.
 *
 * The cut-off is the band-pass center of the instrument, so the Hihat's
 * band-pass is the one that moves; its high-pass stays at the factory
 * value under the cut-off, and its band-pass cannot go below that
 * high-pass. The high-pass does follow the tuning multiplier, so both
 * noise filters scale together. */
const struct mp7_instr_param mp7_instr_params[POPS_INSTR_COUNT] = {
    /* Bass */
    { 1, 0, 3u, 0u, 0u, 0u },
    /* Snare 1 */
    { 1, 1, 0u, MP7_CUTOFF_MIN_HZ, MP7_CUTOFF_MAX_HZ, MP7_SNARE1_BANDPASS_HZ },
    /* Snare 2 */
    { 1, 1, 1u, MP7_CUTOFF_MIN_HZ, MP7_CUTOFF_MAX_HZ, MP7_SNARE2_BANDPASS_HZ },
    /* Small Bongo */
    { 1, 0, 4u, 0u, 0u, 0u },
    /* Large Bongo */
    { 1, 0, 5u, 0u, 0u, 0u },
    /* Conga */
    { 1, 0, 6u, 0u, 0u, 0u },
    /* Cowbell */
    { 1, 0, 7u, 0u, 0u, 0u },
    /* Claves */
    { 1, 0, 8u, 0u, 0u, 0u },
    /* Hihat */
    { 1, 1, 2u, MP7_HIHAT_CUTOFF_MIN_HZ, MP7_CUTOFF_MAX_HZ, MP7_HIHAT_BANDPASS_HZ },
    /* Rimshot */
    { 1, 0, 9u, 0u, 0u, 0u },
    /* Cymbal 1 */
    { 1, 0, 10u, 0u, 0u, 0u },
    /* Cymbal 2 */
    { 1, 0, 11u, 0u, 0u, 0u },
    /* Tambourine */
    { 1, 0, 12u, 0u, 0u, 0u },
    /* Guiro: the length of the instrument is the gate the rhythm holds
     * open, not a decay, so it carries no decay multiplier. */
    { 0, 0, MP7_PARAM_SLOT_NONE, 0u, 0u, 0u },
    /* Quijada */
    { 1, 0, 13u, 0u, 0u, 0u }
};
#endif /* LIBPOPS_EXTRA */

static void env_set(struct pops_env *env,
                    uint32_t attack_us,
                    uint32_t decay_us,
                    uint32_t sample_rate)
{
    memset(env, 0, sizeof(*env));
    env->attack_coef = pops_env_coef_us(attack_us, sample_rate);
    env->decay_coef = pops_env_coef_us(decay_us, sample_rate);
    env->stage = POPS_ENV_IDLE;
}

static void voice_setup(struct mp7_voice *voice,
                        pops_instr instr,
                        const struct mp7_voice_config *config,
                        uint32_t sample_rate)
{
    uint32_t seed;

    memset(voice, 0, sizeof(*voice));
    voice->instr = instr;
    voice->engine = config->engine;

    voice->level_q16 = POPS_Q16_ONE;
    voice->pan_q16 = POPS_Q16_ONE / 2;
    voice->balance_group = POPS_MIX_GROUP_NONE;
#ifdef LIBPOPS_EXTRA
    voice->accent_gain_q16 = POPS_Q16_ONE;
#endif

    switch (instr) {
    case POPS_INSTR_BASS:
        voice->balance_group = POPS_MIX_GROUP_A;
        break;
    case POPS_INSTR_HIHAT:
    case POPS_INSTR_CYMBAL_1:
    case POPS_INSTR_CYMBAL_2:
        voice->balance_group = POPS_MIX_GROUP_B;
        break;
    default:
        break;
    }

    seed = 0x9E3779B9u + (uint32_t)instr * 0x85EBCA6Bu;
    voice->prng = (seed != 0u) ? seed : 1u;

    pops_osc_set_freq(&voice->body_osc, config->freq_hz, sample_rate);

    switch (config->engine) {
    case MP7_ENGINE_SINE_AD:
        env_set(&voice->amp_env, config->attack_us, config->decay_us, sample_rate);
        break;

    case MP7_ENGINE_DUAL_TONE:
        pops_osc_set_freq(&voice->aux_osc, config->freq2_hz, sample_rate);
        env_set(&voice->amp_env, config->attack_us, config->decay_us, sample_rate);
        env_set(&voice->aux_env, config->attack_us, config->decay2_us, sample_rate);
        voice->level2_q16 = config->level2_q16;
        break;

    case MP7_ENGINE_NOISE:
        env_set(&voice->amp_env, config->attack_us, config->decay_us, sample_rate);
        env_set(&voice->aux_env, config->attack_us, config->decay_us, sample_rate);
        pops_biquad_set_highpass(&voice->highpass, config->highpass_hz, sample_rate);
        pops_biquad_set_bandpass(&voice->bandpass,
                                  config->freq_hz,
                                  config->res_q88,
                                  sample_rate);
        voice->tick_q30 = (int32_t)((int64_t)config->tick_q16 * 16384);
        break;

    case MP7_ENGINE_GUIRO:
        pops_osc_set_freq(&voice->aux_osc, config->rate_hz, sample_rate);
        env_set(&voice->amp_env, config->attack_us, config->density_us, sample_rate);
        env_set(&voice->body_env, 1000u, config->decay_us, sample_rate);
        break;

    case MP7_ENGINE_QUIJADA:
        /* Rate drives the comparator clock; Frequency drives the audible
         * body oscillator (algorithm page, Quijada engine). */
        pops_osc_set_freq(&voice->body_osc, config->freq_hz, sample_rate);
        pops_osc_set_freq(&voice->aux_osc, config->rate_hz, sample_rate);
        env_set(&voice->amp_env, config->attack_us, config->decay_us, sample_rate);
        env_set(&voice->short_env, 100u, config->density_us, sample_rate);
        env_set(&voice->long_env, 100u, config->density_us, sample_rate);
        break;

    default:
        break;
    }
}

int32_t mp7_voice_gain_q16(const struct mp7_context_s *ctx,
                           const struct mp7_voice *voice)
{
    int32_t gain = voice->level_q16;

    gain = pops_mul_q16(gain, pops_mixer_balance_gain_q16(&ctx->mixer,
                                                             voice->balance_group));
    gain = pops_mul_q16(gain, ctx->mixer.instr_gain_q16[voice->instr]);
#ifdef LIBPOPS_EXTRA
    /* The accent of the note that is sounding, last, so that it is the
     * same multiply the per-sample path used to do. */
    gain = pops_mul_q16(gain, voice->accent_gain_q16);
#endif

    return gain;
}

void mp7_voice_refresh_gain(struct mp7_context_s *ctx, pops_instr instr)
{
    ctx->voice_gain_q16[instr] = mp7_voice_gain_q16(ctx, &ctx->voices[instr]);
}

#ifdef LIBPOPS_EXTRA
/* The cut-off the filters are actually tuned to.
 *
 * A biquad is only stable below Nyquist, and the design wraps around at
 * half the sample rate, where the poles reach the unit circle and the
 * filter rings without decaying. The requested cut-off is therefore
 * limited to a fraction of the sample rate, which keeps the poles well
 * inside it at every rate. */
static uint32_t cutoff_limit_hz(uint32_t sample_rate)
{
    return (uint32_t)(((uint64_t)sample_rate * MP7_CUTOFF_RATE_NUM) /
                      MP7_CUTOFF_RATE_DEN);
}

/* Apply a new decay time to an envelope that may be sounding.
 *
 * env_set() would reset the envelope, which cuts a note that is
 * already ringing off with a click. Only the coefficients change here,
 * so the note keeps playing and simply decays at the new rate. */
static void env_retime(struct pops_env *env,
                       uint32_t attack_us,
                       uint32_t decay_us,
                       uint32_t sample_rate)
{
    env->attack_coef = pops_env_coef_us(attack_us, sample_rate);
    env->decay_coef = pops_env_coef_us(decay_us, sample_rate);

    /* The running coefficient is a copy of the attack or the decay
     * coefficient, taken when the segment started, so it has to follow
     * the new value or the note would keep the old rate. */
    if (env->stage == POPS_ENV_ATTACK) {
        env->coef = env->attack_coef;
    } else if (env->stage == POPS_ENV_DECAY) {
        env->coef = env->decay_coef;
    }
}

/* A decay time of an instrument, scaled by its decay multiplier.
 *
 * The multiplier is clamped to 0.2..5.0 by the setter, and the longest
 * factory decay is 400 ms, so the scaled time stays within two seconds
 * and fits in 32 bits with room to spare. */
static uint32_t scaled_decay_us(uint32_t decay_us, int32_t decay_q16)
{
    int64_t scaled = ((int64_t)decay_us * (int64_t)decay_q16 +
                      (POPS_Q16_ONE / 2)) / POPS_Q16_ONE;

    if (scaled < 0) {
        scaled = 0;
    }
    return (uint32_t)scaled;
}

/* The pitch of an instrument, scaled by its tuning multiplier.
 *
 * The multiplier is clamped to 0.5..2.0 by the setter, so the scaled
 * value never overflows: the factory frequencies are uint16_t and twice
 * their maximum fits comfortably in 32 bits. Rounding is half-up, like
 * the decay scaling, and 1.0 is the exact identity. */
static uint32_t scaled_pitch_hz(uint32_t freq_hz, int32_t tune_q16)
{
    return (uint32_t)(((int64_t)freq_hz * (int64_t)tune_q16 +
                       (POPS_Q16_ONE / 2)) / POPS_Q16_ONE);
}

/* Apply the decay multiplier of an instrument to its envelopes.
 *
 * The multiplier scales the factory decay time of every decay-bearing
 * envelope the voice has, so the voice keeps its shape at every
 * multiplier: the amplitude envelope of the sine, dual-tone, Quijada
 * and noise engines, the tone envelope of the noise engines, and the
 * second tone of the dual-tone engine, whose factory decay is the
 * shorter decay2. The Guiro carries no decay multiplier: the length of
 * the instrument is the gate the rhythm holds open, and its only
 * factory decay, the per-tooth body ring, is not the length of the
 * instrument. */
void mp7_voice_apply_decay(struct mp7_context_s *ctx, pops_instr instr)
{
    struct mp7_voice *voice = &ctx->voices[instr];
    const struct mp7_voice_config *config = &voice_config[instr];
    uint8_t slot = mp7_instr_params[instr].slot;
    uint32_t decay_us;
    uint32_t decay2_us;

    if (!mp7_instr_params[instr].has_decay) {
        return;
    }

    decay_us = scaled_decay_us(config->decay_us, ctx->instr_decay_q16[slot]);

    switch (voice->engine) {
    case MP7_ENGINE_SINE_AD:
        env_retime(&voice->amp_env, config->attack_us, decay_us,
                   ctx->sample_rate);
        break;

    case MP7_ENGINE_DUAL_TONE:
        decay2_us = scaled_decay_us(config->decay2_us,
                                    ctx->instr_decay_q16[slot]);
        env_retime(&voice->amp_env, config->attack_us, decay_us,
                   ctx->sample_rate);
        env_retime(&voice->aux_env, config->attack_us, decay2_us,
                   ctx->sample_rate);
        break;

    case MP7_ENGINE_NOISE:
        env_retime(&voice->amp_env, config->attack_us, decay_us,
                   ctx->sample_rate);
        env_retime(&voice->aux_env, config->attack_us, decay_us,
                   ctx->sample_rate);
        break;

    case MP7_ENGINE_QUIJADA:
        env_retime(&voice->amp_env, config->attack_us, decay_us,
                   ctx->sample_rate);
        break;

    default:
        break;
    }
}

/* The band-pass centre a noise voice is tuned to before the tuning
 * multiplier: the stored cut-off of the three instruments that expose
 * one, the factory value of the voice table for the Cymbal and the
 * Tambourine, whose cut-off the user cannot set. */
static uint32_t noise_cutoff_hz(const struct mp7_context_s *ctx,
                                const struct mp7_voice_config *config,
                                pops_instr instr)
{
    if (mp7_instr_params[instr].has_cutoff) {
        return ctx->instr_cutoff_hz[mp7_instr_params[instr].slot];
    }
    return config->freq_hz;
}

/* Retune the band-pass of a noise voice.
 *
 * The effective centre is the stored cut-off scaled by the instrument's
 * tuning multiplier, limited to the fraction of the sample rate a
 * biquad is stable at. The coefficients are written without touching the
 * filter state, so a sounding note follows the new centre without a
 * discontinuity. */
static void noise_bandpass_set(struct mp7_context_s *ctx, pops_instr instr)
{
    struct mp7_voice *voice = &ctx->voices[instr];
    const struct mp7_voice_config *config = &voice_config[instr];
    uint32_t freq_hz;
    uint32_t limit;

    freq_hz = scaled_pitch_hz(noise_cutoff_hz(ctx, config, instr),
                              ctx->instr_tune_q16[instr]);
    limit = cutoff_limit_hz(ctx->sample_rate);
    if (freq_hz > limit) {
        freq_hz = limit;
    }

    pops_biquad_set_bandpass_coeffs(&voice->bandpass,
                                     freq_hz,
                                     config->res_q88,
                                     ctx->sample_rate);
}

/* Retune the high-pass of a noise voice.
 *
 * The high-pass is a property of the voice itself, not of the panel or
 * of the extended parameters: the tuning multiplier moves it together
 * with the band-pass, so the voice transposes whole and keeps its
 * spectral shape at every setting. The factory high-passes of the
 * Cymbal and the Tambourine sit at or above the factory band-pass, so a
 * fixed high-pass would take the scaled band-pass below it and starve
 * the voice of output as soon as it tuned down. The high-pass follows
 * the same sample-rate limit the band-pass does. */
static void noise_highpass_set(struct mp7_context_s *ctx, pops_instr instr)
{
    struct mp7_voice *voice = &ctx->voices[instr];
    const struct mp7_voice_config *config = &voice_config[instr];
    uint32_t freq_hz;
    uint32_t limit;

    freq_hz = scaled_pitch_hz(config->highpass_hz,
                              ctx->instr_tune_q16[instr]);
    limit = cutoff_limit_hz(ctx->sample_rate);
    if (freq_hz > limit) {
        freq_hz = limit;
    }

    pops_biquad_set_highpass_coeffs(&voice->highpass,
                                    freq_hz,
                                    ctx->sample_rate);
}

/* Apply the cut-off of an instrument to its band-pass.
 *
 * The cut-off is the band-pass center of every instrument that has one,
 * and the band-pass is the only filter the cut-off moves: the noise
 * high-pass follows the tuning multiplier alone, never the cut-off. The
 * tuning multiplier composes with the cut-off, so the cut-off is the
 * centre of the instrument at its factory pitch, not the centre the
 * filter actually plays. */
void mp7_voice_apply_cutoff(struct mp7_context_s *ctx, pops_instr instr)
{
    if (!mp7_instr_params[instr].has_cutoff) {
        return;
    }

    noise_bandpass_set(ctx, instr);
}

/* Apply the tuning multiplier of an instrument to its voice.
 *
 * The multiplier scales the pitched elements of the voice: the body
 * oscillator of every pitched engine, the second tone of the dual-tone
 * engine as well, and both filters of the noise engines - the band-pass
 * centre, composed with the stored cut-off the way
 * mp7_voice_apply_cutoff() is, and the high-pass, which follows the
 * multiplier alone. The Guiro's ratchet and the Quijada's click clock
 * are timing, not pitch, and stay at the factory rate.
 *
 * The oscillators and the filter coefficients are recomputed without
 * resetting any envelope or filter state, so a sounding note follows the
 * new pitch without a discontinuity. */
void mp7_voice_apply_tune(struct mp7_context_s *ctx, pops_instr instr)
{
    struct mp7_voice *voice = &ctx->voices[instr];
    const struct mp7_voice_config *config = &voice_config[instr];
    int32_t tune_q16 = ctx->instr_tune_q16[instr];

    pops_osc_set_freq(&voice->body_osc,
                      scaled_pitch_hz(config->freq_hz, tune_q16),
                      ctx->sample_rate);

    switch (voice->engine) {
    case MP7_ENGINE_DUAL_TONE:
        pops_osc_set_freq(&voice->aux_osc,
                          scaled_pitch_hz(config->freq2_hz, tune_q16),
                          ctx->sample_rate);
        break;

    case MP7_ENGINE_NOISE:
        noise_highpass_set(ctx, instr);
        noise_bandpass_set(ctx, instr);
        break;

    default:
        break;
    }
}
#endif /* LIBPOPS_EXTRA */

void mp7_voice_init_all(struct mp7_context_s *ctx)
{
    unsigned int i;

    /* voice_setup() resets the whole voice, so it also resets the fixed
     * Level and Pan. The Level is a constant today; if it ever becomes a
     * per-voice setting, a sample-rate change must not clear it. The Pan
     * comes from the per-instrument pan in the mixer, so it is re-applied
     * here. */
    for (i = 0; i < (unsigned int)POPS_INSTR_COUNT; i++) {
        voice_setup(&ctx->voices[i], (pops_instr)i, &voice_config[i], ctx->sample_rate);
        ctx->voices[i].pan_q16 = ctx->mixer.instr_pan_q16[i];
    }

#ifdef LIBPOPS_EXTRA
    /* Re-apply any user-modified synthesis parameters after the voices have
     * been reset to their factory defaults. */
    for (i = 0; i < (unsigned int)POPS_INSTR_COUNT; i++) {
        if (mp7_instr_params[i].has_decay) {
            mp7_voice_apply_decay(ctx, (pops_instr)i);
        }
        if (mp7_instr_params[i].has_cutoff) {
            mp7_voice_apply_cutoff(ctx, (pops_instr)i);
        }
    }

    /* The tuning of every instrument. The loop covers the Cymbal and the
     * Tambourine too, whose band-pass the cut-off setter cannot move, and
     * it runs after the cut-off loop so the band-pass carries the tuning. */
    for (i = 0; i < (unsigned int)POPS_INSTR_COUNT; i++) {
        mp7_voice_apply_tune(ctx, (pops_instr)i);
    }
#endif
}

void mp7_voice_trigger(struct mp7_voice *voice)
{
    uint32_t r1;
    uint32_t r2;
    int32_t t;

    voice->active = 1;

    switch (voice->engine) {
    case MP7_ENGINE_SINE_AD:
        pops_env_trigger(&voice->amp_env);
        break;

    case MP7_ENGINE_DUAL_TONE:
        pops_env_trigger(&voice->amp_env);
        pops_env_trigger(&voice->aux_env);
        break;

    case MP7_ENGINE_NOISE:
        pops_env_trigger(&voice->amp_env);
        pops_env_trigger(&voice->aux_env);
        voice->highpass.x1 = 0;
        voice->highpass.x2 = 0;
        voice->highpass.y1 = 0;
        voice->highpass.y2 = 0;
        voice->bandpass.x1 = 0;
        voice->bandpass.x2 = 0;
        voice->bandpass.y1 = 0;
        voice->bandpass.y2 = 0;
        break;

    case MP7_ENGINE_GUIRO:
        pops_env_trigger_hold(&voice->amp_env);
        voice->body_env.stage = POPS_ENV_IDLE;
        voice->body_env.value = 0;
        voice->last_rate = 0;
        break;

    case MP7_ENGINE_QUIJADA:
        pops_env_trigger(&voice->amp_env);
        /* The secondary envelopes are deliberately not reset: the trigger
         * leaves them running and only restarts the body envelope. */

        /* Both thresholds are re-randomized on every trigger:
         * t = 0.1 + fabs(u) * 0.5, with u uniform in [-1, 1). In Q30 that is
         * 0.1 * 2^30 + abs(u_q31) / 4. */
        r1 = pops_prng_next(&voice->prng);
        r2 = pops_prng_next(&voice->prng);
        t = POPS_Q30_ONE / 10;
        voice->threshold_lo = t + (pops_abs_i32((int32_t)r1) / 4);
        voice->threshold_hi = t + (pops_abs_i32((int32_t)r2) / 4);
        break;

    default:
        break;
    }
}

void mp7_voice_release(struct mp7_voice *voice)
{
    if ((voice->engine == MP7_ENGINE_GUIRO) && voice->active) {
        pops_env_release(&voice->amp_env, voice->amp_env.decay_coef);
    }
}

int64_t mp7_voice_process(struct mp7_voice *voice)
{
    if (!voice->active) {
        return 0;
    }

    /* The voice's Level, the pan split and the master gain are all applied
     * by the mixer, so the engine returns the raw voice signal. */
    return mp7_engine_process(voice);
}
