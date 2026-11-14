/*
 * main.c - test harness for the Mini Pops library.
 *
 * Commands:
 *   --init              report the context size, the default sample rate and
 *                       the sine table footprint
 *   --sine              verify the shared sine table against
 *                       sin(2*pi*i/4096) at all 4096 indices
 *   --gain              verify the Q15 gain multiply, including gains above
 *                       unity
 *   --silence [path]    render silence (no rhythm selected), check it, and
 *                       write a WAV file
 *   --instruments [dir] render all 15 instruments to WAV files
 *   --noise             verify the 6 noise voices are uncorrelated
 *   --guiro             verify Guiro gate hold and note-off release
 *   --quijada           verify Quijada frequencies, thresholds and comparator
 *   --mixer             verify pan, Volume, Balance, instrument gains and
 *                       instrument pans
 *   --api               verify the public API: triggers, rhythm selection,
 *                       tempo and mix controls
 *   --panel             verify the panel state snapshot and lamp flash counter
 *   --pattern           verify the 20 patterns, combination and timing
 *   --rhythms [dir]     render all 20 rhythms and two combinations to WAV
 *   --user              verify the user pattern slots and the pattern source
 *                       switch (an extended build, -DLIBPOPS_EXTRA, only)
 *   --help              this text
 *
 * Exit status is 0 when every check passed, 1 otherwise.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pops.h"
#include "mp7_fixed.h"
#include "mp7_internal.h"
#include "wav_writer.h"

#ifndef TEST_PI
#define TEST_PI 3.14159265358979323846
#endif

#define TEST_SILENCE_SECONDS 0.25

static const char *const test_rhythm_names[POPS_RHYTHM_COUNT] = {
    "Waltz",
    "Samba",
    "Mambo",
    "Rumba",
    "Tango",
    "Bossanova",
    "Rock 1",
    "Rock 3",
    "March 2/4",
    "Swing",
    "Jazz Waltz",
    "Meringue",
    "Cha-Cha",
    "Beguine",
    "Habanera",
    "Slow Rock",
    "Rock 2",
    "Rock 4",
    "March 6/8",
    "Latin Swing",
};

static int failures = 0;

static void fail(const char *what)
{
    printf("FAIL: %s\n", what);
    failures++;
}

static void ok(const char *what)
{
    printf("ok  : %s\n", what);
}

static struct mp7_context_s *context_new(void)
{
    struct mp7_context_s *ctx =
        (struct mp7_context_s *)malloc(sizeof(struct mp7_context_s));

    if (ctx == 0) {
        printf("FAIL: out of memory\n");
        exit(1);
    }
    return ctx;
}

/* Drive the sample-clocked sequencer until `beats` beats have elapsed and
 * return the tick index at which the last of them starts. Every visited
 * column costs one column length, so the returned count is the exact number
 * of samples the sequencer spent on those beats: `columns_per_beat` is 8 on
 * the straight grid and 6 on the compound one. */
static uint64_t run_beats(struct mp7_context_s *ctx,
                          unsigned int beats,
                          unsigned int columns_per_beat)
{
    const unsigned int boundaries = beats * columns_per_beat;
    unsigned int seen = 0u;
    uint8_t previous = ctx->sequencer.column;
    uint64_t t = 0u;

    for (;;) {
        mp7_pattern_tick(ctx);
        if (ctx->sequencer.column != previous) {
            /* The tick that moves the playhead fires the new column, so its
             * index is the sample the beats add up to. */
            previous = ctx->sequencer.column;
            seen++;
            if (seen >= boundaries) {
                return t;
            }
        }
        t++;
    }
}

static int command_init(void)
{
    struct mp7_context_s *ctx = context_new();

    printf("context size        : %lu bytes\n",
           (unsigned long)sizeof(struct mp7_context_s));
    printf("default sample rate : %u Hz\n", (unsigned)POPS_DEFAULT_SAMPLE_RATE);

    mp7_init(ctx, 0);
    if (ctx->sample_rate != POPS_DEFAULT_SAMPLE_RATE) {
        fail("mp7_init(ctx, 0) did not select the default sample rate");
    } else {
        ok("mp7_init(ctx, 0) selects the default sample rate");
    }

    pops_set_sample_rate(ctx, 22050);
    if (ctx->sample_rate != 22050u) {
        fail("pops_set_sample_rate() did not take effect");
    } else {
        ok("pops_set_sample_rate() takes effect");
    }

    printf("sine quadrant       : %u entries, %lu bytes\n",
            (unsigned)POPS_SINE_QUAD_SIZE,
           (unsigned long)(POPS_SINE_QUAD_SIZE * sizeof(int16_t)));
    printf("sine table (shared) : %u entries, %lu bytes\n",
            (unsigned)POPS_SINE_TABLE_SIZE,
           (unsigned long)(POPS_SINE_TABLE_SIZE * sizeof(int16_t)));

    free(ctx);
    return failures == 0 ? 0 : 1;
}

static int command_sine(void)
{
    struct mp7_context_s *ctx = context_new();
    unsigned int i;
    int mismatches = 0;
    int worst = 0;
    int fallback_mismatches = 0;

    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);

    if (pops_sine == 0) {
        fail("mp7_init() did not rebuild the sine table");
        free(ctx);
        return 1;
    }

    for (i = 0; i < POPS_SINE_TABLE_SIZE; i++) {
        double exact = 32767.0 * sin(2.0 * TEST_PI * (double)i / (double)POPS_SINE_TABLE_SIZE);
        int expected = (int)floor(exact + 0.5);
         int got = (int)pops_sine[i];
        int error = got - expected;

        if (error < 0) {
            error = -error;
        }
        if (error > worst) {
            worst = error;
        }
        if (error != 0) {
            if (mismatches < 8) {
                printf("      i=%u: got %d, expected %d\n", i, got, expected);
            }
            mismatches++;
        }

         if ((int)pops_sine_at(i) != got) {
            fallback_mismatches++;
        }
    }

    if (mismatches != 0) {
        printf("FAIL: %d of %u rebuilt entries differ (worst %d LSB)\n",
               mismatches, (unsigned)POPS_SINE_TABLE_SIZE, worst);
        failures++;
    } else {
        ok("the rebuilt table matches sin(2*pi*i/1024) at all 1024 indices");
    }

    if (fallback_mismatches != 0) {
        fail("the quadrant fallback disagrees with the rebuilt table");
    } else {
        ok("the quadrant lookup agrees with the rebuilt table");
    }

    /* Only the first quadrant lives in the image; the full period is built
     * once, on the first mp7_init(), and shared by every context. */
    if ((POPS_SINE_QUAD_SIZE * sizeof(int16_t)) != 514u) {
        fail("the sine quadrant is not 514 bytes");
    } else {
        ok("the sine quadrant is 514 bytes (257 x int16)");
    }

    /* Interpolation quality: the oscillator uses the full 28-bit phase, so
     * check a dense set of fractional phases against libm. A whole table
     * index must still be exact. */
    {
        unsigned int phases = 4096u;
        int worst_interp = 0;

        for (i = 0u; i < phases; i++) {
            uint32_t phase = (uint32_t)((uint64_t)i << 16);
            double exact = 32767.0 * sin(2.0 * TEST_PI * (double)phase
                / 268435456.0);
            int expected = (int)floor(exact + 0.5);
             int got = (int)pops_sine_interp(phase);
            int error = got - expected;

            if (error < 0) {
                error = -error;
            }
            if (error > worst_interp) {
                worst_interp = error;
            }
        }
        if (worst_interp > 1) {
            printf("FAIL: sine interpolation is off by %d LSB\n",
                worst_interp);
            failures++;
        } else {
            printf("ok  : sine interpolation tracks sin() (worst %d LSB)\n",
                worst_interp);
        }
         if ((int)pops_sine_interp(256u << 18) != 32767) {
            fail("sine interpolation is not exact at the peak");
        } else {
            ok("sine interpolation is exact at whole table indices");
        }
    }

    {
         const char *table = (const char *)pops_sine;
        const char *ctx_start = (const char *)ctx;
        const char *ctx_end = ctx_start + sizeof(struct mp7_context_s);

        if ((table >= ctx_start) && (table < ctx_end)) {
            fail("the sine table lives inside the context");
        } else {
            ok("the sine table is shared, not stored in the context");
        }
    }

    /* The rebuilt table must not depend on the context. */
    if (ctx->sample_rate != POPS_DEFAULT_SAMPLE_RATE) {
        fail("the sample rate was not set");
    }

    pops_set_sample_rate(ctx, 22050);
    if (ctx->sample_rate != 22050u) {
        fail("pops_set_sample_rate() did not take effect");
    } else {
        ok("pops_set_sample_rate() takes effect");
    }

    free(ctx);
    return failures == 0 ? 0 : 1;
}

static int command_gain(void)
{
    /* Gain parameters are Q16 units: 1.0 is 65536. The interesting case is
     * a gain above unity, which a Q31 gain could not represent at all. */
    const int32_t unity = 65536;
    const int32_t max_level = 262144;   /* 4.0 */
    int64_t product;

    /* unity gain is the identity */
     product = pops_mul_q31_gain(2147483647, unity);
    if (product != 2147483647) {
        printf("FAIL: unity gain changed the sample (%lld)\n", (long long)product);
        failures++;
    } else {
        ok("unity gain leaves a Q31 sample unchanged");
    }

    /* half gain halves it */
     product = pops_mul_q31_gain(2147483647, unity / 2);
    if (product != 2147483647LL / 2) {
        printf("FAIL: half gain gave %lld\n", (long long)product);
        failures++;
    } else {
        ok("half gain halves a Q31 sample");
    }

    /* zero gain silences it */
     if (pops_mul_q31_gain(2147483647, 0) != 0) {
        fail("zero gain did not silence the sample");
    } else {
        ok("zero gain silences a Q31 sample");
    }

    /* a negative sample stays negative */
     if (pops_mul_q31_gain(-2147483647, unity) > 0) {
        fail("a negative sample changed sign");
    } else {
        ok("a negative sample keeps its sign");
    }

    /* 4.0 gain overflows a Q31 gain but must still be exact in 64 bits */
     product = pops_mul_q31_gain(2147483647, max_level);
    if (product != 2147483647LL * 4LL) {
        printf("FAIL: 4.0 gain gave %lld, expected %lld\n",
               (long long)product, 2147483647LL * 4LL);
        failures++;
    } else {
        ok("4.0 gain is exact in 64 bits (a Q31 gain could not hold it)");
    }

    return failures == 0 ? 0 : 1;
}

static int command_silence(const char *path)
{
    struct mp7_context_s *ctx = context_new();
    const uint32_t sample_rate = POPS_DEFAULT_SAMPLE_RATE;
    size_t frames = (size_t)((TEST_SILENCE_SECONDS * (double)sample_rate) + 0.5);
    int16_t *buffer = (int16_t *)malloc(frames * 2u * sizeof(int16_t));
    size_t i;
    int nonzero = 0;

    if (buffer == 0) {
        printf("FAIL: out of memory\n");
        free(ctx);
        return 1;
    }

    memset(buffer, 0xA5, frames * 2u * sizeof(int16_t));

    mp7_init(ctx, sample_rate);
    pops_process(ctx, buffer, frames);

    for (i = 0; i < frames * 2u; i++) {
        if (buffer[i] != 0) {
            nonzero++;
        }
    }

    if (nonzero != 0) {
        printf("FAIL: %d samples are not silent\n", nonzero);
        failures++;
    } else {
        ok("the renderer writes silence when no rhythm is selected");
    }

    if (wav_write_stereo16(path, buffer, frames, sample_rate) != 0) {
        printf("FAIL: could not write %s\n", path);
        failures++;
    } else {
        printf("ok  : wrote %s (%lu frames)\n", path, (unsigned long)frames);
    }

    free(buffer);
    free(ctx);
    return failures == 0 ? 0 : 1;
}


static void render_voice_i32(struct mp7_context_s *ctx,
                             pops_instr instr,
                             int32_t *out,
                             size_t frames,
                             size_t release_at)
{
    size_t i;

    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    mp7_voice_trigger(&ctx->voices[instr]);

    for (i = 0; i < frames; i++) {
        int64_t sample;

        if (i == release_at) {
            mp7_voice_release(&ctx->voices[instr]);
        }
        sample = mp7_voice_process(&ctx->voices[instr]);
         out[i] = pops_sat_i64_to_i32(sample);
    }
}

static int peak_i32(const int32_t *samples, size_t count)
{
    int peak = 0;
    size_t i;

    for (i = 0; i < count; i++) {
        int value = pops_abs_i32(samples[i]);

        if (value > peak) {
            peak = value;
        }
    }
    return peak;
}

static double rms_i32(const int32_t *samples, size_t count)
{
    double sum = 0.0;
    size_t i;

    for (i = 0; i < count; i++) {
        sum += (double)samples[i] * (double)samples[i];
    }
    return (count > 0u) ? sqrt(sum / (double)count) : 0.0;
}

static int command_instruments(const char *dir)
{
    static const pops_instr instruments[POPS_INSTR_COUNT] = {
        POPS_INSTR_BASS, POPS_INSTR_SNARE_1, POPS_INSTR_SNARE_2,
        POPS_INSTR_SMALL_BONGO, POPS_INSTR_LARGE_BONGO, POPS_INSTR_CONGA,
        POPS_INSTR_COWBELL, POPS_INSTR_CLAVES, POPS_INSTR_HIHAT,
        POPS_INSTR_RIMSHOT, POPS_INSTR_CYMBAL_1, POPS_INSTR_CYMBAL_2,
        POPS_INSTR_TAMBOURINE, POPS_INSTR_GUIRO, POPS_INSTR_QUIJADA
    };
    struct mp7_context_s *ctx = context_new();
    const uint32_t sample_rate = POPS_DEFAULT_SAMPLE_RATE;
    const size_t frames = (size_t)sample_rate;
    const size_t tail_frames = (size_t)(sample_rate / 20u);
    int32_t *mono = (int32_t *)malloc(frames * sizeof(int32_t));
    int16_t *stereo = (int16_t *)malloc(frames * 2u * sizeof(int16_t));
    unsigned int i;

    if ((mono == 0) || (stereo == 0)) {
        printf("FAIL: out of memory\n");
        free(mono);
        free(stereo);
        free(ctx);
        return 1;
    }

    for (i = 0; i < (unsigned int)POPS_INSTR_COUNT; i++) {
        char path[512];
        size_t release_at = frames;
        int peak;
        int tail;
        size_t j;

        if (instruments[i] == POPS_INSTR_GUIRO) {
            release_at = (size_t)(sample_rate / 5u);
        }

        render_voice_i32(ctx, instruments[i], mono, frames, release_at);
        peak = peak_i32(mono, frames);
        tail = peak_i32(mono + (frames - tail_frames), tail_frames);

        if (peak < 100) {
            printf("FAIL: instrument %u is silent (peak %d)\n", i, peak);
            failures++;
        }
        if (tail > 100) {
            printf("FAIL: instrument %u does not decay (tail %d)\n", i, tail);
            failures++;
        }

        for (j = 0; j < frames; j++) {
            int16_t value = (int16_t)pops_q31_to_q15(mono[j]);

            stereo[2u * j] = value;
            stereo[2u * j + 1u] = value;
        }

        if (snprintf(path, sizeof(path), "%s/instrument_%02u.wav", dir, i) >= (int)sizeof(path)) {
            printf("FAIL: path too long\n");
            failures++;
            continue;
        }
        if (wav_write_stereo16(path, stereo, frames, sample_rate) != 0) {
            printf("FAIL: could not write %s\n", path);
            failures++;
        }
    }

    if (failures == 0) {
        ok("all 15 instruments render a non-silent, decaying hit");
    }

    free(mono);
    free(stereo);
    free(ctx);
    return failures == 0 ? 0 : 1;
}

static double correlation(const int32_t *a, const int32_t *b, size_t count)
{
    double mean_a = 0.0;
    double mean_b = 0.0;
    double cov = 0.0;
    double var_a = 0.0;
    double var_b = 0.0;
    size_t i;

    for (i = 0; i < count; i++) {
        mean_a += (double)a[i];
        mean_b += (double)b[i];
    }
    mean_a /= (double)count;
    mean_b /= (double)count;

    for (i = 0; i < count; i++) {
        double da = (double)a[i] - mean_a;
        double db = (double)b[i] - mean_b;

        cov += da * db;
        var_a += da * da;
        var_b += db * db;
    }

    if ((var_a <= 0.0) || (var_b <= 0.0)) {
        return 1.0;
    }
    return cov / sqrt(var_a * var_b);
}

static int command_noise(void)
{
    static const pops_instr noise[6] = {
        POPS_INSTR_SNARE_1, POPS_INSTR_SNARE_2, POPS_INSTR_HIHAT,
        POPS_INSTR_CYMBAL_1, POPS_INSTR_CYMBAL_2, POPS_INSTR_TAMBOURINE
    };
    struct mp7_context_s *ctx = context_new();
    const size_t frames = 8192;
    int32_t *samples = (int32_t *)malloc(6u * frames * sizeof(int32_t));
    unsigned int i;
    unsigned int j;
    int worst = 1000;

    if (samples == 0) {
        printf("FAIL: out of memory\n");
        free(ctx);
        return 1;
    }

    for (i = 0; i < 6u; i++) {
        int peak;

        render_voice_i32(ctx, noise[i], samples + i * frames, frames, frames);
        peak = peak_i32(samples + i * frames, frames);
        if (peak < 100) {
            printf("FAIL: noise voice %u is silent\n", i);
            failures++;
        }
    }

    for (i = 0; i < 6u; i++) {
        for (j = i + 1u; j < 6u; j++) {
            double c = correlation(samples + i * frames, samples + j * frames, frames);
            double absolute = (c < 0.0) ? -c : c;

            if (absolute > 0.2) {
                printf("FAIL: noise voices %u and %u correlate at %.3f\n",
                       i, j, c);
                failures++;
            }
            if ((int)(absolute * 1000.0) < worst) {
                worst = (int)(absolute * 1000.0);
            }
        }
    }

    /* The Hihat has no Highpass control; its high-pass is fixed at 1000 Hz. */
    {
        struct pops_biquad expected;
         const struct pops_biquad *actual = &ctx->voices[POPS_INSTR_HIHAT].highpass;

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
         pops_biquad_set_highpass(&expected, 1000u, POPS_DEFAULT_SAMPLE_RATE);
        if ((actual->b0 != expected.b0) || (actual->b1 != expected.b1) ||
            (actual->b2 != expected.b2) || (actual->a1 != expected.a1) ||
            (actual->a2 != expected.a2)) {
            fail("Hihat high-pass is not fixed at 1000 Hz");
        } else {
            ok("Hihat high-pass is fixed at 1000 Hz");
        }
    }

    if (failures == 0) {
        printf("ok  : the 6 noise voices are uncorrelated (worst |r| = %.3f)\n",
               (double)worst / 1000.0);
    }

    free(samples);
    free(ctx);
    return failures == 0 ? 0 : 1;
}

static int command_guiro(void)
{
    struct mp7_context_s *ctx = context_new();
    const uint32_t sample_rate = POPS_DEFAULT_SAMPLE_RATE;
    const size_t frames = (size_t)(sample_rate / 2u);
    const size_t late = (size_t)(sample_rate / 10u);
    int32_t *held = (int32_t *)malloc(frames * sizeof(int32_t));
    int32_t *released = (int32_t *)malloc(frames * sizeof(int32_t));
    double held_rms;
    double released_rms;

    if ((held == 0) || (released == 0)) {
        printf("FAIL: out of memory\n");
        free(held);
        free(released);
        free(ctx);
        return 1;
    }

    render_voice_i32(ctx, POPS_INSTR_GUIRO, held, frames, frames);
    render_voice_i32(ctx, POPS_INSTR_GUIRO, released, frames, (size_t)(sample_rate / 10u));

    held_rms = rms_i32(held + (frames - late), late);
    released_rms = rms_i32(released + (frames - late), late);

    if (held_rms < 100.0) {
        printf("FAIL: held Guiro stopped early (RMS %.1f)\n", held_rms);
        failures++;
    }
    if (released_rms > 20.0) {
        printf("FAIL: Guiro note-off did not release (RMS %.1f)\n", released_rms);
        failures++;
    }
    if (failures == 0) {
        ok("Guiro holds while gated and releases on note-off");
    }

    free(held);
    free(released);
    free(ctx);
    return failures == 0 ? 0 : 1;
}

static int pattern_enabled_count(unsigned int rhythm)
{
    uint64_t enable = mp7_factory_rhythms[rhythm].enable;
    int count = 0;
    unsigned int column;

    for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
        if ((enable & ((uint64_t)1u << column)) != 0u) {
            count++;
        }
    }
    return count;
}

static uint64_t pattern_enable_mask(unsigned int rhythm)
{
    return mp7_factory_rhythms[rhythm].enable;
}

static uint16_t pattern_note(const struct mp7_factory_rhythm *pattern,
                             unsigned int column)
{
    return pattern->notes[column & (unsigned int)(pattern->notes_period - 1u)];
}

static uint16_t percussion_note(unsigned int column)
{
    return mp7_shared_percussion_notes[
        column & (MP7_PATTERN_HALF_STEPS - 1u)];
}

static int command_pattern(void)
{
    static const unsigned int short_loop[5] = { 0u, 8u, 10u, 15u, 18u };
    static const unsigned int compound_rhythms[3] = { 8u, 15u, 18u };
    struct mp7_context_s *ctx = context_new();
    const uint32_t sample_rate = POPS_DEFAULT_SAMPLE_RATE;
    unsigned int rhythm;
    unsigned int i;

    mp7_init(ctx, sample_rate);

    /* The factory data itself: a rhythm's own column mask only uses the
     * twelve rhythm instrument bits, the five short loops enable 48 columns,
     * everything else enables all 64, and the compound grid is the mask of
     * the three shuffle rhythms. */
    for (rhythm = 0u; rhythm < (unsigned int)POPS_RHYTHM_COUNT; rhythm++) {
        const struct mp7_factory_rhythm *pattern = &mp7_factory_rhythms[rhythm];
        int expect_short = 0;
        int expect_compound = 0;
        unsigned int column;

        for (i = 0u; i < 5u; i++) {
            if (short_loop[i] == rhythm) {
                expect_short = 1;
            }
        }
        for (i = 0u; i < 3u; i++) {
            if (compound_rhythms[i] == rhythm) {
                expect_compound = 1;
            }
        }

        if (pattern_enabled_count(rhythm) != (expect_short ? 48 : 64)) {
            printf("FAIL: %s enables %d columns\n", test_rhythm_names[rhythm],
                   pattern_enabled_count(rhythm));
            failures++;
        }
        if (mp7_mask_is_compound(pattern->enable) != (uint8_t)expect_compound) {
            printf("FAIL: %s is not on the %s grid\n", test_rhythm_names[rhythm],
                   expect_compound ? "compound" : "straight");
            failures++;
        }

        for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
            if ((pattern_note(pattern, column) &
                 (uint16_t)~MP7_PATTERN_RHYTHM_MASK) != 0u) {
                printf("FAIL: %s column %u is not a rhythm instrument mask\n",
                       test_rhythm_names[rhythm], column);
                failures++;
                break;
            }
        }
    }

    /* Check the enable masks by position, not just by their popcount. */
    if ((mp7_factory_rhythms[POPS_RHYTHM_WALTZ].enable !=
         UINT64_C(0xFF0F0FFFFF0F0FFF)) ||
        (mp7_factory_rhythms[POPS_RHYTHM_MARCH_2_4].enable !=
         UINT64_C(0x3F3F3F3F3F3F3F3F)) ||
        (mp7_factory_rhythms[POPS_RHYTHM_SAMBA].enable != ~(uint64_t)0)) {
        fail("the time-signature enable masks are wrong");
    }

    if ((mp7_factory_rhythms[POPS_RHYTHM_WALTZ].notes_period !=
         MP7_PATTERN_HALF_STEPS) ||
        (mp7_factory_rhythms[POPS_RHYTHM_SAMBA].notes_period !=
         MP7_PATTERN_STEPS) ||
        (mp7_factory_rhythms[POPS_RHYTHM_MARCH_2_4].notes_period !=
         MP7_PATTERN_QUARTER_STEPS) ||
        (mp7_factory_rhythms[POPS_RHYTHM_SLOW_ROCK].notes_period !=
         MP7_PATTERN_QUARTER_STEPS) ||
        (mp7_factory_rhythms[POPS_RHYTHM_MARCH_6_8].notes_period !=
         MP7_PATTERN_QUARTER_STEPS)) {
        fail("the stored note periods are not marked correctly");
    }

    if (failures == 0) {
        ok("the 20 factory patterns are well formed");
    }

    /* The Tambourine, Guiro and Quijada layer is a separate definition: it
     * uses only the three percussion bits, and it is not part of any
     * rhythm's own grid. */
    {
        static const char tambourine[] =
            "X...X...X...X...X...X...X...X...X...X...X...X...X...X...X...X...";
        static const char guiro[] =
            "XXXX....X...X...XXXX....X...X...XXXX....X...X...XXXX....X...X...";
        static const char quijada[] =
            "........................X...............................X.......";
        unsigned int column;

        for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
            uint16_t expect = 0u;

            if (tambourine[column] == 'X') {
                expect |= (uint16_t)(1u << POPS_INSTR_TAMBOURINE);
            }
            if (guiro[column] == 'X') {
                expect |= (uint16_t)(1u << POPS_INSTR_GUIRO);
            }
            if (quijada[column] == 'X') {
                expect |= (uint16_t)(1u << POPS_INSTR_QUIJADA);
            }
            if (percussion_note(column) != expect) {
                printf("FAIL: the shared percussion differs on column %u\n",
                       column);
                failures++;
                break;
            }
        }

        for (rhythm = 0u; rhythm < (unsigned int)POPS_RHYTHM_COUNT; rhythm++) {
            const struct mp7_factory_rhythm *pattern = &mp7_factory_rhythms[rhythm];

            for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
                if ((pattern_note(pattern, column) &
                     MP7_PATTERN_PERCUSSION_MASK) != 0u) {
                    printf("FAIL: %s carries the shared percussion itself\n",
                           test_rhythm_names[rhythm]);
                    failures++;
                    break;
                }
            }
        }

        if (failures == 0) {
            ok("the shared percussion layer is a separate definition");
        }
    }

    /* Combination: OR the notes, AND the enable masks, and the grid of the
     * selection follows the masks of the patterns in it. */
    mp7_pattern_combine(ctx, (uint32_t)1u << POPS_RHYTHM_ROCK_1);
    if (ctx->sequencer.enable != ~(uint64_t)0) {
        fail("Rock 1 on its own does not enable all 64 columns");
    }
    if (ctx->sequencer.subdivision != (uint8_t)MP7_SUBDIV_STRAIGHT) {
        fail("Rock 1 is not on the straight grid");
    }
    if ((ctx->sequencer.notes[12] & (1u << POPS_INSTR_BASS)) == 0u) {
        fail("Rock 1 lost its Base hit at column 12");
    }

    /* The view of a rhythm is the grid the sequencer plays: the same
     * columns, with the repetitions of the stored period already applied
     * and the shared percussion layer merged into it. */
    {
        struct pops_pattern_view view;
        unsigned int column;
        int bad = 0;

        pops_get_selection_pattern(ctx, POPS_BANK_A,
                                   pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1),
                                   &view);
        for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
            if (view.notes[column] != ctx->sequencer.notes[column]) {
                bad = 1;
                break;
            }
        }
        if (bad) {
            fail("the view of a rhythm is not the grid the sequencer plays");
        } else {
            ok("the view of a rhythm is the grid the sequencer plays");
        }
    }

    /* The combined grid carries the shared percussion layer in every column. */
    {
        unsigned int column;
        int bad = 0;

        for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
            if ((ctx->sequencer.notes[column] & percussion_note(column)) !=
                percussion_note(column)) {
                bad = 1;
                break;
            }
        }
        if (bad) {
            fail("the combined grid does not carry the shared percussion");
        } else {
            ok("the combined grid carries the shared percussion layer");
        }
    }

    mp7_pattern_combine(ctx,
                        ((uint32_t)1u << POPS_RHYTHM_ROCK_1) |
                        ((uint32_t)1u << POPS_RHYTHM_WALTZ));
    if (ctx->sequencer.enable != pattern_enable_mask(POPS_RHYTHM_WALTZ)) {
        fail("Rock 1 + Waltz does not use the Waltz enable mask");
    }
    if (ctx->sequencer.subdivision != (uint8_t)MP7_SUBDIV_STRAIGHT) {
        fail("Rock 1 + Waltz is not on the straight grid");
    }
    if ((ctx->sequencer.notes[24] & (1u << POPS_INSTR_SNARE_1)) == 0u) {
        fail("Rock 1 + Waltz did not OR the Snare 1 hit at column 24");
    }

    mp7_pattern_combine(ctx,
                        ((uint32_t)1u << POPS_RHYTHM_ROCK_1) |
                        ((uint32_t)1u << POPS_RHYTHM_MARCH_6_8));
    if (ctx->sequencer.enable != pattern_enable_mask(POPS_RHYTHM_MARCH_6_8)) {
        fail("Rock 1 + March 6/8 does not use the March enable mask");
    }
    if (ctx->sequencer.subdivision != (uint8_t)MP7_SUBDIV_COMPOUND) {
        fail("Rock 1 + March 6/8 is not on the compound grid");
    }

    mp7_pattern_combine(ctx, 0u);
    if ((ctx->sequencer.enable != 0u) ||
        (ctx->sequencer.subdivision != (uint8_t)MP7_SUBDIV_STRAIGHT)) {
        fail("an empty selection is not stopped");
    }

    /* Timing: Rock 1 at 120 BPM has a 22050-sample beat, and a straight beat
     * holds eight columns. The Base row is X...........X...X... so the first
     * hit gaps are 12 and 4 columns. */
    mp7_pattern_combine(ctx, (uint32_t)1u << POPS_RHYTHM_ROCK_1);
    mp7_pattern_set_tempo(ctx, 120u * 65536u);
    mp7_pattern_reset(ctx);
    if (ctx->sequencer.beat_samples != 22050u) {
        printf("FAIL: the beat is %u samples, expected 22050\n",
               ctx->sequencer.beat_samples);
        failures++;
    } else {
        ok("the beat is round(60 * 44100 / 120) = 22050 samples");
    }

    {
        const size_t beat = (size_t)ctx->sequencer.beat_samples;
        size_t trigger_times[16];
        unsigned int trigger_count = 0;
        size_t total = (size_t)MP7_PATTERN_STEPS * beat / 8u + 2u;
        size_t t;

        for (t = 0u; t < total; t++) {
            ctx->voices[POPS_INSTR_BASS].active = 0;
            mp7_pattern_tick(ctx);
            if (ctx->voices[POPS_INSTR_BASS].active != 0u) {
                if (trigger_count < 16u) {
                    trigger_times[trigger_count] = t;
                }
                trigger_count++;
            }
        }

        if (trigger_count < 9u) {
            printf("FAIL: Rock 1 Base fired %u times in one loop\n", trigger_count);
            failures++;
        } else {
            static const unsigned int gap_columns[9] = { 12u, 4u, 12u, 4u, 12u, 4u, 12u, 4u, 12u };
            unsigned int hit;

            if (trigger_times[0] != 0u) {
                fail("the first Base hit is not at the first column boundary");
            }
            for (hit = 1u; hit < 9u; hit++) {
                size_t expected = (size_t)gap_columns[hit - 1u] * beat / 8u;
                size_t got = trigger_times[hit] - trigger_times[hit - 1u];

                if (got != expected) {
                    printf("FAIL: Base gap %u is %lu samples, expected %lu\n",
                           hit, (unsigned long)got, (unsigned long)expected);
                    failures++;
                }
            }
            /* The loop is 64 columns of eight, so it is exactly two 4/4
             * bars: the ninth hit lands on the eighth beat. */
            if (trigger_times[8] != 8u * beat) {
                printf("FAIL: the loop is %lu samples, expected %lu\n",
                       (unsigned long)trigger_times[8], (unsigned long)(8u * beat));
                failures++;
            }
            if (failures == 0) {
                ok("Rock 1 Base hits land on the expected column boundaries");
            }
        }
    }

    /* Timing on the compound grid: a compound beat holds six columns, so a
     * compound pattern's hits land a beat apart. The March 6/8 Base row hits
     * every 8 columns and the compound mask enables 6 of every 8, so the hits
     * are 6 columns apart on the grid: the disabled columns take no time. */
    mp7_pattern_combine(ctx, (uint32_t)1u << POPS_RHYTHM_MARCH_6_8);
    mp7_pattern_set_tempo(ctx, 120u * 65536u);
    mp7_pattern_reset(ctx);
    {
        const size_t beat = (size_t)ctx->sequencer.beat_samples;
        size_t trigger_times[16];
        size_t trigger_count = 0u;
        size_t total = (size_t)MP7_PATTERN_STEPS * beat / 6u + 2u;
        size_t t;

        for (t = 0u; t < total; t++) {
            ctx->voices[POPS_INSTR_BASS].active = 0;
            mp7_pattern_tick(ctx);
            if (ctx->voices[POPS_INSTR_BASS].active != 0u) {
                if (trigger_count < 16u) {
                    trigger_times[trigger_count] = t;
                }
                trigger_count++;
            }
        }

        if (trigger_count < 9u) {
            printf("FAIL: March 6/8 Base fired %lu times in one loop\n",
                   (unsigned long)trigger_count);
            failures++;
        } else {
            const size_t expected = beat;
            size_t hit;

            if (trigger_times[0] != 0u) {
                fail("the first Base hit is not at the first column boundary");
            }
            for (hit = 1u; hit < 4u; hit++) {
                size_t got = trigger_times[hit] - trigger_times[hit - 1u];

                if (got != expected) {
                    printf("FAIL: compound Base gap %lu is %lu samples, expected %lu\n",
                           (unsigned long)hit, (unsigned long)got,
                           (unsigned long)expected);
                    failures++;
                }
            }
            /* The compound loop is 48 columns of six, so it is also exactly
             * two 4/4 bars: the ninth hit lands on the eighth beat. */
            if (trigger_times[8] != 8u * beat) {
                printf("FAIL: the compound loop is %lu samples, expected %lu\n",
                       (unsigned long)trigger_times[8],
                       (unsigned long)(8u * beat));
                failures++;
            }
            if (failures == 0) {
                ok("a compound pattern's hits land one beat apart");
            }
        }
    }

    /* Guiro gate: the shared row starts with XXXX and then rests, so the
     * gate must stay open across columns 0..3 and close on column 4. */
    mp7_init(ctx, sample_rate);
    mp7_pattern_combine(ctx, (uint32_t)1u << POPS_RHYTHM_ROCK_1);
    mp7_pattern_set_tempo(ctx, 120u * 65536u);
    {
        const size_t column_samples = (size_t)ctx->sequencer.beat_samples / 8u + 1u;
        size_t t;
        int open_on_three = 0;
        int closed_on_four = 0;

        for (t = 0u; t < 6u * column_samples; t++) {
            uint8_t column = ctx->sequencer.column;

            mp7_pattern_tick(ctx);
            if (ctx->sequencer.column == column) {
                continue;
            }
            if (ctx->sequencer.column == 3u) {
                open_on_three = (ctx->sequencer.guiro_gated == 1u);
            } else if (ctx->sequencer.column == 4u) {
                closed_on_four = (ctx->sequencer.guiro_gated == 0u);
            }
        }
        if (!open_on_three || !closed_on_four) {
            fail("the Guiro gate did not follow the adjacent X columns");
        } else {
            ok("the Guiro gate stays open across adjacent X columns");
        }
    }

    /* An immediate change that disables the column the playhead is on must
     * not idle for a whole column: the next enabled column fires at the next
     * column boundary. March 2/4 disables columns 6 and 7 and hits Snare 2
     * on column 8. */
    mp7_init(ctx, sample_rate);
    pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1));
    ctx->sequencer.column = 6u;
    ctx->sequencer.column_fired = 0u;
    ctx->sequencer.column_pos = 0u;
    pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_MARCH_2_4));
    mp7_pattern_tick(ctx);
    if ((ctx->voices[POPS_INSTR_SNARE_2].active == 0u) ||
        (ctx->sequencer.column != 8u)) {
        fail("the sequencer idled on a column the new grid disables");
    } else {
        ok("a column the new grid disables is skipped, not waited out");
    }

    /* The signature of a mask is the inverse of the mask of a signature,
     * and a mask that belongs to no signature has none. The compound mask
     * is the mask of the 2/4 and of the 6/8 alike, so it names the 2/4. */
    {
        int bad = 0;
        unsigned int timesig;

        for (timesig = 0u; timesig < (unsigned int)POPS_TIMESIG_COUNT; timesig++) {
            pops_timesig found = pops_timesig_of_mask(
                pops_timesig_enable_mask((pops_timesig)timesig));

            if ((found != (pops_timesig)timesig) &&
                !((found == POPS_TIMESIG_2_4) &&
                  (timesig == (unsigned int)POPS_TIMESIG_6_8T))) {
                bad = 1;
                break;
            }
        }
        if (pops_timesig_of_mask(UINT64_C(0x0F0F0F0F0F0F0F0F)) !=
            POPS_TIMESIG_NONE) {
            bad = 1;
        }
        if (bad) {
            fail("pops_timesig_of_mask() is not the inverse of the mask");
        } else {
            ok("pops_timesig_of_mask() names the signature of a mask");
        }
    }

    /* The view of a selection is the grid the sequencer plays, for one
     * rhythm and for several of them. */
    {
        struct pops_pattern_view view;
        unsigned int column;
        int bad = 0;

        mp7_init(ctx, sample_rate);
        pops_select_rhythm(ctx, POPS_BANK_A,
                          pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1));
        pops_get_selection_pattern(ctx, POPS_BANK_A,
                                   pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1),
                                   &view);
        for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
            if (view.notes[column] != ctx->sequencer.notes[column]) {
                bad = 1;
                break;
            }
        }
        if ((view.enable != ctx->sequencer.enable) ||
            (view.compound != (ctx->sequencer.subdivision ==
                               (uint8_t)MP7_SUBDIV_COMPOUND))) {
            bad = 1;
        }

        /* Two rhythms: the notes are OR-ed, the masks are AND-ed, and the
         * grid follows the masks of the rhythms in it. Rock 1 is straight
         * and March 2/4 is compound, so the selection plays compound even
         * though its combined mask is the compound one of March 2/4. */
        pops_select_rhythm(ctx, POPS_BANK_A,
                          pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1) |
                          pops_rhythm_button_bit(POPS_RHYTHM_MARCH_2_4));
        pops_get_selection_pattern(ctx, POPS_BANK_A,
                                   pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1) |
                                   pops_rhythm_button_bit(POPS_RHYTHM_MARCH_2_4),
                                   &view);
        for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
            if (view.notes[column] != ctx->sequencer.notes[column]) {
                bad = 1;
                break;
            }
        }
        if (view.enable != ctx->sequencer.enable) {
            bad = 1;
        }
        if (view.compound != 1u) {
            fail("a selection of a straight and a compound rhythm is not "
                 "compound");
            bad = 1;
        }

        /* Rock 1 and the Waltz carry different signatures, so the selection
         * is a mixed one even though the combined mask is the one of the
         * Waltz. */
        if (view.timesig != POPS_TIMESIG_NONE) {
            fail("a selection of different signatures is not mixed");
            bad = 1;
        }

        if (bad) {
            fail("the view of a selection is not the grid the sequencer plays");
        } else {
            ok("the view of a selection is the grid the sequencer plays");
        }
    }

    /* The view of a selection does not touch the sequencer: it can be asked
     * for a selection that is not playing. */
    {
        struct pops_pattern_view view;
        uint16_t before[MP7_PATTERN_STEPS];
        unsigned int column;
        int bad = 0;

        mp7_init(ctx, sample_rate);
        pops_select_rhythm(ctx, POPS_BANK_A,
                          pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1));
        memcpy(before, ctx->sequencer.notes, sizeof(before));

        pops_get_selection_pattern(ctx, POPS_BANK_A,
                                   pops_rhythm_button_bit(POPS_RHYTHM_WALTZ),
                                   &view);
        for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
            if (ctx->sequencer.notes[column] != before[column]) {
                bad = 1;
                break;
            }
        }
        if (bad) {
            fail("asking for a selection changed the running one");
        } else {
            ok("asking for a selection does not touch the sequencer");
        }
    }

    free(ctx);
    return failures == 0 ? 0 : 1;
}

#ifdef LIBPOPS_EXTRA

/* The largest pole magnitude of a biquad. A filter is stable only while
 * this stays below 1: at 1 the poles sit on the unit circle and the filter
 * rings without decaying, and above it the state diverges. */
static double biquad_pole_radius(const struct pops_biquad *filter)
{
    double a1 = (double)filter->a1 / 1073741824.0;
    double a2 = (double)filter->a2 / 1073741824.0;
    double real = -a1 / 2.0;
    double disc = (a1 * a1 / 4.0) - a2;

    if (disc < 0.0) {
        /* Complex roots: the radius is the square root of their product. */
        return sqrt((real * real) + (-disc));
    }
    {
        double root = sqrt(disc);
        double p1 = fabs(real + root);
        double p2 = fabs(real - root);

        return (p1 > p2) ? p1 : p2;
    }
}

/* Two biquads with the same coefficients play the same filter, so a
 * parameter change can be verified against a reference coefficient set. */
static int biquad_coeffs_match(const struct pops_biquad *a,
                               const struct pops_biquad *b)
{
    return (a->b0 == b->b0) && (a->b1 == b->b1) && (a->b2 == b->b2) &&
           (a->a1 == b->a1) && (a->a2 == b->a2);
}

/* A user pattern with one instrument mask on every column of the cycle. */
static void fill_user_pattern(struct pops_user_pattern *pattern,
                              uint16_t mask,
                              pops_timesig timesig)
{
    unsigned int column;

    memset(pattern, 0, sizeof(*pattern));
    pattern->timesig = timesig;

    for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
        pattern->notes[column] = mask;
    }
}

/* The combined grid of a single user slot: the slot's own columns, with
 * nothing else added to them. */
static int user_grid_matches(const struct mp7_sequencer *seq,
                              const struct pops_user_pattern *pattern)
{
    unsigned int column;

    for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
        if (seq->notes[column] != pattern->notes[column]) {
            return 0;
        }
    }
    return 1;
}

/* The combined grid of one factory rhythm: its own columns plus the shared
 * percussion layer. */
static int factory_grid_matches(const struct mp7_sequencer *seq,
                                 const struct mp7_factory_rhythm *pattern)
{
    unsigned int column;

    for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
        uint16_t expect = (uint16_t)(
            pattern->notes[column & (unsigned int)(pattern->notes_period - 1u)] |
            mp7_shared_percussion_notes[
                column & (MP7_PATTERN_HALF_STEPS - 1u)]);

        if (seq->notes[column] != expect) {
            return 0;
        }
    }
    return 1;
}

static int command_user(void)
{
    struct mp7_context_s *ctx = context_new();
    const uint32_t sample_rate = POPS_DEFAULT_SAMPLE_RATE;
    struct pops_user_pattern pattern;
    struct pops_user_pattern readback;
    unsigned int column;

    mp7_init(ctx, sample_rate);

    /* A pattern survives the round trip through a slot. */
    fill_user_pattern(&pattern, (uint16_t)(1u << POPS_INSTR_BASS), POPS_TIMESIG_4_4);
    if (!pops_set_user_pattern(ctx, 0u, &pattern)) {
        fail("pops_set_user_pattern() rejected a valid slot");
    } else if (!pops_get_user_pattern(ctx, 0u, &readback)) {
        fail("pops_get_user_pattern() did not return a stored pattern");
    } else {
        int same = (readback.timesig == pattern.timesig);

        for (column = 0u; same && (column < POPS_USER_PATTERN_COLUMNS); column++) {
            if (readback.notes[column] != pattern.notes[column]) {
                same = 0;
            }
        }

        if (!same) {
            fail("a user pattern did not survive the round trip");
        } else {
            ok("a user pattern survives the round trip through a slot");
        }
    }

    /* A bad slot, a bad context and a null destination. */
    if (pops_set_user_pattern(ctx, POPS_USER_PATTERN_COUNT, &pattern) ||
        pops_set_user_pattern(0, 0u, &pattern) ||
        pops_get_user_pattern(ctx, POPS_USER_PATTERN_COUNT, &readback) ||
        pops_get_user_pattern(0, 0u, &readback) ||
        pops_get_user_pattern(ctx, 0u, 0)) {
        fail("the user pattern API accepted an invalid argument");
    } else {
        ok("the user pattern API rejects invalid arguments");
    }

    /* An unknown time signature is refused, and the slot keeps the pattern
     * it had. */
    {
        struct pops_user_pattern unknown;

        fill_user_pattern(&unknown, (uint16_t)(1u << POPS_INSTR_SNARE_1),
                          (pops_timesig)POPS_TIMESIG_COUNT);
        if (pops_set_user_pattern(ctx, 0u, &unknown)) {
            fail("pops_set_user_pattern() accepted an unknown time signature");
        } else if (!pops_get_user_pattern(ctx, 0u, &readback) ||
                   (readback.timesig != POPS_TIMESIG_4_4)) {
            fail("an unknown time signature changed the slot");
        } else {
            ok("the user pattern API rejects an unknown time signature");
        }
    }

    /* A null pattern empties the slot, and an empty slot is not a pattern. */
    if (!pops_set_user_pattern(ctx, 0u, 0)) {
        fail("pops_set_user_pattern() rejected a null pattern");
    } else if (pops_get_user_pattern(ctx, 0u, &readback)) {
        fail("an emptied slot still returns a pattern");
    } else {
        ok("a null pattern empties the slot");
    }

    /* The preset source ignores the user slots. Button 0 is the Waltz, so
     * the selection points at slot 0 in the user source. */
    fill_user_pattern(&pattern, (uint16_t)(1u << POPS_INSTR_BASS), POPS_TIMESIG_4_4);
    pops_set_user_pattern(ctx, 0u, &pattern);
    pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_WALTZ));
    if (pops_get_pattern_source(ctx) != POPS_PATTERN_SOURCE_PRESET) {
        fail("the preset source is not the default");
    } else if (!factory_grid_matches(&ctx->sequencer,
                                     &mp7_factory_rhythms[POPS_RHYTHM_WALTZ])) {
        fail("the preset source does not play the factory rhythm");
    } else {
        ok("the preset source plays the factory rhythms, not the user slots");
    }

    /* The switch re-reads the current selection against the user slots. */
    pops_set_pattern_source(ctx, POPS_PATTERN_SOURCE_USER);
    if (pops_get_pattern_source(ctx) != POPS_PATTERN_SOURCE_USER) {
        fail("the pattern source did not switch to the user slots");
    } else if (!user_grid_matches(&ctx->sequencer, &pattern)) {
        fail("the user source does not play the user slot");
    } else {
        ok("the source switch points the selection at the user slots");
    }

    /* The view of one pattern follows the source: the user slot while the
     * user source is selected, the factory rhythm in the preset source. */
    {
        struct pops_pattern_view view;
        unsigned int column;
        int bad = 0;

        pops_set_pattern_source(ctx, POPS_PATTERN_SOURCE_USER);
        pops_get_selection_pattern(ctx, POPS_BANK_A,
                                   pops_rhythm_button_bit(POPS_RHYTHM_WALTZ),
                                   &view);
        for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
            if (view.notes[column] != pattern.notes[column]) {
                bad = 1;
                break;
            }
        }
        if (view.enable != pops_timesig_enable_mask(pattern.timesig)) {
            bad = 1;
        }
        if (view.timesig != pattern.timesig) {
            bad = 1;
        }
        if (bad) {
            fail("the view of a user pattern is not the grid of the slot");
        } else {
            ok("the view of a pattern follows the source of the context");
        }
    }

    /* A user pattern is complete: the shared percussion layer is not added
     * to it, and its own percussion lines play. */
    {
        struct pops_user_pattern percussion;

        fill_user_pattern(&percussion,
                          (uint16_t)(1u << POPS_INSTR_TAMBOURINE),
                          POPS_TIMESIG_4_4);
        pops_set_user_pattern(ctx, 1u, &percussion);
        pops_select_rhythm(ctx, POPS_BANK_A,
                          pops_rhythm_button_bit(POPS_RHYTHM_WALTZ) |
                          pops_rhythm_button_bit(POPS_RHYTHM_SAMBA));

        if ((ctx->sequencer.notes[0] &
             (uint16_t)~((uint16_t)(1u << POPS_INSTR_TAMBOURINE) |
                          (uint16_t)(1u << POPS_INSTR_BASS))) != 0u) {
            fail("a user pattern carries the shared percussion layer");
        } else if ((ctx->sequencer.notes[0] &
                    (uint16_t)(1u << POPS_INSTR_TAMBOURINE)) == 0u) {
            fail("a user pattern does not play its own percussion line");
        } else {
            ok("a user pattern plays its own percussion, not the shared layer");
        }
    }

    /* Two user slots combine like two factory rhythms: the notes are OR-ed
     * and the enable masks are AND-ed. */
    {
        struct pops_user_pattern snare;

        fill_user_pattern(&snare, (uint16_t)(1u << POPS_INSTR_SNARE_1), POPS_TIMESIG_3_4);
        pops_set_user_pattern(ctx, 2u, &snare);
        pops_select_rhythm(ctx, POPS_BANK_A,
                          pops_rhythm_button_bit(POPS_RHYTHM_WALTZ) |
                          pops_rhythm_button_bit(POPS_RHYTHM_MAMBO));

        if ((ctx->sequencer.enable != UINT64_C(0xFF0F0FFFFF0F0FFF)) ||
            ((ctx->sequencer.notes[0] & (uint16_t)(1u << POPS_INSTR_SNARE_1)) == 0u) ||
            ((ctx->sequencer.notes[0] & (uint16_t)(1u << POPS_INSTR_BASS)) == 0u)) {
            fail("two user slots do not combine like two factory rhythms");
        } else {
            ok("two user slots combine like two factory rhythms");
        }
    }

    /* An empty slot plays nothing and disables no column. */
    mp7_init(ctx, sample_rate);
    pops_set_pattern_source(ctx, POPS_PATTERN_SOURCE_USER);
    pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_WALTZ));
    if ((ctx->sequencer.enable != ~(uint64_t)0) ||
        (ctx->sequencer.notes[0] != 0u)) {
        fail("an empty user slot is not a silent pattern");
    } else {
        ok("an empty user slot plays nothing and disables no column");
    }

    /* With no button pressed the user source keeps the transport running
     * and stays silent: the shared layer belongs to the preset source. */
    pops_select_rhythm(ctx, POPS_BANK_A, 0u);
    if (!pops_rhythm_running(ctx) || (ctx->sequencer.enable != ~(uint64_t)0)) {
        fail("the user source stops the transport when no button is pressed");
    } else {
        int silent = 1;

        for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
            if (ctx->sequencer.notes[column] != 0u) {
                silent = 0;
                break;
            }
        }
        if (!silent) {
            fail("the user source plays the shared percussion layer");
        } else {
            ok("the user source stays silent when no button is pressed");
        }
    }

    /* The grid of a user pattern follows its time signature and reaches the
     * sequencer. */
    {
        struct pops_user_pattern shuffle;
        struct pops_user_pattern straight;

        fill_user_pattern(&shuffle, (uint16_t)(1u << POPS_INSTR_BASS), POPS_TIMESIG_2_4);
        pops_set_user_pattern(ctx, 3u, &shuffle);
        pops_select_rhythm(ctx, POPS_BANK_A,
                          pops_rhythm_button_bit(POPS_RHYTHM_WALTZ) |
                          pops_rhythm_button_bit(POPS_RHYTHM_SAMBA) |
                          pops_rhythm_button_bit(POPS_RHYTHM_MAMBO) |
                          pops_rhythm_button_bit(POPS_RHYTHM_RUMBA));
        if (ctx->sequencer.subdivision != (uint8_t)MP7_SUBDIV_COMPOUND) {
            fail("the grid of a user pattern was lost");
        } else {
            ok("the grid of a user pattern reaches the sequencer");
        }

        fill_user_pattern(&straight, (uint16_t)(1u << POPS_INSTR_BASS), POPS_TIMESIG_4_4);
        pops_set_user_pattern(ctx, 4u, &straight);
        pops_select_rhythm(ctx, POPS_BANK_A,
                          pops_rhythm_button_bit(POPS_RHYTHM_WALTZ) |
                          pops_rhythm_button_bit(POPS_RHYTHM_SAMBA) |
                          pops_rhythm_button_bit(POPS_RHYTHM_MAMBO) |
                          pops_rhythm_button_bit(POPS_RHYTHM_RUMBA));
        if (ctx->sequencer.subdivision != (uint8_t)MP7_SUBDIV_COMPOUND) {
            fail("a compound pattern in the selection was straightened");
        } else {
            ok("a compound pattern in the selection keeps the compound grid");
        }
    }

    /* A user pattern plays on the mask of its time signature. */
    {
        struct pops_user_pattern metered;

        mp7_init(ctx, sample_rate);
        pops_set_pattern_source(ctx, POPS_PATTERN_SOURCE_USER);

        fill_user_pattern(&metered, (uint16_t)(1u << POPS_INSTR_BASS), POPS_TIMESIG_7_4);
        pops_set_user_pattern(ctx, 0u, &metered);
        pops_select_rhythm(ctx, POPS_BANK_A,
                          pops_rhythm_button_bit(POPS_RHYTHM_WALTZ));

        if (ctx->sequencer.enable != pops_timesig_enable_mask(POPS_TIMESIG_7_4)) {
            fail("a user pattern does not play on the mask of its signature");
        } else if (ctx->sequencer.enable != UINT64_C(0x00FFFFFFFFFFFFFF)) {
            fail("the 7/4 mask is wrong");
        } else {
            ok("a user pattern plays on the mask of its time signature");
        }
    }

    /* The 5/4 signature uses the compound grid, so its grouped columns keep a
     * triplet feel and the loop is slower than the same columns straight. The
     * 7/4 signature has no such grouping and stays straight. */
    {
        struct pops_user_pattern fives;
        struct pops_user_pattern sevens;

        mp7_init(ctx, sample_rate);
        pops_set_pattern_source(ctx, POPS_PATTERN_SOURCE_USER);

        fill_user_pattern(&fives, (uint16_t)(1u << POPS_INSTR_BASS), POPS_TIMESIG_5_4T);
        pops_set_user_pattern(ctx, 0u, &fives);
        pops_select_rhythm(ctx, POPS_BANK_A,
                          pops_rhythm_button_bit(POPS_RHYTHM_WALTZ));
        if (ctx->sequencer.subdivision != (uint8_t)MP7_SUBDIV_COMPOUND) {
            fail("a 5/4 pattern is not on the compound grid");
        }

        fill_user_pattern(&sevens, (uint16_t)(1u << POPS_INSTR_BASS), POPS_TIMESIG_7_4);
        pops_set_user_pattern(ctx, 0u, &sevens);
        if (ctx->sequencer.subdivision != (uint8_t)MP7_SUBDIV_STRAIGHT) {
            fail("a 7/4 pattern did not stay on the straight grid");
        } else {
            ok("5/4 plays compound and 7/4 stays straight");
        }
    }

    /* The switch keeps the playhead and cancels a pending change. */
    mp7_init(ctx, sample_rate);
    pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1));
    mp7_pattern_tick(ctx);
    pops_select_rhythm(ctx, POPS_BANK_B,
                      pops_rhythm_button_bit(POPS_RHYTHM_JAZZ_WALTZ) |
                      POPS_APPLY_NEXT_CYCLE);
    {
        uint8_t column = ctx->sequencer.column;
        struct pops_panel_state state;

        pops_set_pattern_source(ctx, POPS_PATTERN_SOURCE_USER);
        pops_get_panel_state(ctx, &state);
        if (ctx->sequencer.column != column) {
            fail("the source switch moved the playhead");
        } else if (state.pending) {
            fail("the source switch left a pending change behind");
        } else {
            ok("the source switch keeps the playhead and cancels a pending change");
        }
    }

    /* A reset returns to the preset source but keeps the user slots.
     *
     * The check covers the lowest and the highest slot, the grid as well as
     * the time signature, and a slot that was emptied with a null pattern:
     * it must stay empty across the reset. */
    {
        struct pops_user_pattern low;
        struct pops_user_pattern mid;
        struct pops_user_pattern high;
        int same;

        fill_user_pattern(&low, (uint16_t)(1u << POPS_INSTR_BASS), POPS_TIMESIG_4_4);
        fill_user_pattern(&mid, (uint16_t)(1u << POPS_INSTR_SNARE_1), POPS_TIMESIG_7_4);
        fill_user_pattern(&high, (uint16_t)(1u << POPS_INSTR_HIHAT), POPS_TIMESIG_6_8T);

        pops_set_user_pattern(ctx, 0u, &low);
        pops_set_user_pattern(ctx, 4u, &mid);
        pops_set_user_pattern(ctx, POPS_USER_PATTERN_COUNT - 1u, &high);
        pops_set_user_pattern(ctx, 1u, 0);

        pops_reset(ctx);

        if (pops_get_pattern_source(ctx) != POPS_PATTERN_SOURCE_PRESET) {
            fail("a reset did not return to the preset source");
            goto reset_done;
        }

        same = 1;
        for (column = 0u; same && (column < POPS_USER_PATTERN_COLUMNS); column++) {
            if (!pops_get_user_pattern(ctx, 0u, &readback) ||
                (readback.notes[column] != low.notes[column]) ||
                (readback.timesig != low.timesig)) {
                same = 0;
            }
        }
        if (!same) {
            fail("a reset did not keep the lowest user slot");
            goto reset_done;
        }

        same = 1;
        for (column = 0u; same && (column < POPS_USER_PATTERN_COLUMNS); column++) {
            if (!pops_get_user_pattern(ctx, 4u, &readback) ||
                (readback.notes[column] != mid.notes[column]) ||
                (readback.timesig != mid.timesig)) {
                same = 0;
            }
        }
        if (!same) {
            fail("a reset did not keep the middle user slot");
            goto reset_done;
        }

        same = 1;
        for (column = 0u; same && (column < POPS_USER_PATTERN_COLUMNS); column++) {
            if (!pops_get_user_pattern(ctx, POPS_USER_PATTERN_COUNT - 1u,
                                     &readback) ||
                (readback.notes[column] != high.notes[column]) ||
                (readback.timesig != high.timesig)) {
                same = 0;
            }
        }
        if (!same) {
            fail("a reset did not keep the highest user slot");
            goto reset_done;
        }

        if (pops_get_user_pattern(ctx, 1u, &readback)) {
            fail("a reset refilled an emptied user slot");
            goto reset_done;
        }

        ok("a reset keeps the user slots and returns to the preset source");

    reset_done:
        (void)0;
    }

    /* A user pattern plays. */
    {
        int16_t block[512 * 2];
        int nonzero = 0;
        size_t i;

        mp7_init(ctx, sample_rate);
        fill_user_pattern(&pattern, (uint16_t)(1u << POPS_INSTR_BASS), POPS_TIMESIG_4_4);
        pops_set_user_pattern(ctx, 0u, &pattern);
        pops_select_rhythm(ctx, POPS_BANK_A,
                          pops_rhythm_button_bit(POPS_RHYTHM_WALTZ));
        pops_set_pattern_source(ctx, POPS_PATTERN_SOURCE_USER);

        memset(block, 0, sizeof(block));
        pops_process(ctx, block, 512);
        for (i = 0u; i < 512u * 2u; i++) {
            if (block[i] != 0) {
                nonzero = 1;
                break;
            }
        }
        if (!nonzero) {
            fail("a user pattern renders no audio");
        } else {
            ok("a user pattern renders audio");
        }
    }

    /* A pattern that is playing changes with the pattern. */
    {
        struct pops_user_pattern changed;

        mp7_init(ctx, sample_rate);
        fill_user_pattern(&pattern, (uint16_t)(1u << POPS_INSTR_BASS), POPS_TIMESIG_4_4);
        pops_set_user_pattern(ctx, 0u, &pattern);
        pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_WALTZ));
        pops_set_pattern_source(ctx, POPS_PATTERN_SOURCE_USER);

        fill_user_pattern(&changed, (uint16_t)(1u << POPS_INSTR_SNARE_2), POPS_TIMESIG_4_4);
        pops_set_user_pattern(ctx, 0u, &changed);
        if (!user_grid_matches(&ctx->sequencer, &changed)) {
            fail("a playing user pattern did not change with the pattern");
        } else {
            ok("a playing user pattern changes with the pattern");
        }
    }

    /* Accent: bit 15 of a user pattern column makes every instrument in the
     * column (except the gated Guiro) louder, and it survives a slot round
     * trip. */
    {
        struct pops_user_pattern accented;
        struct pops_user_pattern readback;
        unsigned int failures_before = (unsigned int)failures;

        mp7_init(ctx, sample_rate);
        memset(&accented, 0, sizeof(accented));
        accented.timesig = POPS_TIMESIG_4_4;
        accented.notes[0] = (uint16_t)(1u << POPS_INSTR_BASS);
        accented.notes[1] = (uint16_t)((1u << POPS_INSTR_BASS) |
                                       POPS_PATTERN_ACCENT);

        pops_set_user_pattern(ctx, 0u, &accented);
        if (!pops_get_user_pattern(ctx, 0u, &readback)) {
            fail("pops_get_user_pattern() did not return an accented pattern");
        } else if (readback.notes[1] != accented.notes[1]) {
            fail("the accent bit did not survive the user pattern round trip");
        }

        pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_WALTZ));
        pops_set_pattern_source(ctx, POPS_PATTERN_SOURCE_USER);

        /* A beat of eight samples makes a column one sample long, so every
         * tick fires the next column. */
        ctx->sequencer.beat_samples = 8u;

        mp7_pattern_tick(ctx);                 /* fire column 0 */
        if (ctx->voices[POPS_INSTR_BASS].accent_gain_q16 != POPS_Q16_ONE) {
            fail("an unaccented column carries accent gain");
        }

        mp7_pattern_tick(ctx);                   /* fire column 1 */
        if (ctx->voices[POPS_INSTR_BASS].accent_gain_q16 !=
            MP7_PATTERN_ACCENT_GAIN_Q16) {
            fail("an accented column does not raise the instrument gain");
        }

        if ((unsigned int)failures == failures_before) {
            ok("user pattern accent bit survives round trip and raises gain");
        }
    }

    /* The gated Guiro ignores the accent bit; its gain stays at unity. */
    {
        struct pops_user_pattern accented;
        unsigned int failures_before = (unsigned int)failures;

        mp7_init(ctx, sample_rate);
        memset(&accented, 0, sizeof(accented));
        accented.timesig = POPS_TIMESIG_4_4;
        accented.notes[0] = (uint16_t)(1u << POPS_INSTR_GUIRO);
        accented.notes[1] = (uint16_t)((1u << POPS_INSTR_GUIRO) |
                                       POPS_PATTERN_ACCENT);

        pops_set_user_pattern(ctx, 0u, &accented);
        pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_WALTZ));
        pops_set_pattern_source(ctx, POPS_PATTERN_SOURCE_USER);
        ctx->sequencer.beat_samples = 8u;

        mp7_pattern_tick(ctx);                 /* fire column 0 */
        if (ctx->voices[POPS_INSTR_GUIRO].accent_gain_q16 != POPS_Q16_ONE) {
            fail("unaccented Guiro column changed the Guiro gain");
        }

        mp7_pattern_tick(ctx);                 /* fire column 1 */
        if (ctx->voices[POPS_INSTR_GUIRO].accent_gain_q16 != POPS_Q16_ONE) {
            fail("accented Guiro column changed the Guiro gain");
        }

        if ((unsigned int)failures == failures_before) {
            ok("Guiro ignores the user pattern accent bit");
        }
    }

    /* The accent has to reach the mix as soon as the column fires.
     *
     * The voice's gain is cached, because the per-sample path only applies
     * it, so a note that is already sounding would keep the old accent
     * until the next trigger if the cache were not refreshed here. */
    {
        struct mp7_context_s *live = context_new();
        struct pops_user_pattern accented;
        int16_t stereo[512 * 2];
        int32_t base_gain;
        int32_t expected_gain;
        int32_t peak_plain = 0;
        int32_t peak_accented = 0;
        unsigned int i;

        mp7_init(live, sample_rate);
        memset(&accented, 0, sizeof(accented));
        accented.timesig = POPS_TIMESIG_4_4;
        accented.notes[0] = (uint16_t)((1u << POPS_INSTR_BASS) |
                                       POPS_PATTERN_ACCENT);

        pops_set_user_pattern(live, 0u, &accented);
        pops_select_rhythm(live, POPS_BANK_A,
                          pops_rhythm_button_bit(POPS_RHYTHM_WALTZ));
        pops_set_pattern_source(live, POPS_PATTERN_SOURCE_USER);

        /* A beat of 800 samples makes a column 100 samples long, so the
         * column fires in this block, after the gain was computed for it. */
        live->sequencer.beat_samples = 800u;

        pops_process(live, stereo, 512);        /* the column fires here */

        if (live->voices[POPS_INSTR_BASS].accent_gain_q16 !=
            MP7_PATTERN_ACCENT_GAIN_Q16) {
            fail("the accented column did not fire inside the block");
        }

        base_gain = pops_mul_q16(
            live->voices[POPS_INSTR_BASS].level_q16,
            pops_mul_q16(
                pops_mixer_balance_gain_q16(&live->mixer,
                                             live->voices[POPS_INSTR_BASS].balance_group),
                live->mixer.instr_gain_q16[POPS_INSTR_BASS]));
        expected_gain = pops_mul_q16(base_gain, MP7_PATTERN_ACCENT_GAIN_Q16);

        if (live->voice_gain_q16[POPS_INSTR_BASS] != expected_gain) {
            fail("the accent did not reach the voice gain when the column fired");
        }

        for (i = 0u; i < 512u * 2u; i++) {
            int32_t value = (stereo[i] < 0) ? -stereo[i] : stereo[i];

            if (value > peak_accented) {
                peak_accented = value;
            }
        }

        /* The same note, without the accent. */
        mp7_init(live, sample_rate);
        pops_trigger(live, POPS_INSTR_BASS, 100);
        pops_process(live, stereo, 512);
        for (i = 0u; i < 512u * 2u; i++) {
            int32_t value = (stereo[i] < 0) ? -stereo[i] : stereo[i];

            if (value > peak_plain) {
                peak_plain = value;
            }
        }

        if ((peak_plain == 0) || (peak_accented <= peak_plain)) {
            fail("the accented note is not louder than the plain one");
        } else {
            int32_t expected_peak = (peak_plain * MP7_PATTERN_ACCENT_GAIN_Q16) >> 16;

            /* The output is rounded to Q15, so the peak can be a unit or two
             * off the exact ratio. */
            if ((peak_accented < expected_peak - 2) ||
                (peak_accented > expected_peak + 2)) {
                fail("the accented note is not louder by the accent factor");
            } else {
                ok("an accent lifts the note in the same block it fires");
            }
        }

        free(live);
    }

    /* Extended synthesis controls. */
    {
        unsigned int failures_before = (unsigned int)failures;

        /* Accent gain clamps to 0.5..2.0, has a matching getter, and
         * changes the accented column. */
        mp7_init(ctx, sample_rate);
        if (pops_get_accent_gain(ctx) != MP7_PATTERN_ACCENT_GAIN_Q16) {
            fail("accent gain getter did not return the default");
        }
        if (pops_get_accent_gain(0) != 0) {
            fail("accent gain getter did not return 0 for a null context");
        }
        pops_set_accent_gain(ctx, 3 * POPS_Q16_ONE);
        if (pops_get_accent_gain(ctx) != 2 * POPS_Q16_ONE) {
            fail("accent gain is not clamped to 2.0");
        }
        pops_set_accent_gain(ctx, POPS_Q16_ONE / 4);
        if (pops_get_accent_gain(ctx) != POPS_Q16_ONE / 2) {
            fail("accent gain is not clamped to 0.5");
        }

        {
            struct pops_user_pattern accented;

            memset(&accented, 0, sizeof(accented));
            accented.timesig = POPS_TIMESIG_4_4;
            accented.notes[0] = (uint16_t)((1u << POPS_INSTR_BASS) |
                                            POPS_PATTERN_ACCENT);
            accented.notes[1] = (uint16_t)((1u << POPS_INSTR_BASS) |
                                           POPS_PATTERN_ACCENT);
            pops_set_user_pattern(ctx, 0u, &accented);
            pops_select_rhythm(ctx, POPS_BANK_A,
                              pops_rhythm_button_bit(POPS_RHYTHM_WALTZ));
            pops_set_pattern_source(ctx, POPS_PATTERN_SOURCE_USER);
            ctx->sequencer.beat_samples = 8u;

            pops_set_accent_gain(ctx, POPS_Q16_ONE);
            mp7_pattern_tick(ctx);                 /* fire column 0 */
            if (ctx->voices[POPS_INSTR_BASS].accent_gain_q16 != POPS_Q16_ONE) {
                fail("accent gain 1.0 did not take effect");
            }

            pops_set_accent_gain(ctx, POPS_Q16_ONE / 2);
            mp7_pattern_tick(ctx);                 /* fire column 1 */
            if (ctx->voices[POPS_INSTR_BASS].accent_gain_q16 !=
                POPS_Q16_ONE / 2) {
                fail("accent gain change did not reach the voice");
            }
        }

        /* Snare 1 decay multiplier. */
        mp7_init(ctx, sample_rate);
        {
            int32_t default_decay = ctx->voices[POPS_INSTR_SNARE_1].amp_env.decay_coef;

            pops_set_instr_decay(ctx, POPS_INSTR_SNARE_1, 2 * POPS_Q16_ONE);
            if (ctx->voices[POPS_INSTR_SNARE_1].amp_env.decay_coef >=
                default_decay) {
                fail("doubling Snare 1 decay did not lengthen the decay");
            }
            pops_set_instr_decay(ctx, POPS_INSTR_SNARE_1, POPS_Q16_ONE / 2);
            if (ctx->voices[POPS_INSTR_SNARE_1].amp_env.decay_coef <=
                default_decay) {
                fail("halving Snare 1 decay did not shorten the decay");
            }
        }

        pops_set_instr_decay(ctx, POPS_INSTR_SNARE_1, 16 * POPS_Q16_ONE);
        if (pops_get_instr_decay(ctx, POPS_INSTR_SNARE_1) != 5 * POPS_Q16_ONE) {
            fail("Snare 1 decay multiplier is not clamped to 5.0");
        }
        pops_set_instr_decay(ctx, POPS_INSTR_SNARE_1, POPS_Q16_ONE / 16);
        if (pops_get_instr_decay(ctx, POPS_INSTR_SNARE_1) != POPS_Q16_ONE / 5) {
            fail("Snare 1 decay multiplier is not clamped to 0.2");
        }

        /* Snare 1 band-pass frequency. */
        mp7_init(ctx, sample_rate);
        {
            int32_t default_b0 = ctx->voices[POPS_INSTR_SNARE_1].bandpass.b0;

            pops_set_instr_cutoff(ctx, POPS_INSTR_SNARE_1, 10000u);
            if (pops_get_instr_cutoff(ctx, POPS_INSTR_SNARE_1) != 10000u) {
                fail("Snare 1 cut-off frequency was not stored");
            }
            if (ctx->voices[POPS_INSTR_SNARE_1].bandpass.b0 == default_b0) {
                fail("Snare 1 cut-off frequency change did not update the bandpass");
            }

            pops_set_instr_cutoff(ctx, POPS_INSTR_SNARE_1, 500u);
            if (pops_get_instr_cutoff(ctx, POPS_INSTR_SNARE_1) != 1000u) {
                fail("Snare 1 cut-off frequency is not clamped to the minimum");
            }

            pops_set_instr_cutoff(ctx, POPS_INSTR_SNARE_1, 20000u);
            if (pops_get_instr_cutoff(ctx, POPS_INSTR_SNARE_1) != 15000u) {
                fail("Snare 1 cut-off frequency is not clamped to the maximum");
            }
        }

        /* Hihat band-pass frequency. The high-pass stays at 1000 Hz. */
        mp7_init(ctx, sample_rate);
        {
            int32_t default_b0 = ctx->voices[POPS_INSTR_HIHAT].bandpass.b0;
            int32_t default_hp = ctx->voices[POPS_INSTR_HIHAT].highpass.b0;

            pops_set_instr_cutoff(ctx, POPS_INSTR_HIHAT, 5000u);
            if (pops_get_instr_cutoff(ctx, POPS_INSTR_HIHAT) != 5000u) {
                fail("Hihat cut-off frequency was not stored");
            }
            if (ctx->voices[POPS_INSTR_HIHAT].bandpass.b0 == default_b0) {
                fail("Hihat cut-off frequency change did not update the bandpass");
            }
            if (ctx->voices[POPS_INSTR_HIHAT].highpass.b0 != default_hp) {
                fail("Hihat cut-off frequency change moved the high-pass");
            }

            pops_set_instr_cutoff(ctx, POPS_INSTR_HIHAT, 20000u);
            if (pops_get_instr_cutoff(ctx, POPS_INSTR_HIHAT) != 15000u) {
                fail("Hihat cut-off frequency is not clamped to the maximum");
            }

            pops_set_instr_cutoff(ctx, POPS_INSTR_HIHAT, 500u);
            if (pops_get_instr_cutoff(ctx, POPS_INSTR_HIHAT) != 2000u) {
                fail("Hihat cut-off frequency is not clamped to the minimum");
            }
        }

        /* Snare 2 decay multiplier and band-pass cut-off. */
        mp7_init(ctx, sample_rate);
        {
            int32_t default_decay = ctx->voices[POPS_INSTR_SNARE_2].amp_env.decay_coef;
            int32_t default_b0 = ctx->voices[POPS_INSTR_SNARE_2].bandpass.b0;

            pops_set_instr_decay(ctx, POPS_INSTR_SNARE_2, 2 * POPS_Q16_ONE);
            if (ctx->voices[POPS_INSTR_SNARE_2].amp_env.decay_coef >= default_decay) {
                fail("doubling Snare 2 decay did not lengthen the decay");
            }

            pops_set_instr_decay(ctx, POPS_INSTR_SNARE_2, 16 * POPS_Q16_ONE);
            if (pops_get_instr_decay(ctx, POPS_INSTR_SNARE_2) != 5 * POPS_Q16_ONE) {
                fail("Snare 2 decay multiplier is not clamped to 5.0");
            }
            pops_set_instr_decay(ctx, POPS_INSTR_SNARE_2, POPS_Q16_ONE / 16);
            if (pops_get_instr_decay(ctx, POPS_INSTR_SNARE_2) != POPS_Q16_ONE / 5) {
                fail("Snare 2 decay multiplier is not clamped to 0.2");
            }

            pops_set_instr_cutoff(ctx, POPS_INSTR_SNARE_2, 10000u);
            if (pops_get_instr_cutoff(ctx, POPS_INSTR_SNARE_2) != 10000u) {
                fail("Snare 2 cut-off frequency was not stored");
            }
            if (ctx->voices[POPS_INSTR_SNARE_2].bandpass.b0 == default_b0) {
                fail("Snare 2 cut-off frequency change did not update the bandpass");
            }

            pops_set_instr_cutoff(ctx, POPS_INSTR_SNARE_2, 500u);
            if (pops_get_instr_cutoff(ctx, POPS_INSTR_SNARE_2) != 1000u) {
                fail("Snare 2 cut-off frequency is not clamped to the minimum");
            }

            pops_set_instr_cutoff(ctx, POPS_INSTR_SNARE_2, 20000u);
            if (pops_get_instr_cutoff(ctx, POPS_INSTR_SNARE_2) != 15000u) {
                fail("Snare 2 cut-off frequency is not clamped to the maximum");
            }
        }

        /* The Cymbal high-passes. The band-pass stays at its fixed
         * factory center; the factory corner is the top of the range. */
        mp7_init(ctx, sample_rate);
        {
            int32_t default_hp = ctx->voices[POPS_INSTR_CYMBAL_1].highpass.b0;
            int32_t default_bp = ctx->voices[POPS_INSTR_CYMBAL_1].bandpass.b0;

            pops_set_instr_highpass(ctx, POPS_INSTR_CYMBAL_1, 2000u);
            if (pops_get_instr_highpass(ctx, POPS_INSTR_CYMBAL_1) != 2000u) {
                fail("Cymbal 1 high-pass frequency was not stored");
            }
            if (ctx->voices[POPS_INSTR_CYMBAL_1].highpass.b0 == default_hp) {
                fail("Cymbal 1 high-pass frequency change did not update the high-pass");
            }
            if (ctx->voices[POPS_INSTR_CYMBAL_1].bandpass.b0 != default_bp) {
                fail("Cymbal 1 high-pass frequency change moved the band-pass");
            }

            pops_set_instr_highpass(ctx, POPS_INSTR_CYMBAL_1, 100u);
            if (pops_get_instr_highpass(ctx, POPS_INSTR_CYMBAL_1) != 2000u) {
                fail("Cymbal 1 high-pass frequency is not clamped to the minimum");
            }

            pops_set_instr_highpass(ctx, POPS_INSTR_CYMBAL_1, 20000u);
            if (pops_get_instr_highpass(ctx, POPS_INSTR_CYMBAL_1) != 5500u) {
                fail("Cymbal 1 high-pass frequency is not clamped to the maximum");
            }
            if (ctx->voices[POPS_INSTR_CYMBAL_1].highpass.b0 != default_hp) {
                fail("the factory Cymbal 1 high-pass frequency did not return the factory corner");
            }
        }

        /* Cymbal 2, and the instruments that carry no high-pass. */
        mp7_init(ctx, sample_rate);
        {
            pops_set_instr_highpass(ctx, POPS_INSTR_CYMBAL_2, 3000u);
            if (pops_get_instr_highpass(ctx, POPS_INSTR_CYMBAL_2) != 3000u) {
                fail("Cymbal 2 high-pass frequency was not stored");
            }

            pops_set_instr_highpass(ctx, POPS_INSTR_CYMBAL_2, 500u);
            if (pops_get_instr_highpass(ctx, POPS_INSTR_CYMBAL_2) != 2000u) {
                fail("Cymbal 2 high-pass frequency is not clamped to the minimum");
            }

            pops_set_instr_highpass(ctx, POPS_INSTR_CYMBAL_2, 20000u);
            if (pops_get_instr_highpass(ctx, POPS_INSTR_CYMBAL_2) != 5500u) {
                fail("Cymbal 2 high-pass frequency is not clamped to the maximum");
            }

            pops_set_instr_highpass(ctx, POPS_INSTR_HIHAT, 5000u);
            pops_set_instr_highpass(ctx, POPS_INSTR_SNARE_1, 5000u);
            pops_set_instr_highpass(ctx, POPS_INSTR_TAMBOURINE, 5000u);
            pops_set_instr_highpass(ctx, POPS_INSTR_BASS, 5000u);
            if ((pops_get_instr_highpass(ctx, POPS_INSTR_HIHAT) != 0u) ||
                (pops_get_instr_highpass(ctx, POPS_INSTR_SNARE_1) != 0u) ||
                (pops_get_instr_highpass(ctx, POPS_INSTR_TAMBOURINE) != 0u) ||
                (pops_get_instr_highpass(ctx, POPS_INSTR_BASS) != 0u)) {
                fail("a high-pass change was stored for an instrument that has none");
            }
        }

        /* Hihat decay multiplier. */
        mp7_init(ctx, sample_rate);
        {
            int32_t default_decay = ctx->voices[POPS_INSTR_HIHAT].amp_env.decay_coef;

            pops_set_instr_decay(ctx, POPS_INSTR_HIHAT, 2 * POPS_Q16_ONE);
            if (ctx->voices[POPS_INSTR_HIHAT].amp_env.decay_coef >= default_decay) {
                fail("doubling Hihat decay did not lengthen the decay");
            }

            pops_set_instr_decay(ctx, POPS_INSTR_HIHAT, 16 * POPS_Q16_ONE);
            if (pops_get_instr_decay(ctx, POPS_INSTR_HIHAT) != 5 * POPS_Q16_ONE) {
                fail("Hihat decay multiplier is not clamped to 5.0");
            }
            pops_set_instr_decay(ctx, POPS_INSTR_HIHAT, POPS_Q16_ONE / 16);
            if (pops_get_instr_decay(ctx, POPS_INSTR_HIHAT) != POPS_Q16_ONE / 5) {
                fail("Hihat decay multiplier is not clamped to 0.2");
            }
        }

        /* The decay scales the factory decay of every instrument that has
         * one, engine by engine. The untouched Cowbell identity, the 200 ms
         * Bass, the 250 ms + 4 ms Conga pair, the 300 ms Quijada body and
         * the 200 ms Cymbal 1 pair cover the sine, dual-tone, Quijada and
         * noise engines; the snare and the Hihat cover the noise engine
         * above. */
        mp7_init(ctx, sample_rate);
        {
            if (ctx->voices[POPS_INSTR_COWBELL].amp_env.decay_coef !=
                pops_env_coef_us(50000u, sample_rate)) {
                fail("the 1.0 decay did not leave the Cowbell at its factory decay");
            }
            if (ctx->voices[POPS_INSTR_COWBELL].aux_env.decay_coef !=
                pops_env_coef_us(4000u, sample_rate)) {
                fail("the 1.0 decay did not leave the Cowbell's short tone alone");
            }

            pops_set_instr_decay(ctx, POPS_INSTR_BASS, 2 * POPS_Q16_ONE);
            if (ctx->voices[POPS_INSTR_BASS].amp_env.decay_coef !=
                pops_env_coef_us(400000u, sample_rate)) {
                fail("doubling the Bass decay did not double its decay time");
            }

            pops_set_instr_decay(ctx, POPS_INSTR_CONGA, POPS_Q16_ONE / 2);
            if (ctx->voices[POPS_INSTR_CONGA].amp_env.decay_coef !=
                pops_env_coef_us(125000u, sample_rate)) {
                fail("halving the Conga decay did not halve its body decay");
            }
            if (ctx->voices[POPS_INSTR_CONGA].aux_env.decay_coef !=
                pops_env_coef_us(2000u, sample_rate)) {
                fail("halving the Conga decay did not halve its strike decay");
            }

            pops_set_instr_decay(ctx, POPS_INSTR_QUIJADA, 2 * POPS_Q16_ONE);
            if (ctx->voices[POPS_INSTR_QUIJADA].amp_env.decay_coef !=
                pops_env_coef_us(600000u, sample_rate)) {
                fail("doubling the Quijada decay did not double its body decay");
            }

            pops_set_instr_decay(ctx, POPS_INSTR_CYMBAL_1, 2 * POPS_Q16_ONE);
            if (ctx->voices[POPS_INSTR_CYMBAL_1].amp_env.decay_coef !=
                pops_env_coef_us(400000u, sample_rate)) {
                fail("doubling the Cymbal decay did not double its amplitude decay");
            }
            if (ctx->voices[POPS_INSTR_CYMBAL_1].aux_env.decay_coef !=
                pops_env_coef_us(400000u, sample_rate)) {
                fail("doubling the Cymbal decay did not double its tone decay");
            }
        }

        /* Every parameter-bearing instrument gets a slot of its own, and
         * the slots enumerate 0..MP7_PARAM_COUNT-1 with none missing and
         * none repeated. */
        {
            unsigned int seen[MP7_PARAM_COUNT];
            unsigned int i;
            unsigned int j;

            for (j = 0u; j < MP7_PARAM_COUNT; j++) {
                seen[j] = 0u;
            }
            for (i = 0u; i < (unsigned int)POPS_INSTR_COUNT; i++) {
                if (mp7_instr_params[i].slot == MP7_PARAM_SLOT_NONE) {
                    continue;
                }
                if ((mp7_instr_params[i].slot >= MP7_PARAM_COUNT) ||
                    (seen[mp7_instr_params[i].slot]++ != 0u)) {
                    fail("the instrument parameter slots are not a clean 0..MP7_PARAM_COUNT-1");
                    break;
                }
            }
        }

        /* The Guiro is the one instrument with neither parameter: its
         * length is the gate the rhythm holds open, not a decay, and it
         * has no band-pass. A call for it must reach neither of its
         * envelopes, gate and body ring alike. */
        mp7_init(ctx, sample_rate);
        {
            int32_t guiro_amp = ctx->voices[POPS_INSTR_GUIRO].amp_env.decay_coef;
            int32_t guiro_body = ctx->voices[POPS_INSTR_GUIRO].body_env.decay_coef;

            pops_set_instr_decay(ctx, POPS_INSTR_GUIRO, 3 * POPS_Q16_ONE);
            pops_set_instr_cutoff(ctx, POPS_INSTR_GUIRO, 3000u);
            if ((pops_get_instr_decay(ctx, POPS_INSTR_GUIRO) != 0) ||
                (pops_get_instr_cutoff(ctx, POPS_INSTR_GUIRO) != 0u)) {
                fail("a parameter was stored for an instrument that has none");
            }
            if ((ctx->voices[POPS_INSTR_GUIRO].amp_env.decay_coef != guiro_amp) ||
                (ctx->voices[POPS_INSTR_GUIRO].body_env.decay_coef != guiro_body)) {
                fail("a parameter change reached the Guiro's gate or body ring");
            }

            /* The Bass has a decay but no cut-off. */
            pops_set_instr_cutoff(ctx, POPS_INSTR_BASS, 3000u);
            if (pops_get_instr_cutoff(ctx, POPS_INSTR_BASS) != 0u) {
                fail("a cut-off change was stored for an instrument that has none");
            }
        }

        /* The tuning multiplier scales what is pitched in the voice. */
        mp7_init(ctx, sample_rate);
        {
            struct pops_osc osc;
            unsigned int default_body;

            pops_osc_set_freq(&osc, 65u, sample_rate);
            default_body = ctx->voices[POPS_INSTR_BASS].body_osc.inc;
            if (default_body != osc.inc) {
                fail("the 1.0 tuning did not leave the Bass at its factory pitch");
            }

            pops_set_instr_tune(ctx, POPS_INSTR_BASS, 2 * POPS_Q16_ONE);
            pops_osc_set_freq(&osc, 130u, sample_rate);
            if (ctx->voices[POPS_INSTR_BASS].body_osc.inc != osc.inc) {
                fail("the doubled tuning did not move the Bass an octave up");
            }

            pops_set_instr_tune(ctx, POPS_INSTR_BASS, POPS_Q16_ONE / 2);
            pops_osc_set_freq(&osc, 33u, sample_rate);
            if (ctx->voices[POPS_INSTR_BASS].body_osc.inc != osc.inc) {
                fail("the halved tuning did not move the Bass an octave down");
            }
            if (pops_get_instr_tune(ctx, POPS_INSTR_BASS) != POPS_Q16_ONE / 2) {
                fail("the tuning getter did not return the stored value");
            }

            pops_set_instr_tune(ctx, POPS_INSTR_BASS, 0);
            if (pops_get_instr_tune(ctx, POPS_INSTR_BASS) != POPS_Q16_ONE / 2) {
                fail("the tuning multiplier is not clamped to 0.5");
            }
            pops_set_instr_tune(ctx, POPS_INSTR_BASS, 4 * POPS_Q16_ONE);
            if (pops_get_instr_tune(ctx, POPS_INSTR_BASS) != 2 * POPS_Q16_ONE) {
                fail("the tuning multiplier is not clamped to 2.0");
            }
        }

        /* The dual-tone voices move both oscillators. */
        mp7_init(ctx, sample_rate);
        {
            struct pops_osc osc;

            pops_set_instr_tune(ctx, POPS_INSTR_COWBELL, 2 * POPS_Q16_ONE);
            pops_osc_set_freq(&osc, 1720u, sample_rate);
            if (ctx->voices[POPS_INSTR_COWBELL].body_osc.inc != osc.inc) {
                fail("the doubled tuning did not move the Cowbell's low tone");
            }
            pops_osc_set_freq(&osc, 9200u, sample_rate);
            if (ctx->voices[POPS_INSTR_COWBELL].aux_osc.inc != osc.inc) {
                fail("the doubled tuning did not move the Cowbell's high tone");
            }

            pops_set_instr_tune(ctx, POPS_INSTR_COWBELL, POPS_Q16_ONE / 2);
            pops_osc_set_freq(&osc, 430u, sample_rate);
            if (ctx->voices[POPS_INSTR_COWBELL].body_osc.inc != osc.inc) {
                fail("the halved tuning did not move the Cowbell's low tone");
            }
            pops_osc_set_freq(&osc, 2300u, sample_rate);
            if (ctx->voices[POPS_INSTR_COWBELL].aux_osc.inc != osc.inc) {
                fail("the halved tuning did not move the Cowbell's high tone");
            }
        }

        /* The Guiro's ratchet is timing, not pitch, and stays put. */
        mp7_init(ctx, sample_rate);
        {
            struct pops_osc osc;
            unsigned int default_rate = ctx->voices[POPS_INSTR_GUIRO].aux_osc.inc;

            pops_set_instr_tune(ctx, POPS_INSTR_GUIRO, 2 * POPS_Q16_ONE);
            pops_osc_set_freq(&osc, 8200u, sample_rate);
            if (ctx->voices[POPS_INSTR_GUIRO].body_osc.inc != osc.inc) {
                fail("the doubled tuning did not move the Guiro's body tone");
            }
            if (ctx->voices[POPS_INSTR_GUIRO].aux_osc.inc != default_rate) {
                fail("the doubled tuning moved the Guiro's ratchet");
            }
        }

        /* The noise voices have no oscillator: the tuning scales the
         * band-pass centre, composed with the cut-off, and moves the
         * high-pass with it. The 102, 512 and 154 resonances mirror the
         * Snare 1, Hihat and Cymbal 1 rows of the voice table. */
        mp7_init(ctx, sample_rate);
        {
            struct pops_biquad oracle;

            pops_set_instr_tune(ctx, POPS_INSTR_SNARE_1, 2 * POPS_Q16_ONE);
            pops_biquad_set_bandpass(&oracle, 11000u, 102u, sample_rate);
            if (!biquad_coeffs_match(&ctx->voices[POPS_INSTR_SNARE_1].bandpass,
                                     &oracle)) {
                fail("the doubled tuning did not move the Snare 1 band-pass up");
            }

            /* A later cut-off composes with the tuning: 5500 Hz at 2.0 is
             * the centre 11000 Hz is at 1.0. */
            pops_set_instr_cutoff(ctx, POPS_INSTR_SNARE_1, 5500u);
            if (!biquad_coeffs_match(&ctx->voices[POPS_INSTR_SNARE_1].bandpass,
                                     &oracle)) {
                fail("the cut-off did not compose with the tuning multiplier");
            }

            pops_set_instr_tune(ctx, POPS_INSTR_SNARE_1, POPS_Q16_ONE);
            pops_set_instr_cutoff(ctx, POPS_INSTR_SNARE_1, 11000u);
            if (!biquad_coeffs_match(&ctx->voices[POPS_INSTR_SNARE_1].bandpass,
                                     &oracle)) {
                fail("a 1.0 tuning did not play the stored cut-off");
            }

            /* 20000 Hz is beyond two fifths of 44100 Hz, so the change is
             * limited to 17640 Hz. */
            pops_set_instr_tune(ctx, POPS_INSTR_HIHAT, 2 * POPS_Q16_ONE);
            {
                uint32_t limit = (uint32_t)(((uint64_t)sample_rate * 2u) / 5u);

                pops_biquad_set_bandpass(&oracle, limit, 512u, sample_rate);
                if (!biquad_coeffs_match(&ctx->voices[POPS_INSTR_HIHAT].bandpass,
                                         &oracle)) {
                    fail("the doubled Hihat tuning was not limited to the rate");
                }
            }

            pops_set_instr_tune(ctx, POPS_INSTR_HIHAT, POPS_Q16_ONE / 2);
            pops_biquad_set_bandpass(&oracle, 5000u, 512u, sample_rate);
            if (!biquad_coeffs_match(&ctx->voices[POPS_INSTR_HIHAT].bandpass,
                                     &oracle)) {
                fail("the halved tuning did not move the Hihat band-pass down");
            }

            /* The Cymbal has no cut-off parameter, but its factory
             * band-pass still follows the tuning. */
            pops_set_instr_tune(ctx, POPS_INSTR_CYMBAL_1, POPS_Q16_ONE / 2);
            pops_biquad_set_bandpass(&oracle, 2800u, 154u, sample_rate);
            if (!biquad_coeffs_match(&ctx->voices[POPS_INSTR_CYMBAL_1].bandpass,
                                     &oracle)) {
                fail("the halved tuning did not move the Cymbal band-pass down");
            }

            /* The high-passes follow the tuning as well, so the whole
             * voice transposes. The Cymbal's factory corner sits a
             * hundred Hz below its band-pass and the Tambourine's above
             * its own. */
            pops_biquad_set_highpass_coeffs(&oracle, 2750u, sample_rate);
            if (!biquad_coeffs_match(&ctx->voices[POPS_INSTR_CYMBAL_1].highpass,
                                     &oracle)) {
                fail("the halved tuning did not move the Cymbal high-pass down");
            }

            /* A stored high-pass composes with the tuning the way the
             * cut-off composes with it: 3000 Hz at 2.0 is the corner
             * 6000 Hz is at 1.0. */
            pops_set_instr_tune(ctx, POPS_INSTR_CYMBAL_1, 2 * POPS_Q16_ONE);
            pops_set_instr_highpass(ctx, POPS_INSTR_CYMBAL_1, 3000u);
            pops_biquad_set_highpass_coeffs(&oracle, 6000u, sample_rate);
            if (!biquad_coeffs_match(&ctx->voices[POPS_INSTR_CYMBAL_1].highpass,
                                     &oracle)) {
                fail("the high-pass did not compose with the tuning multiplier");
            }

            pops_set_instr_tune(ctx, POPS_INSTR_CYMBAL_1, POPS_Q16_ONE);
            pops_biquad_set_highpass_coeffs(&oracle, 3000u, sample_rate);
            if (!biquad_coeffs_match(&ctx->voices[POPS_INSTR_CYMBAL_1].highpass,
                                     &oracle)) {
                fail("the 1.0 tuning did not play the stored high-pass");
            }

            /* The Tambourine's factory high-pass sits above its factory
             * band-pass: a fixed high-pass would take the tuned-down
             * band-pass below it and starve the voice of output. */
            pops_set_instr_tune(ctx, POPS_INSTR_TAMBOURINE, POPS_Q16_ONE / 2);
            pops_biquad_set_highpass_coeffs(&oracle, 3300u, sample_rate);
            if (!biquad_coeffs_match(&ctx->voices[POPS_INSTR_TAMBOURINE].highpass,
                                     &oracle)) {
                fail("the halved tuning did not move the Tambourine high-pass down");
            }
            pops_biquad_set_bandpass(&oracle, 2300u, 2560u, sample_rate);
            if (!biquad_coeffs_match(&ctx->voices[POPS_INSTR_TAMBOURINE].bandpass,
                                     &oracle)) {
                fail("the halved tuning did not move the Tambourine band-pass down");
            }

            /* Back at 1.0 the high-pass returns to its factory value. */
            pops_set_instr_tune(ctx, POPS_INSTR_TAMBOURINE, POPS_Q16_ONE);
            pops_biquad_set_highpass_coeffs(&oracle, 6600u, sample_rate);
            if (!biquad_coeffs_match(&ctx->voices[POPS_INSTR_TAMBOURINE].highpass,
                                     &oracle)) {
                fail("the 1.0 tuning did not leave the Tambourine high-pass alone");
            }
        }

        /* At 32000 Hz the doubled Tambourine high-pass (13200 Hz) runs
         * into the stability limit and is clamped to two fifths of the
         * rate, 12800 Hz. */
        mp7_init(ctx, 32000u);
        {
            struct pops_biquad oracle;

            pops_set_instr_tune(ctx, POPS_INSTR_TAMBOURINE, 2 * POPS_Q16_ONE);
            pops_biquad_set_highpass_coeffs(&oracle, 12800u, 32000u);
            if (!biquad_coeffs_match(&ctx->voices[POPS_INSTR_TAMBOURINE].highpass,
                                     &oracle)) {
                fail("the doubled Tambourine high-pass was not limited to the rate");
            }
            pops_biquad_set_bandpass(&oracle, 9200u, 2560u, 32000u);
            if (!biquad_coeffs_match(&ctx->voices[POPS_INSTR_TAMBOURINE].bandpass,
                                     &oracle)) {
                fail("the doubled tuning did not move the Tambourine band-pass up");
            }
        }

        /* A sample-rate change keeps the tuning and re-derives the
         * increments for the new rate. */
        mp7_init(ctx, sample_rate);
        {
            struct pops_osc osc;

            pops_set_instr_tune(ctx, POPS_INSTR_BASS, 2 * POPS_Q16_ONE);
            pops_set_sample_rate(ctx, 32000u);
            pops_osc_set_freq(&osc, 130u, 32000u);
            if (ctx->voices[POPS_INSTR_BASS].body_osc.inc != osc.inc) {
                fail("a sample-rate change lost the tuning multiplier");
            }
        }

        /* The getters mirror the setters, and answer 0 for a null
         * context. */
        mp7_init(ctx, sample_rate);
        if (pops_get_instr_decay(ctx, POPS_INSTR_SNARE_1) != POPS_Q16_ONE) {
            fail("the decay getter did not return the default");
        }
        if (pops_get_instr_cutoff(ctx, POPS_INSTR_SNARE_1) != MP7_SNARE1_BANDPASS_HZ) {
            fail("the cut-off getter did not return the default");
        }
        if (pops_get_instr_cutoff(ctx, POPS_INSTR_HIHAT) != MP7_HIHAT_BANDPASS_HZ) {
            fail("the Hihat cut-off getter did not return the default");
        }
        if (pops_get_instr_highpass(ctx, POPS_INSTR_CYMBAL_1) != MP7_CYMBAL1_HIGHPASS_HZ) {
            fail("the Cymbal 1 high-pass getter did not return the default");
        }
        if (pops_get_instr_highpass(ctx, POPS_INSTR_CYMBAL_2) != MP7_CYMBAL2_HIGHPASS_HZ) {
            fail("the Cymbal 2 high-pass getter did not return the default");
        }
        if ((pops_get_instr_decay(0, POPS_INSTR_SNARE_1) != 0) ||
            (pops_get_instr_cutoff(0, POPS_INSTR_SNARE_1) != 0u) ||
            (pops_get_instr_highpass(0, POPS_INSTR_CYMBAL_1) != 0u)) {
            fail("the getters did not return 0 for a null context");
        }

        /* pops_reset() restores the synthesis defaults. */
        pops_set_accent_gain(ctx, POPS_Q16_ONE / 2);
        pops_set_instr_decay(ctx, POPS_INSTR_SNARE_1, 4 * POPS_Q16_ONE);
        pops_set_instr_decay(ctx, POPS_INSTR_SNARE_2, 4 * POPS_Q16_ONE);
        pops_set_instr_decay(ctx, POPS_INSTR_HIHAT, 4 * POPS_Q16_ONE);
        pops_set_instr_decay(ctx, POPS_INSTR_BASS, 3 * POPS_Q16_ONE);
        pops_set_instr_decay(ctx, POPS_INSTR_TAMBOURINE, POPS_Q16_ONE / 4);
        pops_set_instr_cutoff(ctx, POPS_INSTR_SNARE_1, 10000u);
        pops_set_instr_cutoff(ctx, POPS_INSTR_SNARE_2, 10000u);
        pops_set_instr_cutoff(ctx, POPS_INSTR_HIHAT, 5000u);
        pops_set_instr_highpass(ctx, POPS_INSTR_CYMBAL_1, 2000u);
        pops_set_instr_highpass(ctx, POPS_INSTR_CYMBAL_2, 2000u);
        pops_set_instr_tune(ctx, POPS_INSTR_BASS, 2 * POPS_Q16_ONE);
        pops_set_instr_tune(ctx, POPS_INSTR_HIHAT, POPS_Q16_ONE / 2);
        pops_reset(ctx);
        if ((pops_get_accent_gain(ctx) != MP7_PATTERN_ACCENT_GAIN_Q16) ||
            (pops_get_instr_decay(ctx, POPS_INSTR_SNARE_1) != POPS_Q16_ONE) ||
            (pops_get_instr_decay(ctx, POPS_INSTR_SNARE_2) != POPS_Q16_ONE) ||
            (pops_get_instr_decay(ctx, POPS_INSTR_HIHAT) != POPS_Q16_ONE) ||
            (pops_get_instr_decay(ctx, POPS_INSTR_BASS) != POPS_Q16_ONE) ||
            (pops_get_instr_decay(ctx, POPS_INSTR_TAMBOURINE) != POPS_Q16_ONE) ||
            (pops_get_instr_cutoff(ctx, POPS_INSTR_SNARE_1) != MP7_SNARE1_BANDPASS_HZ) ||
            (pops_get_instr_cutoff(ctx, POPS_INSTR_SNARE_2) != MP7_SNARE2_BANDPASS_HZ) ||
            (pops_get_instr_cutoff(ctx, POPS_INSTR_HIHAT) != MP7_HIHAT_BANDPASS_HZ) ||
            (pops_get_instr_highpass(ctx, POPS_INSTR_CYMBAL_1) != MP7_CYMBAL1_HIGHPASS_HZ) ||
            (pops_get_instr_highpass(ctx, POPS_INSTR_CYMBAL_2) != MP7_CYMBAL2_HIGHPASS_HZ) ||
            (pops_get_instr_tune(ctx, POPS_INSTR_BASS) != POPS_Q16_ONE) ||
            (pops_get_instr_tune(ctx, POPS_INSTR_HIHAT) != POPS_Q16_ONE)) {
            fail("pops_reset() did not restore the extended synthesis defaults");
        } else {
            ok("pops_reset() restores the extended synthesis defaults");
        }

        /* A null context is ignored by every setter. */
        pops_set_accent_gain(0, POPS_Q16_ONE);
        pops_set_instr_decay(0, POPS_INSTR_SNARE_1, POPS_Q16_ONE);
        pops_set_instr_decay(0, POPS_INSTR_SNARE_2, POPS_Q16_ONE);
        pops_set_instr_decay(0, POPS_INSTR_HIHAT, POPS_Q16_ONE);
        pops_set_instr_cutoff(0, POPS_INSTR_SNARE_1, 1000u);
        pops_set_instr_cutoff(0, POPS_INSTR_SNARE_2, 1000u);
        pops_set_instr_cutoff(0, POPS_INSTR_HIHAT, 1000u);
        pops_set_instr_highpass(0, POPS_INSTR_CYMBAL_1, 1000u);
        pops_set_instr_highpass(0, POPS_INSTR_CYMBAL_2, 1000u);
        pops_set_instr_tune(0, POPS_INSTR_BASS, POPS_Q16_ONE);
        if (pops_get_instr_tune(0, POPS_INSTR_BASS) != 0) {
            fail("the tuning getter did not return 0 for a null context");
        }
        ok("the extended synthesis setters ignore a null context");

        if ((unsigned int)failures == failures_before) {
            ok("extended synthesis controls clamp, apply and reset");
        }
    }

    /* A decay change must not cut off a note that is already sounding. */
    {
        struct mp7_context_s *live = context_new();
        struct mp7_voice *snare = &live->voices[POPS_INSTR_SNARE_1];
        unsigned int i;
        int still_playing = 1;

        mp7_init(live, 44100);
        mp7_voice_trigger(snare);
        for (i = 0u; i < 2000u; i++) {
            mp7_voice_process(snare);
        }

        /* The note is well into its decay when the change arrives. */
        if (snare->amp_env.stage != POPS_ENV_DECAY) {
            fail("the Snare 1 note did not reach its decay stage");
            still_playing = 0;
        }

        pops_set_instr_decay(live, POPS_INSTR_SNARE_1, 2 * POPS_Q16_ONE);

        if (still_playing) {
            if (!snare->active) {
                fail("a decay change deactivated the Snare 1 voice");
                still_playing = 0;
            } else if (snare->amp_env.stage != POPS_ENV_DECAY) {
                fail("a decay change reset the Snare 1 envelope");
                still_playing = 0;
            } else if (mp7_voice_process(snare) == 0) {
                fail("a decay change silenced the Snare 1 note");
                still_playing = 0;
            }
        }

        if (still_playing) {
            ok("a decay change does not cut off a sounding note");
        }

        free(live);
    }

    /* A tuning change must not cut off a note that is already sounding:
     * the note follows the new pitch from the next sample. */
    {
        struct mp7_context_s *live = context_new();
        struct mp7_voice *bass = &live->voices[POPS_INSTR_BASS];
        unsigned int i;
        int still_playing = 1;

        mp7_init(live, 44100);
        mp7_voice_trigger(bass);
        for (i = 0u; i < 2000u; i++) {
            mp7_voice_process(bass);
        }

        if (bass->amp_env.stage != POPS_ENV_DECAY) {
            fail("the Bass note did not reach its decay stage");
            still_playing = 0;
        }

        pops_set_instr_tune(live, POPS_INSTR_BASS, 2 * POPS_Q16_ONE);

        if (still_playing) {
            if (!bass->active) {
                fail("a tuning change deactivated the Bass voice");
                still_playing = 0;
            } else if (bass->amp_env.stage != POPS_ENV_DECAY) {
                fail("a tuning change reset the Bass envelope");
                still_playing = 0;
            } else if (mp7_voice_process(bass) == 0) {
                fail("a tuning change silenced the Bass note");
                still_playing = 0;
            } else {
                ok("a tuning change does not cut off a sounding note");
            }
        }

        free(live);
    }

    /* The cut-offs must stay below the sample rate's Nyquist frequency at
     * any rate, or the biquad becomes unstable and the voice buzzes. */
    {
        static const uint32_t rates[] = {8000u, 16000u, 22050u, 32000u,
                                          44100u, 48000u};
        unsigned int r;
        int all_stable = 1;

        for (r = 0u; r < (sizeof(rates) / sizeof(rates[0])); r++) {
            struct mp7_context_s *low = context_new();
            double bp_pole;

            mp7_init(low, rates[r]);
            pops_set_instr_cutoff(low, POPS_INSTR_SNARE_1, 15000u);
            pops_set_instr_cutoff(low, POPS_INSTR_SNARE_2, 15000u);
            pops_set_instr_cutoff(low, POPS_INSTR_HIHAT, 15000u);
            pops_set_instr_highpass(low, POPS_INSTR_CYMBAL_1, 5500u);
            pops_set_instr_highpass(low, POPS_INSTR_CYMBAL_2, 5500u);
            pops_set_instr_tune(low, POPS_INSTR_SNARE_1, 2 * POPS_Q16_ONE);
            pops_set_instr_tune(low, POPS_INSTR_HIHAT, 2 * POPS_Q16_ONE);
            pops_set_instr_tune(low, POPS_INSTR_CYMBAL_1, 2 * POPS_Q16_ONE);
            pops_set_instr_tune(low, POPS_INSTR_CYMBAL_2, 2 * POPS_Q16_ONE);

            /* The cut-off moves the band-pass of all three instruments;
             * the Hihat's high-pass stays at its factory value, and the
             * cymbal high-passes carry the tuning too. */
            bp_pole = biquad_pole_radius(&low->voices[POPS_INSTR_SNARE_1].bandpass);
            if (biquad_pole_radius(&low->voices[POPS_INSTR_SNARE_2].bandpass) >= 1.0) {
                fail("a cut-off above Nyquist made the Snare 2 biquad unstable");
                all_stable = 0;
            }
            if (biquad_pole_radius(&low->voices[POPS_INSTR_HIHAT].bandpass) >= 1.0) {
                fail("a cut-off above Nyquist made the Hihat biquad unstable");
                all_stable = 0;
            }
            if (biquad_pole_radius(&low->voices[POPS_INSTR_HIHAT].highpass) >= 1.0) {
                fail("the fixed Hihat high-pass became unstable");
                all_stable = 0;
            }
            if (biquad_pole_radius(&low->voices[POPS_INSTR_CYMBAL_1].highpass) >= 1.0) {
                fail("the tuned Cymbal high-pass became unstable");
                all_stable = 0;
            }
            if (biquad_pole_radius(&low->voices[POPS_INSTR_CYMBAL_2].highpass) >= 1.0) {
                fail("the tuned Cymbal 2 high-pass became unstable");
                all_stable = 0;
            }

            if (bp_pole >= 1.0) {
                fail("a cut-off above Nyquist made the biquad unstable");
                all_stable = 0;
            }
            free(low);
        }

        if (all_stable) {
            ok("the cut-offs and the cymbal high-passes stay stable at every sample rate");
        }
    }

    free(ctx);
    return failures == 0 ? 0 : 1;
}

#else /* !LIBPOPS_EXTRA */

static int command_user(void)
{
    printf("skip: the user pattern slots need an extended build"
           " (-DLIBPOPS_EXTRA)\n");
    return 0;
}

#endif /* LIBPOPS_EXTRA */

static int render_rhythm(struct mp7_context_s *ctx,
                         uint32_t selection,
                         int16_t *stereo,
                         size_t *frames_out)
{
    unsigned int column;
    size_t enabled = 0u;
    size_t steps;
    size_t frames;

    mp7_pattern_combine(ctx, selection);
    mp7_pattern_set_tempo(ctx, 120u * 65536u);
    steps = (ctx->sequencer.subdivision == (uint8_t)MP7_SUBDIV_COMPOUND) ? 6u : 8u;

    for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
        if ((ctx->sequencer.enable & ((uint64_t)1u << column)) != 0u) {
            enabled++;
        }
    }
    /* A column is a beat divided by the columns per beat, and the beat does
     * not divide evenly into them, so the longest column is one sample more
     * than the average. */
    frames = enabled * ((size_t)ctx->sequencer.beat_samples / steps + 1u);

    pops_process(ctx, stereo, frames);
    *frames_out = frames;
    return (int)enabled;
}

static int command_rhythms(const char *dir)
{
    struct mp7_context_s *ctx = context_new();
    const uint32_t sample_rate = POPS_DEFAULT_SAMPLE_RATE;
    size_t max_frames = (size_t)(sample_rate * 5u);
    int16_t *stereo = (int16_t *)malloc(max_frames * 2u * sizeof(int16_t));
    unsigned int rhythm;
    unsigned int failures_before = (unsigned int)failures;

    if (stereo == 0) {
        printf("FAIL: out of memory\n");
        free(ctx);
        return 1;
    }

    for (rhythm = 0u; rhythm < (unsigned int)POPS_RHYTHM_COUNT; rhythm++) {
        char path[512];
        size_t frames = 0u;
        size_t i;
        int peak = 0;

        mp7_init(ctx, sample_rate);
        render_rhythm(ctx, (uint32_t)1u << rhythm, stereo, &frames);

        for (i = 0u; i < frames * 2u; i++) {
            int value = (stereo[i] < 0) ? -stereo[i] : stereo[i];

            if (value > peak) {
                peak = value;
            }
        }
        if (peak < 100) {
            printf("FAIL: %s rendered silence\n", test_rhythm_names[rhythm]);
            failures++;
        }
        if (snprintf(path, sizeof(path), "%s/rhythm_%02u.wav", dir, rhythm) >=
            (int)sizeof(path)) {
            printf("FAIL: path too long\n");
            failures++;
            continue;
        }
        if (wav_write_stereo16(path, stereo, frames, sample_rate) != 0) {
            printf("FAIL: could not write %s\n", path);
            failures++;
        }
    }

    /* Two documented combinations, for A/B listening. */
    {
        static const uint32_t selections[2] = {
            ((uint32_t)1u << POPS_RHYTHM_ROCK_1) | ((uint32_t)1u << POPS_RHYTHM_WALTZ),
            ((uint32_t)1u << POPS_RHYTHM_ROCK_1) | ((uint32_t)1u << POPS_RHYTHM_MARCH_6_8)
        };
        static const char *const names[2] = {
            "combo_rock1_waltz.wav",
            "combo_rock1_march68.wav"
        };
        unsigned int i;

        for (i = 0u; i < 2u; i++) {
            char path[512];
            size_t frames = 0u;

            mp7_init(ctx, sample_rate);
            render_rhythm(ctx, selections[i], stereo, &frames);
            if (snprintf(path, sizeof(path), "%s/%s", dir, names[i]) >=
                (int)sizeof(path)) {
                printf("FAIL: path too long\n");
                failures++;
                continue;
            }
            if (wav_write_stereo16(path, stereo, frames, sample_rate) != 0) {
                printf("FAIL: could not write %s\n", path);
                failures++;
            }
        }
    }

    if ((unsigned int)failures == failures_before) {
        ok("all 20 rhythms and both combinations render audio");
    }

    free(stereo);
    free(ctx);
    return failures == 0 ? 0 : 1;
}

static int command_quijada(void)
{
    struct mp7_context_s *ctx = context_new();
    struct pops_osc body_expected;
    struct pops_osc clock_expected;
    const int32_t low = POPS_Q30_ONE / 10;
    const int32_t high = (int32_t)(((int64_t)POPS_Q30_ONE * 6) / 10);
    struct mp7_voice *voice;
    int i;

    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    voice = &ctx->voices[POPS_INSTR_QUIJADA];

    pops_osc_set_freq(&body_expected, 2700u, POPS_DEFAULT_SAMPLE_RATE);
    pops_osc_set_freq(&clock_expected, 25u, POPS_DEFAULT_SAMPLE_RATE);
    if (voice->body_osc.inc != body_expected.inc) {
        fail("Quijada body oscillator is not 2700 Hz");
    } else {
        ok("Quijada body oscillator is 2700 Hz");
    }
    if (voice->aux_osc.inc != clock_expected.inc) {
        fail("Quijada comparator clock is not 25 Hz");
    } else {
        ok("Quijada comparator clock is 25 Hz");
    }

    for (i = 0; i < 8; i++) {
        mp7_voice_trigger(voice);
        if ((voice->threshold_lo < low) || (voice->threshold_lo > high) ||
            (voice->threshold_hi < low) || (voice->threshold_hi > high)) {
            fail("Quijada threshold is outside [0.1, 0.6]");
            break;
        }
    }
    if (i == 8) {
        ok("both Quijada thresholds are re-randomized in [0.1, 0.6]");
    }

    /* Comparator mapping: a crossing of the low threshold must fire the
     * short envelope, and a crossing of the high threshold the long one. */
    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    voice = &ctx->voices[POPS_INSTR_QUIJADA];
    voice->prev_clock = -POPS_Q30_ONE;
    voice->aux_osc.phase = (uint32_t)1024u << 16; /* sine peak */
    voice->threshold_lo = POPS_Q30_ONE / 5;      /* 0.2 */
    voice->threshold_hi = INT32_MAX;              /* above the clock peak */
    (void)mp7_engine_process(voice);
    if ((voice->short_env.stage != POPS_ENV_ATTACK) ||
        (voice->long_env.stage != POPS_ENV_IDLE)) {
        fail("low Quijada threshold does not fire the short envelope");
    } else {
        ok("low Quijada threshold fires the short envelope");
    }

    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    voice = &ctx->voices[POPS_INSTR_QUIJADA];
    voice->prev_clock = -POPS_Q30_ONE;
    voice->aux_osc.phase = (uint32_t)1024u << 16; /* sine peak */
    voice->threshold_lo = INT32_MAX;              /* above the clock peak */
    voice->threshold_hi = (int32_t)(((int64_t)POPS_Q30_ONE * 2) / 5);  /* 0.4 */
    (void)mp7_engine_process(voice);
    if ((voice->long_env.stage != POPS_ENV_ATTACK) ||
        (voice->short_env.stage != POPS_ENV_IDLE)) {
        fail("high Quijada threshold does not fire the long envelope");
    } else {
        ok("high Quijada threshold fires the long envelope");
    }

    free(ctx);
    return failures == 0 ? 0 : 1;
}

static int channel_peak(const int16_t *stereo, size_t frames, unsigned int channel)
{
    int peak = 0;
    size_t i;

    for (i = 0u; i < frames; i++) {
        int value = (int)stereo[2u * i + channel];

        if (value < 0) {
            value = -value;
        }
        if (value > peak) {
            peak = value;
        }
    }
    return peak;
}

static int stereo_is_silent(const int16_t *stereo, size_t frames)
{
    size_t i;

    for (i = 0u; i < frames * 2u; i++) {
        if (stereo[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static void render_trigger(struct mp7_context_s *ctx, pops_instr instr,
                           int16_t *stereo, size_t frames)
{
    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    mp7_voice_trigger(&ctx->voices[instr]);
    pops_process(ctx, stereo, frames);
}

static int command_mixer(void)
{
    struct mp7_context_s *ctx = context_new();
    const size_t frames = 4096u;
    int16_t *stereo = (int16_t *)malloc(frames * 2u * sizeof(int16_t));
    int16_t *other = (int16_t *)malloc(frames * 2u * sizeof(int16_t));
    unsigned int i;

    if ((stereo == 0) || (other == 0)) {
        printf("FAIL: out of memory\n");
        free(stereo);
        free(other);
        free(ctx);
        return 1;
    }

    /* Defaults: Volume 1.0 (master gain 0.5), Balance center, instrument
     * gains at 0.5 for the three Additional Volume faders and 1.0 elsewhere. */
    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    if ((ctx->mixer.panel_volume_q16 != POPS_Q16_ONE) ||
        (ctx->mixer.balance_q16 != POPS_Q16_ONE / 2)) {
        fail("the mixer defaults are not Volume 1.0 / Balance 0.5");
    }
    for (i = 0u; i < (unsigned int)POPS_INSTR_COUNT; i++) {
        int32_t expected = POPS_Q16_ONE;

        if ((i == (unsigned int)POPS_INSTR_TAMBOURINE) ||
            (i == (unsigned int)POPS_INSTR_GUIRO) ||
            (i == (unsigned int)POPS_INSTR_QUIJADA)) {
            expected = POPS_Q16_ONE / 2;
        }
        if (ctx->mixer.instr_gain_q16[i] != expected) {
            fail("an instrument gain does not have its panel default");
            break;
        }
    }
    for (i = 0u; i < (unsigned int)POPS_INSTR_COUNT; i++) {
        if (ctx->mixer.instr_pan_q16[i] != POPS_Q16_ONE / 2) {
            fail("an instrument pan does not default to center");
            break;
        }
    }
    for (i = 0u; i < (unsigned int)POPS_INSTR_COUNT; i++) {
        if ((ctx->voices[i].level_q16 != POPS_Q16_ONE) ||
            (ctx->voices[i].pan_q16 != POPS_Q16_ONE / 2)) {
            fail("a voice does not default to Level 1.0 / Pan 0.5");
            break;
        }
    }
    if (failures == 0) {
        ok("the mixer defaults to Volume 1.0, Balance 0.5 and panel gains");
    }

    /* A centered voice appears equally in both channels, and a hard pan sends
     * it entirely to one side. */
    render_trigger(ctx, POPS_INSTR_BASS, stereo, frames);
    if ((channel_peak(stereo, frames, 0u) == 0) ||
        (channel_peak(stereo, frames, 0u) != channel_peak(stereo, frames, 1u))) {
        fail("a centered voice is not equal in both channels");
    } else {
        ok("a centered voice is equal in both channels");
    }

    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    mp7_voice_trigger(&ctx->voices[POPS_INSTR_BASS]);
    ctx->voices[POPS_INSTR_BASS].pan_q16 = 0;
    pops_process(ctx, stereo, frames);
    if ((channel_peak(stereo, frames, 0u) == 0) ||
        (channel_peak(stereo, frames, 1u) != 0)) {
        fail("pan 0 is not hard left");
    } else {
        ok("pan 0 sends a voice hard left");
    }

    /* The center pan splits the voice 0.5 / 0.5, so a hard-left voice is
     * exactly twice as loud in the left channel as a centered one. Together
     * with the default master gain of 0.5 that puts a default hit at 0.25
     * per channel (-12.04 dBFS) at the anchor of +/-1.0 for a Level 1.0
     * voice. */
    {
        int centered;
        int hard_left;

        render_trigger(ctx, POPS_INSTR_BASS, stereo, frames);
        centered = channel_peak(stereo, frames, 0u);

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        mp7_voice_trigger(&ctx->voices[POPS_INSTR_BASS]);
        ctx->voices[POPS_INSTR_BASS].pan_q16 = 0;
        pops_process(ctx, stereo, frames);
        hard_left = channel_peak(stereo, frames, 0u);

        if ((centered == 0) ||
            (hard_left < (2 * centered) - 1) ||
            (hard_left > (2 * centered) + 1)) {
            printf("FAIL: the center pan does not split the voice in half (peaks %d -> %d)\n",
                   centered, hard_left);
            failures++;
        } else {
            ok("the center pan splits the voice 0.5 / 0.5");
        }
    }

    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    mp7_voice_trigger(&ctx->voices[POPS_INSTR_BASS]);
    ctx->voices[POPS_INSTR_BASS].pan_q16 = POPS_Q16_ONE;
    pops_process(ctx, stereo, frames);
    if ((channel_peak(stereo, frames, 1u) == 0) ||
        (channel_peak(stereo, frames, 0u) != 0)) {
        fail("pan 1.0 is not hard right");
    } else {
        ok("pan 1.0 sends a voice hard right");
    }

    /* A per-instrument pan moves its own voice only. */
    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_set_instr_pan(ctx, POPS_INSTR_BASS, 0);
    mp7_voice_trigger(&ctx->voices[POPS_INSTR_BASS]);
    pops_process(ctx, stereo, frames);
    if ((channel_peak(stereo, frames, 0u) == 0) ||
        (channel_peak(stereo, frames, 1u) != 0)) {
        fail("a per-instrument pan of 0 does not send its voice hard left");
    } else {
        ok("a per-instrument pan of 0 sends its voice hard left");
    }

    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_set_instr_pan(ctx, POPS_INSTR_BASS, POPS_Q16_ONE);
    mp7_voice_trigger(&ctx->voices[POPS_INSTR_BASS]);
    pops_process(ctx, stereo, frames);
    if ((channel_peak(stereo, frames, 1u) == 0) ||
        (channel_peak(stereo, frames, 0u) != 0)) {
        fail("a per-instrument pan of 1.0 does not send its voice hard right");
    } else {
        ok("a per-instrument pan of 1.0 sends its voice hard right");
    }

    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_set_instr_pan(ctx, POPS_INSTR_BASS, 0);
    mp7_voice_trigger(&ctx->voices[POPS_INSTR_SNARE_1]);
    pops_process(ctx, stereo, frames);
    if ((channel_peak(stereo, frames, 0u) == 0) ||
        (channel_peak(stereo, frames, 0u) != channel_peak(stereo, frames, 1u))) {
        fail("a per-instrument pan moved another voice");
    } else {
        ok("a per-instrument pan moves its own voice only");
    }

    /* The master Volume is applied as 0.5 * value: 1.0 is half scale, 2.0 is
     * unity, and 0 is silence. */
    if (pops_mixer_to_q15(INT32_MAX, POPS_Q16_ONE) != 16384) {
        fail("Volume 1.0 is not half the panel value as a master gain");
    } else {
        ok("Volume 1.0 applies the 0.5x master gain");
    }
    if ((pops_mixer_to_q15(INT32_MAX, 2 * POPS_Q16_ONE) != 32767) ||
        (pops_mixer_to_q15(INT32_MIN, 2 * POPS_Q16_ONE) != -32768)) {
        fail("the master gain does not saturate at full scale");
    } else {
        ok("the master gain saturates once, at the final Q15 conversion");
    }

    render_trigger(ctx, POPS_INSTR_BASS, stereo, frames);
    {
        int base_peak = channel_peak(stereo, frames, 0u);

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        pops_mixer_set_volume(&ctx->mixer, 2 * POPS_Q16_ONE);
        mp7_voice_trigger(&ctx->voices[POPS_INSTR_BASS]);
        pops_process(ctx, stereo, frames);
        {
            int doubled = channel_peak(stereo, frames, 0u);
            int expected = 2 * base_peak;

            if ((doubled < expected - 2) || (doubled > expected + 2)) {
                printf("FAIL: Volume 2.0 gave %d, expected about %d\n",
                       doubled, expected);
                failures++;
            } else {
                ok("Volume 2.0 gives unity master gain (double the default)");
            }
        }

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        pops_mixer_set_volume(&ctx->mixer, 0);
        mp7_voice_trigger(&ctx->voices[POPS_INSTR_BASS]);
        pops_process(ctx, stereo, frames);
        if (!stereo_is_silent(stereo, frames)) {
            fail("Volume 0 is not silent");
        } else {
            ok("Volume 0 mutes the mix");
        }
    }

    /* Balance is a Base-versus-cymbals crossfade. Snare 1 belongs to neither
     * group and must not move. */
    render_trigger(ctx, POPS_INSTR_BASS, stereo, frames);
    {
        int bass_bottom;
        int bass_top;

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        pops_mixer_set_balance(&ctx->mixer, 0);
        mp7_voice_trigger(&ctx->voices[POPS_INSTR_BASS]);
        pops_process(ctx, stereo, frames);
        bass_bottom = channel_peak(stereo, frames, 0u);

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        pops_mixer_set_balance(&ctx->mixer, POPS_Q16_ONE);
        mp7_voice_trigger(&ctx->voices[POPS_INSTR_BASS]);
        pops_process(ctx, stereo, frames);
        bass_top = channel_peak(stereo, frames, 0u);

        /* Balance takes the Base from gain 1.0 down to 0.2, a factor of 5,
         * and never mutes it completely. */
        {
            int expected = 5 * bass_top;
            int tolerance = expected / 10 + 2;

            if ((bass_top == 0) ||
                (bass_bottom < expected - tolerance) ||
                (bass_bottom > expected + tolerance)) {
                printf("FAIL: Balance does not attenuate Base to 0.2 (peaks %d -> %d)\n",
                       bass_bottom, bass_top);
                failures++;
            } else {
                ok("Balance attenuates Base to about 0.2 at the right");
            }
        }
    }

    render_trigger(ctx, POPS_INSTR_HIHAT, stereo, frames);
    {
        int hihat_left;
        int hihat_right;

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        pops_mixer_set_balance(&ctx->mixer, 0);
        mp7_voice_trigger(&ctx->voices[POPS_INSTR_HIHAT]);
        pops_process(ctx, stereo, frames);
        hihat_left = channel_peak(stereo, frames, 0u);

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        pops_mixer_set_balance(&ctx->mixer, POPS_Q16_ONE);
        mp7_voice_trigger(&ctx->voices[POPS_INSTR_HIHAT]);
        pops_process(ctx, stereo, frames);
        hihat_right = channel_peak(stereo, frames, 0u);

        /* The cymbal group goes from 0.2 at the left to 1.0 at the right, the
         * same factor of 5, and is never muted completely. */
        {
            int expected = 5 * hihat_left;
            int tolerance = expected / 10 + 2;

            if ((hihat_left == 0) ||
                (hihat_right < expected - tolerance) ||
                (hihat_right > expected + tolerance)) {
                printf("FAIL: Balance does not attenuate the cymbal group to 0.2 (peaks %d -> %d)\n",
                       hihat_left, hihat_right);
                failures++;
            } else {
                ok("Balance attenuates Hihat/Cymbal 1/Cymbal 2 to about 0.2 at the left");
            }
        }
    }

    render_trigger(ctx, POPS_INSTR_SNARE_1, stereo, frames);
    {
        int snare_bottom;
        int snare_top;

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        pops_mixer_set_balance(&ctx->mixer, 0);
        mp7_voice_trigger(&ctx->voices[POPS_INSTR_SNARE_1]);
        pops_process(ctx, stereo, frames);
        snare_bottom = channel_peak(stereo, frames, 0u);

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        pops_mixer_set_balance(&ctx->mixer, POPS_Q16_ONE);
        mp7_voice_trigger(&ctx->voices[POPS_INSTR_SNARE_1]);
        pops_process(ctx, stereo, frames);
        snare_top = channel_peak(stereo, frames, 0u);

        if (snare_bottom != snare_top) {
            printf("FAIL: Balance moved Snare 1 (%d -> %d)\n",
                   snare_bottom, snare_top);
            failures++;
        } else {
            ok("Balance leaves Snare 1 untouched");
        }
    }

    /* A per-instrument gain multiplies its own voice only. */
    render_trigger(ctx, POPS_INSTR_TAMBOURINE, stereo, frames);
    {
        int tamb_default = channel_peak(stereo, frames, 0u);
        int tamb_full;

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        pops_mixer_set_instr_gain(&ctx->mixer, POPS_INSTR_TAMBOURINE,
                                   POPS_Q16_ONE);
        mp7_voice_trigger(&ctx->voices[POPS_INSTR_TAMBOURINE]);
        pops_process(ctx, stereo, frames);
        tamb_full = channel_peak(stereo, frames, 0u);

        if ((tamb_default == 0) || (tamb_full < tamb_default + tamb_default / 2)) {
            printf("FAIL: the Tambourine instrument gain does not scale its voice\n");
            failures++;
        } else {
            ok("the Tambourine instrument gain scales its voice");
        }

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        pops_mixer_set_instr_gain(&ctx->mixer, POPS_INSTR_TAMBOURINE, 0);
        mp7_voice_trigger(&ctx->voices[POPS_INSTR_TAMBOURINE]);
        pops_process(ctx, stereo, frames);
        if (!stereo_is_silent(stereo, frames)) {
            fail("a zero instrument gain does not silence its voice");
        } else {
            ok("a zero instrument gain silences its voice");
        }
    }

    /* The per-instrument gain runs from 0 to 2: 1.0 is the instrument's
     * reference level, 2.0 is twice it and 0.5 is half of it. */
    {
        int peak_unity;
        int peak_double;
        int peak_half;

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        pops_mixer_set_volume(&ctx->mixer, POPS_Q16_ONE / 4);
        mp7_voice_trigger(&ctx->voices[POPS_INSTR_CLAVES]);
        pops_process(ctx, stereo, frames);
        peak_unity = channel_peak(stereo, frames, 0u);

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        pops_mixer_set_volume(&ctx->mixer, POPS_Q16_ONE / 4);
        pops_mixer_set_instr_gain(&ctx->mixer, POPS_INSTR_CLAVES,
                                   2 * POPS_Q16_ONE);
        mp7_voice_trigger(&ctx->voices[POPS_INSTR_CLAVES]);
        pops_process(ctx, stereo, frames);
        peak_double = channel_peak(stereo, frames, 0u);

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        pops_mixer_set_volume(&ctx->mixer, POPS_Q16_ONE / 4);
        pops_mixer_set_instr_gain(&ctx->mixer, POPS_INSTR_CLAVES,
                                   POPS_Q16_ONE / 2);
        mp7_voice_trigger(&ctx->voices[POPS_INSTR_CLAVES]);
        pops_process(ctx, stereo, frames);
        peak_half = channel_peak(stereo, frames, 0u);

        if ((peak_unity == 0) ||
            ((2 * peak_double) < (3 * peak_unity)) ||
            ((2 * peak_double) > (5 * peak_unity))) {
            printf("FAIL: a gain of 2.0 does not double the voice\n");
            failures++;
        } else if ((2 * peak_half) < (3 * peak_unity / 4) ||
                   (2 * peak_half) > (5 * peak_unity / 4)) {
            printf("FAIL: a gain of 0.5 does not halve the voice\n");
            failures++;
        } else {
            ok("the instrument gain spans 0..2 around the reference level");
        }
    }

    /* Muting an active voice must not freeze it: unmuting mid-hit has to
     * sound the same as if the voice had never been muted. */
    {
        const size_t half = frames / 4u;
        struct mp7_context_s *ref = context_new();

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        pops_mixer_set_instr_gain(&ctx->mixer, POPS_INSTR_CYMBAL_1, 0);
        mp7_voice_trigger(&ctx->voices[POPS_INSTR_CYMBAL_1]);
        pops_process(ctx, stereo, half); /* the muted first half */

        mp7_init(ref, POPS_DEFAULT_SAMPLE_RATE);
        pops_mixer_set_instr_gain(&ref->mixer, POPS_INSTR_CYMBAL_1,
                                   POPS_Q16_ONE);
        mp7_voice_trigger(&ref->voices[POPS_INSTR_CYMBAL_1]);
        pops_process(ref, other, half); /* the reference first half, discarded */

        pops_mixer_set_instr_gain(&ctx->mixer, POPS_INSTR_CYMBAL_1,
                                   POPS_Q16_ONE);
        pops_process(ctx, stereo, half);
        pops_process(ref, other, half);

        if (memcmp(stereo, other, half * 2u * sizeof(int16_t)) != 0) {
            fail("muting an active voice changed its envelope or filter state");
        } else {
            ok("a muted active voice keeps advancing its state");
        }

        free(ref);
    }

    {
        unsigned int v;

        for (v = 0u; v < (unsigned int)POPS_INSTR_COUNT; v++) {
            mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
            pops_mixer_set_instr_gain(&ctx->mixer, v, 0);
            mp7_voice_trigger(&ctx->voices[v]);
            pops_process(ctx, stereo, frames);
            if (!stereo_is_silent(stereo, frames)) {
                printf("FAIL: instrument gain %u did not silence its voice\n", v);
                failures++;
            }
        }

        /* One instrument's gain must not change another voice. */
        render_trigger(ctx, POPS_INSTR_BASS, stereo, frames);
        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        pops_mixer_set_instr_gain(&ctx->mixer, POPS_INSTR_SNARE_1, 0);
        mp7_voice_trigger(&ctx->voices[POPS_INSTR_BASS]);
        pops_process(ctx, other, frames);
        if (memcmp(stereo, other, frames * 2u * sizeof(int16_t)) != 0) {
            fail("an instrument gain changed another voice");
        } else {
            ok("instrument gains are independent across all 15 instruments");
        }
    }

    /* The Cowbell and the two Bongos carry a fixed half Level. Their signal
     * is half as loud, and the per-instrument gain keeps its whole 0..2 range
     * on top of the halved signal: its top is twice the halved signal. */
    {
        static const pops_instr trimmed[3] = {
            POPS_INSTR_COWBELL, POPS_INSTR_SMALL_BONGO, POPS_INSTR_LARGE_BONGO
        };
        /* An instrument of the same engine that carries no trim. */
        static const pops_instr plain[3] = {
            POPS_INSTR_CONGA, POPS_INSTR_BASS, POPS_INSTR_CLAVES
        };
        unsigned int h;

        for (h = 0u; h < 3u; h++) {
            struct mp7_voice *voice = &ctx->voices[trimmed[h]];
            int64_t trimmed_out[32];
            int64_t plain_out[32];
            unsigned int s;
            int trimmed_ok = 1;

            /* The trim sits at the engine output, so the engine of the same
             * voice plays the untrimmed signal when the instrument is not
             * one of the trimmed ones. */
            mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
            mp7_voice_trigger(voice);
            for (s = 0u; s < 32u; s++) {
                trimmed_out[s] = mp7_engine_process(voice);
            }

            mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
            voice = &ctx->voices[trimmed[h]];
            voice->instr = plain[h];
            mp7_voice_trigger(voice);
            for (s = 0u; s < 32u; s++) {
                plain_out[s] = mp7_engine_process(voice);
            }

            for (s = 0u; s < 32u; s++) {
                if (trimmed_out[s] != (plain_out[s] >> 1)) {
                    trimmed_ok = 0;
                    break;
                }
            }

            if (!trimmed_ok) {
                printf("FAIL: instrument %u is not trimmed to half level\n",
                       (unsigned int)trimmed[h]);
                failures++;
                continue;
            }

            ok("a trimmed voice plays at half level");
        }
    }

    /* The trim is on the voice, not on the gain: the per-instrument gain
     * still scales the trimmed signal over its whole 0..2 range. */
    {
        static const pops_instr trimmed[3] = {
            POPS_INSTR_COWBELL, POPS_INSTR_SMALL_BONGO, POPS_INSTR_LARGE_BONGO
        };
        unsigned int h;

        for (h = 0u; h < 3u; h++) {
            int full_peak;
            int half_gain_peak;

            render_trigger(ctx, trimmed[h], stereo, frames);
            full_peak = channel_peak(stereo, frames, 0u);

            mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
            pops_mixer_set_instr_gain(&ctx->mixer, trimmed[h],
                                       POPS_Q16_ONE / 2);
            mp7_voice_trigger(&ctx->voices[trimmed[h]]);
            pops_process(ctx, other, frames);
            half_gain_peak = channel_peak(other, frames, 0u);

            /* The output is rounded to Q15, so the peaks can be a unit or
             * two off the exact ratio. */
            if ((full_peak == 0) ||
                (half_gain_peak < full_peak / 2 - 4) ||
                (half_gain_peak > full_peak / 2 + 4)) {
                printf("FAIL: the instrument gain does not scale the trimmed "
                       "voice %u (%d vs %d)\n",
                       (unsigned int)trimmed[h], half_gain_peak, full_peak);
                failures++;
                continue;
            }

            ok("a trimmed voice keeps the instrument gain range");
        }
    }

    free(stereo);
    free(other);
    free(ctx);
    return failures == 0 ? 0 : 1;
}

static int command_api(void)
{
    struct mp7_context_s *ctx = context_new();
    unsigned int i;

    /* The allocation helper initializes the context and can be released. */
    {
        struct mp7_context_s *created =
            (struct mp7_context_s *)pops_create_context(0);
        struct mp7_context_s *second =
            (struct mp7_context_s *)pops_create_context(22050);
        int16_t block[64 * 2];

        if ((created == 0) ||
            (created->sample_rate != POPS_DEFAULT_SAMPLE_RATE)) {
            fail("pops_create_context() did not initialize a default-rate context");
        } else {
            ok("pops_create_context() allocates and initializes a context");
        }

        if ((second == 0) || (second->sample_rate != 22050u)) {
            fail("pops_create_context() did not take the requested sample rate");
        } else {
            ok("pops_create_context() takes the requested sample rate");
        }

        /* Two contexts live at the same time and do not disturb each other. */
        if ((created != 0) && (second != 0) &&
            (created->sample_rate == POPS_DEFAULT_SAMPLE_RATE) &&
            (second->sample_rate == 22050u)) {
            ok("two contexts live at the same time with their own settings");
        } else {
            fail("two live contexts do not keep their own settings");
        }

        /* A created context renders audio like any other. */
        if (created != 0) {
            int non_zero = 0;

            memset(block, 0, sizeof(block));
            pops_trigger((pops_context)created, POPS_INSTR_BASS, 100);
            pops_process((pops_context)created, block, 64);
            for (i = 0u; i < 64u * 2u; i++) {
                if (block[i] != 0) {
                    non_zero = 1;
                    break;
                }
            }
            if (!non_zero) {
                fail("a created context renders no audio");
            } else {
                ok("a created context renders audio");
            }
        }

        pops_destroy_context((pops_context)second);
        pops_destroy_context((pops_context)created);

        /* A null context is ignored. */
        pops_destroy_context(0);
        ok("pops_destroy_context() ignores a null context");
    }

    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);

    /* Version reporting. */
    if (pops_version_number() != POPS_VERSION_NUMBER) {
        fail("pops_version_number() does not match POPS_VERSION_NUMBER");
    } else {
        ok("pops_version_number() returns the header version");
    }

    /* Factory-pattern access. The view always carries the whole 64-column
     * grid, with the repetitions of a shorter stored period applied and the
     * shared percussion layer merged into it, so the consumer never has to
     * know the stored period. */
    {
        struct pops_pattern_view view;
        int bad = 0;

        for (i = 0u; i < (unsigned int)POPS_RHYTHM_COUNT; i++) {
            unsigned int column;

            pops_get_selection_pattern(ctx, pops_rhythm_bank((pops_rhythm)i),
                                       pops_rhythm_button_bit((pops_rhythm)i),
                                       &view);
            if (view.enable != mp7_factory_rhythms[i].enable) {
                bad = 1;
                break;
            }
            if (view.compound !=
                mp7_mask_is_compound(mp7_factory_rhythms[i].enable)) {
                bad = 1;
                break;
            }
            if (view.timesig != mp7_factory_rhythms[i].timesig) {
                bad = 1;
                break;
            }
            for (column = 0u; column < POPS_PATTERN_COLUMNS; column++) {
                if (view.notes[column] !=
                    (pattern_note(&mp7_factory_rhythms[i], column) |
                     percussion_note(column))) {
                    bad = 1;
                    break;
                }
            }
            if (bad) {
                break;
            }
        }

        /* A selection of every rhythm of a bank is a selection like any
         * other: the notes of all of them, and the mask they have in
         * common. */
        {
            uint64_t expect_enable = ~(uint64_t)0;
            unsigned int rhythm;

            for (rhythm = 0u; rhythm < 10u; rhythm++) {
                expect_enable &= mp7_factory_rhythms[rhythm].enable;
            }
            pops_get_selection_pattern(ctx, POPS_BANK_A, 0x03FFu, &view);
            if (view.enable != expect_enable) {
                bad = 1;
            }
            if (view.timesig != POPS_TIMESIG_NONE) {
                bad = 1;
            }
        }

        if (bad) {
            fail("the view of a rhythm returned the wrong data");
        } else {
            ok("the view of a selection is the view of a rhythm");
        }
    }

    /* Every time signature has its own step-enable mask, and an unknown one
     * has none. */
    if ((pops_timesig_enable_mask(POPS_TIMESIG_4_4) != ~(uint64_t)0) ||
        (pops_timesig_enable_mask(POPS_TIMESIG_3_4) !=
         UINT64_C(0xFF0F0FFFFF0F0FFF)) ||
        (pops_timesig_enable_mask(POPS_TIMESIG_5_4T) !=
         UINT64_C(0x0F0F3F3F0F0F3F3F)) ||
        (pops_timesig_enable_mask(POPS_TIMESIG_7_4) !=
         UINT64_C(0x00FFFFFFFFFFFFFF)) ||
        (pops_timesig_enable_mask(POPS_TIMESIG_2_4) != MP7_PATTERN_COMPOUND_MASK) ||
        (pops_timesig_enable_mask(POPS_TIMESIG_6_8T) != MP7_PATTERN_COMPOUND_MASK) ||
        (pops_timesig_enable_mask((pops_timesig)POPS_TIMESIG_COUNT) != 0u)) {
        fail("the time signature masks are wrong");
    } else {
        ok("every time signature has its own enable mask");
    }

    /* The grid of a signature follows its mask: the 2/4, the 5/4 and the 6/8
     * play compound, the others play straight. */
    if ((mp7_mask_is_compound(pops_timesig_enable_mask(POPS_TIMESIG_2_4)) != 1u) ||
        (mp7_mask_is_compound(pops_timesig_enable_mask(POPS_TIMESIG_6_8T)) != 1u) ||
        (mp7_mask_is_compound(pops_timesig_enable_mask(POPS_TIMESIG_5_4T)) != 1u) ||
        (mp7_mask_is_compound(pops_timesig_enable_mask(POPS_TIMESIG_3_4)) != 0u) ||
        (mp7_mask_is_compound(pops_timesig_enable_mask(POPS_TIMESIG_4_4)) != 0u) ||
        (mp7_mask_is_compound(pops_timesig_enable_mask(POPS_TIMESIG_7_4)) != 0u)) {
        fail("the grid of a time signature is wrong");
    } else {
        ok("2/4, 5/4 and 6/8 play compound; 3/4, 4/4 and 7/4 play straight");
    }

    /* Velocity 0 is note-off. It releases the gated Guiro and leaves a
     * one-shot voice running. */
    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_trigger(ctx, POPS_INSTR_GUIRO, 100u);
    pops_trigger(ctx, POPS_INSTR_GUIRO, 0u);
    if (ctx->voices[POPS_INSTR_GUIRO].amp_env.stage != POPS_ENV_RELEASE) {
        fail("a zero-velocity Guiro trigger did not release the voice");
    } else {
        ok("a zero-velocity trigger releases the Guiro");
    }
    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_trigger(ctx, POPS_INSTR_BASS, 100u);
    pops_trigger(ctx, POPS_INSTR_BASS, 0u);
    if (ctx->voices[POPS_INSTR_BASS].active == 0u) {
        fail("note-off stopped a one-shot instrument");
    } else {
        ok("note-off leaves a one-shot instrument running");
    }

    /* The Guiro gate has two holders: the pattern's own gate and the note
     * the caller triggers. Neither one may cut the other off. */
    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1));
    pops_trigger(ctx, POPS_INSTR_GUIRO, 100u);
    {
        const size_t column_samples =
            (size_t)ctx->sequencer.beat_samples / 8u + 1u;
        size_t t;

        /* The shared Guiro row is XXXX...., so the pattern closes its own
         * gate on column 4. */
        for (t = 0u; t < 5u * column_samples; t++) {
            mp7_pattern_tick(ctx);
        }
        if (ctx->sequencer.guiro_gated != 0u) {
            fail("the pattern did not close its Guiro gate on a rest column");
        } else if (ctx->voices[POPS_INSTR_GUIRO].amp_env.stage == POPS_ENV_RELEASE) {
            fail("the pattern released a Guiro note the caller holds");
        } else {
            ok("the pattern does not cut off a Guiro note the caller holds");
        }
    }

    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1));
    mp7_pattern_tick(ctx); /* column 0 opens the pattern's gate */
    pops_trigger(ctx, POPS_INSTR_GUIRO, 0u);
    if (ctx->voices[POPS_INSTR_GUIRO].amp_env.stage == POPS_ENV_RELEASE) {
        fail("a caller's note-off cut off the pattern's Guiro gate");
    } else {
        ok("a caller's note-off does not cut off the pattern's Guiro gate");
    }

    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1));
    mp7_pattern_tick(ctx); /* column 0 opens the pattern's gate */
    {
        unsigned int sample;

        /* The Guiro attack is 1 ms, so the gate reaches its hold stage well
         * within this many samples. */
        for (sample = 0u; sample < 4410u; sample++) {
            (void)mp7_voice_process(&ctx->voices[POPS_INSTR_GUIRO]);
        }
    }
    if (ctx->voices[POPS_INSTR_GUIRO].amp_env.stage != POPS_ENV_HOLD) {
        fail("the pattern's Guiro gate did not reach its hold stage");
    } else {
        pops_trigger(ctx, POPS_INSTR_GUIRO, 100u);
        if (ctx->voices[POPS_INSTR_GUIRO].amp_env.stage != POPS_ENV_HOLD) {
            fail("a caller's Guiro note restarted the pattern's ratchet");
        } else {
            ok("a caller's Guiro note does not restart the pattern's ratchet");
        }
    }

    /* Rhythm selection mirrors the panel: one bank lever plus ten buttons. */
    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    if (pops_rhythm_running(ctx)) {
        fail("the sequencer is running before the first selection");
    }

    pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_WALTZ));
    if (!pops_rhythm_running(ctx) ||
        (ctx->sequencer.selected != pops_rhythm_button_bit(POPS_RHYTHM_WALTZ))) {
        fail("the Waltz button did not select Waltz in bank A");
    }

    pops_select_rhythm(ctx, POPS_BANK_A,
        pops_rhythm_button_bit(POPS_RHYTHM_WALTZ) | pops_rhythm_button_bit(POPS_RHYTHM_MARCH_2_4));
    if ((ctx->sequencer.selected !=
         (pops_rhythm_button_bit(POPS_RHYTHM_WALTZ) |
          pops_rhythm_button_bit(POPS_RHYTHM_MARCH_2_4))) ||
        (ctx->sequencer.subdivision != (uint8_t)MP7_SUBDIV_COMPOUND)) {
        fail("two buttons in one bank did not combine their rhythms");
    }

    /* Keys 10..14 do not exist: they are ignored. */
    pops_select_rhythm(ctx, POPS_BANK_A, 0x7C00u);
    if ((ctx->sequencer.selected != 0u) ||
        (ctx->sequencer.enable != ~(uint64_t)0)) {
        fail("button-mask bits above button 9 are not ignored");
    }

    pops_stop_rhythms(ctx);
    if (pops_rhythm_running(ctx) || (ctx->sequencer.selected != 0u) ||
        (ctx->sequencer.enable != 0u)) {
        fail("pops_stop_rhythms() did not stop the sequencer");
    }
    if (failures == 0) {
        ok("the bank/button selection combines buttons and Clear stops the sequencer");
    }

    /* Mapping a rhythm name to its panel bank and button bit. */
    if ((pops_rhythm_bank(POPS_RHYTHM_WALTZ) != POPS_BANK_A) ||
        (pops_rhythm_bank(POPS_RHYTHM_JAZZ_WALTZ) != POPS_BANK_B) ||
        (pops_rhythm_bank(POPS_RHYTHM_LATIN_SWING) != POPS_BANK_B) ||
        (pops_rhythm_button_bit(POPS_RHYTHM_WALTZ) != 1u) ||
        (pops_rhythm_button_bit(POPS_RHYTHM_SAMBA) != 2u) ||
        (pops_rhythm_button_bit(POPS_RHYTHM_JAZZ_WALTZ) != 1u) ||
        (pops_rhythm_button_bit(POPS_RHYTHM_LATIN_SWING) != 512u) ||
        (pops_rhythm_bank((pops_rhythm)POPS_RHYTHM_COUNT) != POPS_BANK_A) ||
        (pops_rhythm_button_bit((pops_rhythm)POPS_RHYTHM_COUNT) != 0u)) {
        fail("the rhythm-to-bank/button mapping is wrong");
    } else {
        ok("a rhythm maps to the correct bank and button bit");
    }

    /* The same button in bank B selects the corresponding upper-bank rhythm. */
    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_select_rhythm(ctx, POPS_BANK_B, 1u);
    if (ctx->sequencer.selected != ((uint32_t)1u << 10)) {
        fail("bank B button 0 did not select Jazz Waltz");
    }
    pops_select_rhythm(ctx, POPS_BANK_A, 1u);
    if (ctx->sequencer.selected != 1u) {
        fail("bank A button 0 did not select Waltz");
    } else {
        ok("the bank lever reinterprets the same button mask");
    }

    /* An immediate change keeps the playhead. Selecting no buttons keeps the
     * transport running and leaves only the Tambourine/Guiro/Quijada layer. */
    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_select_rhythm(ctx, POPS_BANK_A,
                      pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1));
    {
        uint8_t column;
        uint8_t column_fired;
        uint32_t position;
        unsigned int t;
        unsigned int column_index;
        int saw_default_hits = 0;

        for (t = 0u; t < 5u * ctx->sequencer.beat_samples / 8u + 123u; t++) {
            mp7_pattern_tick(ctx);
        }
        column = ctx->sequencer.column;
        column_fired = ctx->sequencer.column_fired;
        position = ctx->sequencer.column_pos;

        pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1) |
            pops_rhythm_button_bit(POPS_RHYTHM_WALTZ));

        if ((ctx->sequencer.column != column) ||
            (ctx->sequencer.column_fired != column_fired) ||
            (ctx->sequencer.column_pos != position)) {
            fail("an immediate rhythm change restarted the playhead");
        } else if (ctx->sequencer.selected !=
                   (pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1) |
                    pops_rhythm_button_bit(POPS_RHYTHM_WALTZ))) {
            fail("an immediate rhythm change did not combine the grids");
        } else {
            ok("an immediate rhythm change preserves the playhead");
        }

        pops_select_rhythm(ctx, POPS_BANK_A, 0u); /* no button pressed */

        if ((ctx->sequencer.column != column) ||
            (ctx->sequencer.column_fired != column_fired) ||
            (ctx->sequencer.column_pos != position) ||
            !pops_rhythm_running(ctx) ||
            (ctx->sequencer.selected != 0u)) {
            fail("selecting no rhythm buttons restarted or stopped the sequencer");
        }

        for (column_index = 0u; column_index < MP7_PATTERN_STEPS; column_index++) {
            uint16_t hits = ctx->sequencer.notes[column_index];

            if ((hits & ~MP7_PATTERN_PERCUSSION_MASK) != 0u) {
                fail("the no-button grid contains a main rhythm instrument");
                break;
            }
            if (hits != 0u) {
                saw_default_hits = 1;
            }
        }
        if (!saw_default_hits || (ctx->sequencer.enable != ~(uint64_t)0) ||
            (ctx->sequencer.subdivision != (uint8_t)MP7_SUBDIV_STRAIGHT)) {
            fail("the no-button grid is not the straight, full-length percussion layer");
        } else {
            ok("no rhythm buttons keeps the fixed percussion layer playing");
        }
    }

    /* The no-button layer triggers Tambourine and Guiro at column 0 and leaves
     * the main instruments silent. */
    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_select_rhythm(ctx, POPS_BANK_A, 0u);
    mp7_pattern_tick(ctx);
    if ((ctx->voices[POPS_INSTR_TAMBOURINE].active == 0u) ||
        (ctx->voices[POPS_INSTR_GUIRO].active == 0u) ||
        (ctx->voices[POPS_INSTR_BASS].active != 0u) ||
        (ctx->voices[POPS_INSTR_SNARE_1].active != 0u)) {
        fail("the no-button layer does not fire only the percussion voices");
    } else {
        ok("the no-button layer fires Tambourine and Guiro, not the main rhythm");
    }

    /* A queued change waits for the pattern cycle to wrap. */
    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1));
    {
        unsigned int t;

        for (t = 0u; t < 10u * 3u * 919u + 300u; t++) {
            mp7_pattern_tick(ctx);
        }

        pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1) |
            pops_rhythm_button_bit(POPS_RHYTHM_WALTZ) | POPS_APPLY_NEXT_CYCLE);
        if ((ctx->sequencer.selected != pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1)) ||
            (ctx->sequencer.pending_valid == 0u) ||
            (ctx->sequencer.pending_selected !=
             (pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1) |
              pops_rhythm_button_bit(POPS_RHYTHM_WALTZ)))) {
            fail("a queued rhythm change did not stay pending");
        } else {
            uint8_t previous = ctx->sequencer.column;
            int wrapped = 0;

            for (t = 0u; t < 128u * 3u * 919u; t++) {
                mp7_pattern_tick(ctx);
                if (ctx->sequencer.column < previous) {
                    wrapped = 1;
                    break;
                }
                previous = ctx->sequencer.column;
            }

            if (!wrapped) {
                fail("the pattern never wrapped while a change was queued");
            } else if ((ctx->sequencer.selected !=
                        (pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1) |
                         pops_rhythm_button_bit(POPS_RHYTHM_WALTZ))) ||
                       (ctx->sequencer.pending_valid != 0u) ||
                       (ctx->sequencer.column != 0u)) {
                fail("a queued rhythm change was not applied at the cycle wrap");
            } else {
                ok("a queued rhythm change starts at the next pattern cycle");
            }
        }
    }

    /* A queued selection replaces any earlier pending selection; an immediate
     * set cancels it. */
    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1));
    mp7_pattern_tick(ctx);
    pops_select_rhythm(ctx, POPS_BANK_A,
        pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1) |
        pops_rhythm_button_bit(POPS_RHYTHM_WALTZ) |
        pops_rhythm_button_bit(POPS_RHYTHM_SAMBA) |
        POPS_APPLY_NEXT_CYCLE);
    pops_select_rhythm(ctx, POPS_BANK_A,
        pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1) |
        pops_rhythm_button_bit(POPS_RHYTHM_SAMBA) |
        POPS_APPLY_NEXT_CYCLE);
    if ((ctx->sequencer.pending_selected !=
         (pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1) |
          pops_rhythm_button_bit(POPS_RHYTHM_SAMBA))) ||
        (ctx->sequencer.pending_valid == 0u)) {
        fail("a later queued selection did not replace the pending one");
    }
    pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1));
    if (ctx->sequencer.pending_valid != 0u) {
        fail("an immediate selection did not cancel the pending change");
    } else {
        ok("queued selections are replaced by later queues or immediate sets");
    }

    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1));
    mp7_pattern_tick(ctx);
    pops_select_rhythm(ctx, POPS_BANK_A, POPS_APPLY_NEXT_CYCLE); /* queue the no-button layer */
    if ((ctx->sequencer.selected != pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1)) ||
        (ctx->sequencer.pending_valid == 0u) ||
        (ctx->sequencer.pending_selected != 0u)) {
        fail("a queued no-button selection did not stay pending");
    } else {
        unsigned int t;

        for (t = 0u; t < 128u * 3u * 919u; t++) {
            uint8_t previous = ctx->sequencer.column;

            mp7_pattern_tick(ctx);
            if (ctx->sequencer.column < previous) {
                break;
            }
        }
        if ((ctx->sequencer.selected != 0u) ||
            (ctx->sequencer.pending_valid != 0u) ||
            !pops_rhythm_running(ctx) ||
            (ctx->sequencer.enable != ~(uint64_t)0) ||
            (ctx->sequencer.subdivision != (uint8_t)MP7_SUBDIV_STRAIGHT)) {
            fail("a queued no-button selection did not switch to the percussion layer");
        } else {
            ok("a queued no-button selection switches at the next cycle");
        }
    }

    /* An immediate stop cancels a pending change and resets the transport. */
    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1));
    mp7_pattern_tick(ctx);
    pops_select_rhythm(ctx, POPS_BANK_A,
        pops_rhythm_button_bit(POPS_RHYTHM_WALTZ) | POPS_APPLY_NEXT_CYCLE);
    pops_stop_rhythms(ctx);
    if (pops_rhythm_running(ctx) ||
        (ctx->sequencer.pending_valid != 0u) ||
        (ctx->sequencer.selected != 0u) ||
        (ctx->sequencer.enable != 0u)) {
        fail("an immediate stop did not cancel the pending change");
    } else {
        ok("an immediate stop cancels a pending rhythm change");
    }

    /* Queueing while stopped has no cycle to wait for, so it starts now. */
    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_select_rhythm(ctx, POPS_BANK_A,
        pops_rhythm_button_bit(POPS_RHYTHM_WALTZ) | POPS_APPLY_NEXT_CYCLE);
    if ((ctx->sequencer.selected != pops_rhythm_button_bit(POPS_RHYTHM_WALTZ)) ||
        (ctx->sequencer.pending_valid != 0u) ||
        !pops_rhythm_running(ctx)) {
        fail("queueing a rhythm while stopped did not start it");
    } else {
        ok("queueing a rhythm while stopped starts it immediately");
    }

    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_select_rhythm(ctx, POPS_BANK_A, POPS_APPLY_NEXT_CYCLE);
    if (!pops_rhythm_running(ctx) || (ctx->sequencer.selected != 0u) ||
        (ctx->sequencer.enable != ~(uint64_t)0)) {
        fail("queueing no buttons while stopped did not start the percussion layer");
    } else {
        ok("queueing no buttons while stopped starts the percussion layer");
    }

    /* Tempo clamping and the beat interval. */
    pops_set_tempo(ctx, 120u, 0u);
    if ((ctx->sequencer.tempo_q16 != 120u * 65536u) ||
        (ctx->sequencer.beat_samples != 22050u)) {
        fail("pops_set_tempo(120, 0) did not give a 22050-sample beat");
    }
    pops_set_tempo(ctx, 0u, 0u);
    if (ctx->sequencer.tempo_q16 != 65536u) {
        fail("pops_set_tempo(0, 0) is not clamped to 1 BPM");
    }
    pops_set_tempo(ctx, 1000u, 0u);
    if (ctx->sequencer.tempo_q16 != 240u * 65536u) {
        fail("pops_set_tempo(1000, 0) is not clamped to 240 BPM");
    }
    /* The fraction joins the clamp: 0.99998 BPM clamps up to 1.0 and
     * 240.000015 clamps down to 240.0. */
    pops_set_tempo(ctx, 0u, 65535u);
    if (ctx->sequencer.tempo_q16 != 65536u) {
        fail("pops_set_tempo(0, 65535) is not clamped to 1.0 BPM");
    }
    pops_set_tempo(ctx, 240u, 1u);
    if (ctx->sequencer.tempo_q16 != 240u * 65536u) {
        fail("pops_set_tempo(240, 1) is not clamped to 240.0 BPM");
    }
    /* A fractional tempo stores its own Q16 value. */
    pops_set_tempo(ctx, 120u, 32768u);
    if (ctx->sequencer.tempo_q16 != (120u * 65536u) + 32768u) {
        fail("pops_set_tempo(120, 32768) did not store 120.5 BPM");
    }
    /* The MIDI format. 500000 microseconds per quarter note is exactly
     * 120 BPM: the same beat the BPM setter gives, and no fraction to
     * accumulate. The clamps mirror 240 BPM and 1 BPM. */
    pops_set_tempo_us(ctx, 500000u);
    if ((ctx->sequencer.tempo_q16 != 120u * 65536u) ||
        (ctx->sequencer.beat_samples != 22050u)) {
        fail("pops_set_tempo_us(500000) is not exactly 120 BPM");
    }
    pops_set_tempo_us(ctx, 0u);
    if ((ctx->sequencer.tempo_q16 != 240u * 65536u) ||
        (ctx->sequencer.beat_samples != 11025u)) {
        fail("pops_set_tempo_us(0) is not clamped to 240 BPM");
    }
    pops_set_tempo_us(ctx, 249999u);
    if (ctx->sequencer.tempo_q16 != 240u * 65536u) {
        fail("pops_set_tempo_us(249999) is not clamped to 240 BPM");
    }
    pops_set_tempo_us(ctx, 60000001u);
    if ((ctx->sequencer.tempo_q16 != 65536u) ||
        (ctx->sequencer.beat_samples != 2646000u)) {
        fail("pops_set_tempo_us(60000001) is not clamped to 1 BPM");
    }
    if (failures == 0) {
        ok("pops_set_tempo() and pops_set_tempo_us() clamp and build the beat");
    }

    /* The exact beat period: a fractional tempo must not drift. At 120.5 BPM
     * a beat is not a whole number of samples at 44100 Hz, so a rounded beat
     * length would wander off; the accumulated fraction must keep the grid
     * on the ideal rational one. A boundary sits within half a sample of the
     * ideal and never further: the tempo_q16 field of the context is the
     * denominator of the ideal. */
    {
        const unsigned int beats = 1000u;
        const uint64_t ideal_num =
            (uint64_t)beats * 60u * POPS_DEFAULT_SAMPLE_RATE * 65536u;
        uint64_t den;
        uint64_t ticks;
        int drifted = 0;

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        pops_set_tempo(ctx, 120u, 32768u);            /* 120.5 BPM */
        den = ctx->sequencer.tempo_q16;
        pops_select_rhythm(ctx, POPS_BANK_A, POPS_APPLY_NEXT_CYCLE);
        ticks = run_beats(ctx, beats, 8u);

        if ((den * ticks) > ideal_num) {
            drifted = ((den * ticks) - ideal_num) > ((den + 1u) / 2u);
        } else {
            drifted = (ideal_num - (den * ticks)) > ((den + 1u) / 2u);
        }
        if (drifted) {
            fail("a fractional tempo drifts off the exact beat period");
        } else {
            ok("a fractional tempo holds the exact beat on the straight grid");
        }
    }

    /* The same on the compound grid, where a beat holds six visited columns
     * and the cycle only visits 48 of the 64 columns. */
    {
        const unsigned int beats = 1000u;
        const uint64_t ideal_num =
            (uint64_t)beats * 60u * POPS_DEFAULT_SAMPLE_RATE * 65536u;
        uint64_t den;
        uint64_t ticks;
        int drifted = 0;

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        pops_set_tempo(ctx, 120u, 32768u);            /* 120.5 BPM */
        den = ctx->sequencer.tempo_q16;
        pops_select_rhythm(ctx, POPS_BANK_A,
                           pops_rhythm_button_bit(POPS_RHYTHM_MARCH_6_8) |
                           POPS_APPLY_NEXT_CYCLE);
        ticks = run_beats(ctx, beats, 6u);

        if ((den * ticks) > ideal_num) {
            drifted = ((den * ticks) - ideal_num) > ((den + 1u) / 2u);
        } else {
            drifted = (ideal_num - (den * ticks)) > ((den + 1u) / 2u);
        }
        if (drifted) {
            fail("a fractional tempo drifts off the exact beat period");
        } else {
            ok("a fractional tempo holds the exact beat on the compound grid");
        }
    }

    /* The microseconds-per-quarter-note format converts exactly. 500000 us is
     * 120 BPM, whose beat is a whole number of samples here, so 1000 beats
     * must land on the sample, not near it. */
    {
        const unsigned int beats = 1000u;
        const uint64_t ideal_num =
            (uint64_t)beats * POPS_DEFAULT_SAMPLE_RATE * 500000u;
        uint64_t ticks;

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        pops_set_tempo_us(ctx, 500000u);
        pops_select_rhythm(ctx, POPS_BANK_A, POPS_APPLY_NEXT_CYCLE);
        ticks = run_beats(ctx, beats, 8u);

        if ((ticks * 1000000u) != ideal_num) {
            fail("the MIDI tempo format does not convert exactly");
        } else {
            ok("the MIDI tempo format converts exactly into whole samples");
        }
    }

    /* A microseconds value whose beat is not a whole number of samples
     * (44100 * 466999 / 1000000 = 20594.66) must not drift either. */
    {
        const unsigned int beats = 1000u;
        const uint64_t ideal_num =
            (uint64_t)beats * POPS_DEFAULT_SAMPLE_RATE * 466999u;
        uint64_t ticks;
        int drifted = 0;

        mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
        pops_set_tempo_us(ctx, 466999u);
        pops_select_rhythm(ctx, POPS_BANK_A, POPS_APPLY_NEXT_CYCLE);
        ticks = run_beats(ctx, beats, 8u);

        if (((uint64_t)1000000u * ticks) > ideal_num) {
            drifted = (((uint64_t)1000000u * ticks) - ideal_num) > 500000u;
        } else {
            drifted = (ideal_num - ((uint64_t)1000000u * ticks)) > 500000u;
        }
        if (drifted) {
            fail("a MIDI microseconds tempo drifts off the exact beat period");
        } else {
            ok("a fractional MIDI tempo holds the exact beat");
        }
    }

    /* Mix controls. */
    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_set_volume(ctx, 500000);
    if (ctx->mixer.panel_volume_q16 != 4 * POPS_Q16_ONE) {
        fail("pops_set_volume() does not clamp to 4.0");
    }
    pops_set_volume(ctx, -1);
    if (ctx->mixer.panel_volume_q16 != 0) {
        fail("pops_set_volume() does not clamp to 0");
    }
    pops_set_balance(ctx, 100000);
    if (ctx->mixer.balance_q16 != POPS_Q16_ONE) {
        fail("pops_set_balance() does not clamp to 1.0");
    }

    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_set_instr_gain(ctx, POPS_INSTR_QUIJADA, 100000);
    if (ctx->mixer.instr_gain_q16[POPS_INSTR_QUIJADA] != 100000) {
        fail("pops_set_instr_gain() does not keep a gain below 2.0");
    }
    pops_set_instr_gain(ctx, POPS_INSTR_QUIJADA, 3 * POPS_Q16_ONE);
    if (ctx->mixer.instr_gain_q16[POPS_INSTR_QUIJADA] != 2 * POPS_Q16_ONE) {
        fail("pops_set_instr_gain() does not clamp Quijada to 2.0");
    }
    pops_set_instr_gain(ctx, POPS_INSTR_GUIRO, -1);
    if (ctx->mixer.instr_gain_q16[POPS_INSTR_GUIRO] != 0) {
        fail("pops_set_instr_gain() does not clamp Guiro to 0");
    }
    for (i = 0u; i < (unsigned int)POPS_INSTR_COUNT; i++) {
        pops_set_instr_gain(ctx, (pops_instr)i, 0);
        if (ctx->mixer.instr_gain_q16[i] != 0) {
            fail("pops_set_instr_gain() did not map an instrument");
            break;
        }
    }
    if (failures == 0) {
        ok("pops_set_instr_gain() maps and clamps all instruments");
    }

    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_set_instr_pan(ctx, POPS_INSTR_QUIJADA, POPS_Q16_ONE + 1);
    if (ctx->mixer.instr_pan_q16[POPS_INSTR_QUIJADA] != POPS_Q16_ONE) {
        fail("pops_set_instr_pan() does not clamp Quijada to 1.0");
    }
    pops_set_instr_pan(ctx, POPS_INSTR_GUIRO, -1);
    if (ctx->mixer.instr_pan_q16[POPS_INSTR_GUIRO] != 0) {
        fail("pops_set_instr_pan() does not clamp Guiro to 0");
    }
    for (i = 0u; i < (unsigned int)POPS_INSTR_COUNT; i++) {
        pops_set_instr_pan(ctx, (pops_instr)i, 0);
        if ((ctx->mixer.instr_pan_q16[i] != 0) ||
            (ctx->voices[i].pan_q16 != 0)) {
            fail("pops_set_instr_pan() did not map an instrument");
            break;
        }
    }
    if (failures == 0) {
        ok("pops_set_instr_pan() maps and clamps all instruments");
    }

    /* A per-instrument pan survives a sample-rate change: the voices are
     * rebuilt there, and the pan lives in the mixer. */
    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_set_instr_pan(ctx, POPS_INSTR_CYMBAL_1, POPS_Q16_ONE);
    pops_set_sample_rate(ctx, 44100u);
    if ((ctx->mixer.instr_pan_q16[POPS_INSTR_CYMBAL_1] != POPS_Q16_ONE) ||
        (ctx->voices[POPS_INSTR_CYMBAL_1].pan_q16 != POPS_Q16_ONE)) {
        fail("a sample-rate change clears the per-instrument pan");
    } else {
        ok("a sample-rate change keeps the per-instrument pan");
    }

    /* Reset restores the panel defaults. */
    pops_reset(ctx);
    {
        int gain_bad = 0;

        for (i = 0u; i < (unsigned int)POPS_INSTR_COUNT; i++) {
            int32_t expected = POPS_Q16_ONE;

            if ((i == (unsigned int)POPS_INSTR_TAMBOURINE) ||
                (i == (unsigned int)POPS_INSTR_GUIRO) ||
                (i == (unsigned int)POPS_INSTR_QUIJADA)) {
                expected = POPS_Q16_ONE / 2;
            }
            if ((ctx->mixer.instr_gain_q16[i] != expected) ||
                (ctx->mixer.instr_pan_q16[i] != POPS_Q16_ONE / 2)) {
                gain_bad = 1;
                break;
            }
        }

        if (gain_bad ||
            (ctx->mixer.panel_volume_q16 != POPS_Q16_ONE) ||
            (ctx->mixer.balance_q16 != POPS_Q16_ONE / 2) ||
            (ctx->sequencer.tempo_q16 != 120u * 65536u) || pops_rhythm_running(ctx)) {
            fail("pops_reset() did not restore the panel defaults");
        } else {
            ok("pops_reset() restores the panel defaults");
        }
    }

    free(ctx);
    return failures == 0 ? 0 : 1;
}

static int command_panel(void)
{
    struct mp7_context_s *ctx = context_new();
    struct pops_panel_state state;
    unsigned int guard;

    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_get_panel_state(ctx, &state);
    if ((state.bank != POPS_BANK_A) || (state.buttons != 0u) || state.pending ||
        state.running || (state.column != 0u) || (state.flash_count != 0u)) {
        fail("the initial panel state is not the defaults");
    } else {
        ok("pops_get_panel_state() reports the initial defaults");
    }

    pops_get_panel_state(0, &state);
    if ((state.bank != POPS_BANK_A) || (state.pending_bank != POPS_BANK_A) ||
        state.pending || state.running || (state.buttons != 0u)) {
        fail("a null context does not yield a zeroed panel state");
    } else {
        ok("a null context yields a zeroed panel state");
    }

    /* The applied selection reports its bank and button mask. */
    pops_select_rhythm(ctx, POPS_BANK_B, 1u);
    pops_get_panel_state(ctx, &state);
    if ((state.bank != POPS_BANK_B) || (state.buttons != 1u) || state.pending ||
        !state.running || (ctx->sequencer.selected != ((uint32_t)1u << 10))) {
        fail("the panel state does not report the applied bank/buttons");
    } else {
        ok("the panel state reports the applied bank and buttons");
    }

    /* Queued selections are visible separately from the applied one. */
    pops_select_rhythm(ctx, POPS_BANK_A,
        pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1) | POPS_APPLY_NEXT_CYCLE);
    pops_get_panel_state(ctx, &state);
    if (!state.pending || (state.pending_bank != POPS_BANK_A) ||
        (state.pending_buttons != pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1)) ||
        (state.bank != POPS_BANK_B)) {
        fail("the panel state does not report a queued selection");
    } else {
        ok("the panel state reports the queued selection");
    }

    /* A queued change becomes the applied selection at the cycle wrap. */
    guard = 0u;
    while ((ctx->sequencer.pending_valid != 0u) && (guard++ < 300000u)) {
        mp7_pattern_tick(ctx);
    }
    pops_get_panel_state(ctx, &state);
    if (state.pending || (state.bank != POPS_BANK_A) ||
        (state.buttons != pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1))) {
        fail("the queued selection was not applied to the panel state");
    } else {
        ok("the queued selection becomes the applied panel state");
    }

    /* A stop keeps the lever/button indication but stops the transport and
     * cancels any further queued change. */
    pops_stop_rhythms(ctx);
    pops_get_panel_state(ctx, &state);
    if (state.running || state.pending || (state.bank != POPS_BANK_A) ||
        (state.buttons != pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1))) {
        fail("pops_stop_rhythms() did not leave the expected panel state");
    } else {
        ok("pops_stop_rhythms() keeps the bank/button indication");
    }

    /* Flash counter: one flash at column 31 and one at the cycle wrap. */
    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_ROCK_1));
    guard = 0u;
    while ((ctx->sequencer.column != 31u) && (guard++ < 300000u)) {
        mp7_pattern_tick(ctx);
    }
    pops_get_panel_state(ctx, &state);
    if ((ctx->sequencer.column != 31u) || (state.flash_count != 1u)) {
        fail("the lamp did not flash at the second half of the grid");
    } else {
        ok("the lamp flashes at the second half of the grid");
    }

    guard = 0u;
    while ((ctx->sequencer.column != 0u) && (guard++ < 300000u)) {
        mp7_pattern_tick(ctx);
    }
    pops_get_panel_state(ctx, &state);
    if ((ctx->sequencer.column != 0u) || (state.flash_count != 2u)) {
        fail("the lamp did not flash at the cycle wrap");
    } else {
        ok("the lamp flashes at the cycle wrap");
    }

    /* The counter survives a stop; it is only reset by init/reset. */
    pops_stop_rhythms(ctx);
    pops_get_panel_state(ctx, &state);
    if (state.running || (state.flash_count != 2u)) {
        fail("pops_stop_rhythms() reset the lamp flash counter");
    } else {
        ok("the lamp flash counter survives a stop");
    }

    pops_reset(ctx);
    pops_get_panel_state(ctx, &state);
    if (state.flash_count != 0u) {
        fail("pops_reset() did not reset the lamp flash counter");
    } else {
        ok("pops_reset() resets the lamp flash counter");
    }

    mp7_init(ctx, POPS_DEFAULT_SAMPLE_RATE);
    pops_get_panel_state(ctx, &state);
    if (state.flash_count != 0u) {
        fail("mp7_init() did not reset the lamp flash counter");
    } else {
        ok("mp7_init() resets the lamp flash counter");
    }

    /* A queued change that only moves the lever is applied too: the
     * no-button layer is the same on both banks, so the rhythm mask does
     * not change, but the bank indication still has to follow the lever. */
    pops_select_rhythm(ctx, POPS_BANK_A, 0u);
    pops_select_rhythm(ctx, POPS_BANK_B, POPS_APPLY_NEXT_CYCLE);
    pops_get_panel_state(ctx, &state);
    if (!state.pending || (state.pending_bank != POPS_BANK_B) ||
        (state.buttons != 0u) || (state.bank != POPS_BANK_A)) {
        fail("a queued lever change is not reported as pending");
    } else {
        ok("a queued lever change is reported as pending");
    }

    guard = 0u;
    while ((ctx->sequencer.pending_valid != 0u) && (guard++ < 300000u)) {
        mp7_pattern_tick(ctx);
    }
    pops_get_panel_state(ctx, &state);
    if (state.pending || (state.bank != POPS_BANK_B) || (state.buttons != 0u) ||
        !state.running) {
        fail("a queued lever change was not applied to the panel state");
    } else {
        ok("a queued lever change becomes the applied panel state");
    }

    free(ctx);
    return failures == 0 ? 0 : 1;
}

#if MP7_FX_REVERB
static int command_reverb(void)
{
    enum { FRAMES = 4096 };
    static int16_t dry[FRAMES * 2];
    static int16_t wet[FRAMES * 2];
    pops_context ctx = pops_create_context(POPS_DEFAULT_SAMPLE_RATE);
    unsigned int i;
    int differs = 0;

    if (ctx == 0) {
        fail("pops_create_context() failed for the reverb test");
        return failures == 0 ? 0 : 1;
    }

    if ((pops_get_instr_fx_send(ctx, POPS_INSTR_SNARE_1) != 0) ||
        (pops_get_instr_fx_send(ctx, POPS_INSTR_BASS) != 0)) {
        fail("the instrument sends do not default to zero");
    } else {
        ok("the instrument sends default to zero");
    }

    /* With no send the reverb is bypassed and the mix is the dry mix. */
    pops_trigger(ctx, POPS_INSTR_SNARE_1, 100);
    pops_process(ctx, dry, FRAMES);

    /* A full send adds a wet return, so the mix changes. */
    pops_reset(ctx);
    pops_set_instr_fx_send(ctx, POPS_INSTR_SNARE_1, POPS_Q16_ONE);
    pops_trigger(ctx, POPS_INSTR_SNARE_1, 100);
    pops_process(ctx, wet, FRAMES);

    for (i = 0u; i < (unsigned int)(FRAMES * 2); i++) {
        if (dry[i] != wet[i]) {
            differs++;
        }
    }
    if (differs == 0) {
        fail("a non-zero send did not change the mix");
    } else {
        ok("a non-zero send adds the reverb to the mix");
    }

    /* The send clamps, and a reset returns it to its default. */
    pops_set_instr_fx_send(ctx, POPS_INSTR_SNARE_1, -1);
    if (pops_get_instr_fx_send(ctx, POPS_INSTR_SNARE_1) != 0) {
        fail("the reverb send did not clamp to zero");
    } else {
        ok("the reverb send clamps to zero");
    }

    pops_reset(ctx);
    if (pops_get_instr_fx_send(ctx, POPS_INSTR_SNARE_1) != 0) {
        fail("pops_reset() did not restore the reverb defaults");
    } else {
        ok("pops_reset() restores the reverb defaults");
    }

    pops_destroy_context(ctx);
    return failures == 0 ? 0 : 1;
}
#endif /* MP7_FX_REVERB */

static void usage(const char *program)
{
    printf("usage: %s <command>\n\n", program);
    printf("  --init             report the context size, the default sample rate\n"
           "                     and the sine table footprint\n"
           "  --sine             verify the shared sine table, index by index\n"
           "  --gain             verify the Q15 gain multiply, including >unity\n"
           "  --silence [path]   render silence, check it and write a WAV file\n"
           "                     (default path: silence.wav)\n"
           "  --instruments [dir] render all 15 instruments to WAV files\n"
           "                     (default dir: build)\n"
           "  --noise            verify the 6 noise voices are uncorrelated\n"
           "  --guiro            verify Guiro gate hold and note-off release\n"
           "  --mixer            verify pan, Volume, Balance, instrument gains\n"
           "                     and instrument pans\n"
           "  --api              verify triggers, rhythm selection, tempo and the\n"
           "                     the panel mix controls\n"
           "  --panel            verify the panel state snapshot and lamp counter\n"
           "  --pattern          verify the factory patterns, combination and timing\n"
           "  --rhythms [dir]    render all 20 rhythms and two combinations to WAV\n"
           "                     files (default dir: build)\n"
           "  --user             verify the user pattern slots and the pattern\n"
           "                     source switch (an extended build only)\n"
#if MP7_FX_REVERB
           "  --reverb           verify the send reverb and its controls\n"
#endif
           "  --help             this text\n");
}

int main(int argc, char **argv)
{
    const char *command;
    const char *path = "silence.wav";

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    command = argv[1];

    if (strcmp(command, "--init") == 0) {
        return command_init();
    }
    if (strcmp(command, "--sine") == 0) {
        return command_sine();
    }
    if (strcmp(command, "--gain") == 0) {
        return command_gain();
    }
    if (strcmp(command, "--silence") == 0) {
        if (argc > 2) {
            path = argv[2];
        }
        return command_silence(path);
    }
    if (strcmp(command, "--instruments") == 0) {
        const char *dir = (argc > 2) ? argv[2] : "build";

        return command_instruments(dir);
    }
    if (strcmp(command, "--noise") == 0) {
        return command_noise();
    }
    if (strcmp(command, "--guiro") == 0) {
        return command_guiro();
    }
    if (strcmp(command, "--quijada") == 0) {
        return command_quijada();
    }
    if (strcmp(command, "--mixer") == 0) {
        return command_mixer();
    }
    if (strcmp(command, "--api") == 0) {
        return command_api();
    }
    if (strcmp(command, "--panel") == 0) {
        return command_panel();
    }
    if (strcmp(command, "--pattern") == 0) {
        return command_pattern();
    }
    if (strcmp(command, "--rhythms") == 0) {
        const char *dir = (argc > 2) ? argv[2] : "build";

        return command_rhythms(dir);
    }
    if (strcmp(command, "--user") == 0) {
        return command_user();
    }
#if MP7_FX_REVERB
    if (strcmp(command, "--reverb") == 0) {
        return command_reverb();
    }
#endif
    if (strcmp(command, "--help") == 0 || strcmp(command, "-h") == 0) {
        usage(argv[0]);
        return 0;
    }

    printf("unknown command: %s\n\n", command);
    usage(argv[0]);
    return 1;
}
