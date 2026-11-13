/*
 * mp7.c - context handling and entry points of the Mini Pops library.
 *
 * The context owns the 15 voice states, the rhythm sequencer and the global
 * mix controls. pops_process() advances the sequencer, renders every voice and
 * sums it into a stereo mix through the shared mixer core.
 */

#include <stdlib.h>
#include <string.h>

#include "mp7_internal.h"

/* The output buffer is cleared in bounded chunks so that the byte count
 * (frames * 2 channels * 2 bytes) cannot overflow size_t on a 32-bit
 * target. On pi32v2 a single memset of more than SIZE_MAX bytes would
 * wrap to a short length and leave stale samples in the buffer. */
#define MP7_CLEAR_CHUNK_FRAMES 1024u

static void clear_stereo(int16_t *stereo_out, size_t frames)
{
    while (frames > 0u) {
        size_t chunk = (frames > MP7_CLEAR_CHUNK_FRAMES) ? MP7_CLEAR_CHUNK_FRAMES : frames;

        memset(stereo_out, 0, chunk * 2u * sizeof(int16_t));
        stereo_out += chunk * 2u;
        frames -= chunk;
    }
}

pops_context pops_create_context(uint32_t sample_rate)
{
    struct mp7_context_s *c =
        (struct mp7_context_s *)malloc(sizeof(struct mp7_context_s));

    if (c == 0) {
        return 0;
    }

    mp7_init((pops_context)c, sample_rate);

#if MP7_FX_REVERB
    /* The reverb delay memory is a second allocation. Without it the
     * context still works, but the bus has nothing to render into, so
     * creation fails like any other allocation failure. */
    if (pops_reverb_init(&c->reverb) != 0) {
        free(c);
        return 0;
    }
    mp7_fx_apply(c);
#endif

    return (pops_context)c;
}

void pops_destroy_context(pops_context ctx)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;

    if (c == 0) {
        return;
    }

    mp7_fx_free(c);
    free(c);
}

/* Prepare a context. Internal: pops_create_context() and pops_reset() drive
 * it, and the public API has no other entry point.
 *
 * This zeroes the whole context, so it also clears the user-defined pattern
 * slots in an extended build. */
void mp7_init(pops_context ctx, uint32_t sample_rate)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    if (c == 0) {
        return;
    }

    pops_sine_init();

    memset(c, 0, sizeof(*c));

    if (sample_rate == 0u) {
        sample_rate = POPS_DEFAULT_SAMPLE_RATE;
    }
    c->sample_rate = sample_rate;

    /* The reverb controls are reset here, but the delay memory is not
     * allocated: mp7_init() can be called on a caller-owned context, so it
     * only prepares the state. pops_create_context() attaches the bus and
     * pops_reset() preserves it. */
    mp7_fx_init(c);

#ifdef LIBPOPS_EXTRA
    /* Extended-build synthesis defaults. These must match the factory voice
     * configuration in src/mp7_voice.c. Set them before mp7_voice_init_all()
     * so the voice setup can apply them. */
    c->accent_gain_q16 = MP7_PATTERN_ACCENT_GAIN_Q16;
    {
        unsigned int i;

        /* Every instrument takes the tuning; 1.0 is the factory pitch. */
        for (i = 0u; i < (unsigned int)POPS_INSTR_COUNT; i++) {
            c->instr_tune_q16[i] = POPS_Q16_ONE;
        }

        for (i = 0u; i < (unsigned int)POPS_INSTR_COUNT; i++) {
            const struct mp7_instr_param *param = &mp7_instr_params[i];

            if (param->slot == MP7_PARAM_SLOT_NONE) {
                continue;
            }
            /* The decay multiplier is 1.0, the factory decay. */
            c->instr_decay_q16[param->slot] = POPS_Q16_ONE;
            if (param->has_cutoff) {
                c->instr_cutoff_hz[param->slot] = param->cutoff_default_hz;
            }
            if (param->has_highpass) {
                c->instr_highpass_hz[param->slot] = param->highpass_default_hz;
            }
        }
    }
#endif

    /* The mixer is initialized first: the voices take their pan from the
     * per-instrument pan positions. */
    pops_mixer_init(&c->mixer);

    mp7_voice_init_all(c);
    mp7_pattern_init(c);

    /* The panel's three Additional Volume faders start at 0.5; every other
     * instrument starts at unity. */
    pops_mixer_set_instr_gain(&c->mixer, POPS_INSTR_TAMBOURINE, POPS_Q16_ONE / 2);
    pops_mixer_set_instr_gain(&c->mixer, POPS_INSTR_GUIRO, POPS_Q16_ONE / 2);
    pops_mixer_set_instr_gain(&c->mixer, POPS_INSTR_QUIJADA, POPS_Q16_ONE / 2);
}

void pops_set_sample_rate(pops_context ctx, uint32_t sample_rate)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    if (c == 0) {
        return;
    }

    if (sample_rate == 0u) {
        sample_rate = POPS_DEFAULT_SAMPLE_RATE;
    }
    c->sample_rate = sample_rate;
    mp7_voice_init_all(c);

    /* The voices were re-initialized, so no gate is open any more. */
    c->guiro_manual_gated = 0u;
    c->guiro_open = 0u;

    /* Keep the selected rhythms and tempo, but rebuild the timing and start
     * the current pattern from its first enabled column at the new rate. */
    mp7_pattern_set_tempo(c, c->sequencer.tempo);
    mp7_pattern_reset(c);

#if MP7_FX_REVERB
    /* The reverb's active delay lengths follow the sample rate. The delay
     * memory itself is already sized for the maximum rate, so this only
     * retunes and clears it. */
    pops_reverb_clear(&c->reverb);
    mp7_fx_apply(c);
#endif
}

/* Reset the context, keeping the sample rate and - in an extended build -
 * the user-defined pattern slots. The reverb delay memory is not touched:
 * mp7_init() zeroes the context, so the caller saves and restores it. */
static void mp7_reset_state(pops_context ctx, uint32_t sample_rate)
{
#ifdef LIBPOPS_EXTRA
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    struct mp7_user_slot saved_patterns[POPS_USER_PATTERN_COUNT];

    memcpy(saved_patterns, c->user_patterns, sizeof(saved_patterns));
    mp7_init(ctx, sample_rate);
    memcpy(c->user_patterns, saved_patterns, sizeof(saved_patterns));
#else
    mp7_init(ctx, sample_rate);
#endif
}

void pops_reset(pops_context ctx)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    uint32_t sample_rate;

    if (c == 0) {
        return;
    }

    /* Keep the sample rate and the user-defined pattern slots, and drop
     * everything else back to the defaults. */
    sample_rate = c->sample_rate;
#if MP7_FX_REVERB
    {
        /* mp7_init() zeroes the whole context, so the delay memory is
         * saved across it instead of being leaked and reallocated. */
        struct pops_reverb saved_reverb = c->reverb;

        mp7_reset_state(ctx, sample_rate);

        c->reverb = saved_reverb;
        pops_reverb_clear(&c->reverb);
        mp7_fx_apply(c);
    }
#else
    mp7_reset_state(ctx, sample_rate);
#endif
}

/* Effective per-voice gain: its fixed Level, scaled by Balance and by the
 * per-instrument gain, and by the accent of the note that is sounding.
 *
 * The gain lives in the context rather than in a local, because it also
 * changes when a column fires, which can happen in the middle of a block.
 * It is refreshed there, and whenever a note is triggered, so the per-sample
 * path only has to apply it. */
void pops_process(pops_context ctx, int16_t *stereo_out, size_t frames)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    size_t frame;
    unsigned int instr;

    if (stereo_out == 0) {
        return;
    }

    if (c == 0) {
        clear_stereo(stereo_out, frames);
        return;
    }

    /* The effective per-voice gain only changes when a mix control changes,
     * and a control cannot change while a block is being rendered, so the
     * gain is computed once per block instead of once per sample. */
    for (instr = 0u; instr < (unsigned int)POPS_INSTR_COUNT; instr++) {
        mp7_voice_refresh_gain(c, (pops_instr)instr);
    }

    for (frame = 0u; frame < frames; frame++) {
        int64_t sum_left = 0;
        int64_t sum_right = 0;
#if MP7_FX_REVERB
        int64_t fx_left = 0;
        int64_t fx_right = 0;
        const int fx_on = (c->fx_active != 0);
#endif

        /* Triggers are evaluated before the sample is rendered, so hits are
         * sample-accurate. */
        mp7_pattern_tick(c);

        for (instr = 0u; instr < (unsigned int)POPS_INSTR_COUNT; instr++) {
            struct mp7_voice *voice = &c->voices[instr];
            int64_t sample;
            int64_t left;
            int64_t right;

            /* An idle voice has no envelope or filter state left to
             * advance, so skip the engine call entirely. */
            if (!voice->active) {
                continue;
            }

            sample = mp7_voice_process(voice);
            if (sample == 0) {
                continue;
            }

            sample = pops_mul_q31_gain(sample, c->voice_gain_q16[instr]);
            left = pops_mixer_pan_left(sample, voice->pan_q16);
            right = pops_mixer_pan_right(sample, voice->pan_q16);
            sum_left += left;
            sum_right += right;

#if MP7_FX_REVERB
            /* The send takes the instrument's post-gain, post-pan signal.
             * A zero send skips the multiply, and when the bus is bypassed
             * the whole block is skipped. */
            if (fx_on) {
                const int32_t send = c->instr_fx_send_q16[instr];

                if (send != 0) {
                    fx_left += pops_mul_q31_gain(left, send);
                    fx_right += pops_mul_q31_gain(right, send);
                }
            }
#endif
        }

#if MP7_FX_REVERB
        if (fx_on) {
            int32_t wet_l;
            int32_t wet_r;

            pops_reverb_process(&c->reverb, fx_left, fx_right,
                                 &wet_l, &wet_r);
            sum_left += wet_l;
            sum_right += wet_r;
        }
#endif

        stereo_out[2u * frame] = pops_mixer_to_q15(sum_left, c->mixer.panel_volume_q16);
        stereo_out[2u * frame + 1u] = pops_mixer_to_q15(sum_right,
                                                         c->mixer.panel_volume_q16);
    }
}

void pops_trigger(pops_context ctx, pops_instr instr, uint8_t velocity)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    if ((c == 0) || ((unsigned int)instr >= (unsigned int)POPS_INSTR_COUNT)) {
        return;
    }

    if (instr == POPS_INSTR_GUIRO) {
        /* The Guiro is the gated voice: a note the caller triggers holds the
         * same gate the sequencer uses, so the two never fight over it. */
        mp7_guiro_gate(c, c->sequencer.guiro_gated,
                       (uint8_t)(velocity != 0u));
        return;
    }

    if (velocity == 0u) {
        mp7_voice_release(&c->voices[instr]);
        return;
    }

#ifdef LIBPOPS_EXTRA
    c->voices[instr].accent_gain_q16 = POPS_Q16_ONE;
    mp7_voice_refresh_gain(c, instr);
#endif
    mp7_voice_trigger(&c->voices[instr]);
}

/* The panel has one bank lever and ten buttons. Derive the internal absolute
 * rhythm mask from them; bits 10..14 of the button mask do not exist, and bit
 * 15 is the apply flag rather than a button. */
static uint32_t rhythm_selection(pops_bank bank, uint16_t buttons)
{
    uint32_t button_mask = (uint32_t)buttons & 0x03FFu;

    return (bank == POPS_BANK_B) ? (button_mask << 10) : button_mask;
}

void pops_select_rhythm(pops_context ctx, pops_bank bank, uint16_t buttons)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    uint16_t button_mask = (uint16_t)(buttons & 0x03FFu);
    int apply_next = (buttons & POPS_APPLY_NEXT_CYCLE) != 0u;
    uint32_t selected;
    int was_running;

    if (c == 0) {
        return;
    }

    bank = (bank == POPS_BANK_B) ? POPS_BANK_B : POPS_BANK_A;
    selected = rhythm_selection(bank, button_mask);
    was_running = (c->sequencer.running != 0u);

    c->sequencer.running = 1u;

    /* There is no current cycle to finish when the sequencer is stopped, so a
     * deferred selection takes effect immediately and starts the transport. */
    if (apply_next && was_running) {
        c->sequencer.pending_selected = selected;
        c->sequencer.pending_bank = (uint8_t)bank;
        c->sequencer.pending_buttons = button_mask;

        /* A change is pending when the rhythm mask or the panel indication
         * changes. The two are not the same thing: the no-button layer is the
         * same on both banks, so a lever change alone would not show up in the
         * mask, yet the bank lamp still has to follow it. */
        c->sequencer.pending_valid =
            ((selected != c->sequencer.selected) ||
             ((uint8_t)bank != c->sequencer.bank) ||
             (button_mask != c->sequencer.buttons)) ? 1u : 0u;
        return;
    }

    c->sequencer.bank = (uint8_t)bank;
    c->sequencer.buttons = button_mask;
    c->sequencer.pending_bank = (uint8_t)bank;
    c->sequencer.pending_buttons = button_mask;
    c->sequencer.pending_selected = selected;
    c->sequencer.pending_valid = 0u;

    if ((selected != c->sequencer.selected) || !was_running) {
        mp7_pattern_combine(c, selected);
    }
}

void pops_stop_rhythms(pops_context ctx)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    if (c == 0) {
        return;
    }

    c->sequencer.running = 0u;
    c->sequencer.selected = 0u;
    c->sequencer.pending_selected = 0u;
    c->sequencer.pending_bank = c->sequencer.bank;
    c->sequencer.pending_buttons = c->sequencer.buttons;
    c->sequencer.pending_valid = 0u;

    mp7_pattern_combine(c, 0u);
    mp7_pattern_reset(c);
}

void pops_set_tempo(pops_context ctx, uint16_t bpm)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    if (c == 0) {
        return;
    }

    mp7_pattern_set_tempo(c, (uint32_t)bpm);
}

bool pops_rhythm_running(const void *ctx)
{
    const struct mp7_context_s *c = (const struct mp7_context_s *)ctx;
    return (c != 0) && (c->sequencer.running != 0u);
}

void pops_get_panel_state(const void *ctx,
                         struct pops_panel_state *out)
{
    const struct mp7_context_s *c = (const struct mp7_context_s *)ctx;
    if (out == 0) {
        return;
    }

    if (c == 0) {
        memset(out, 0, sizeof(*out));
        out->bank = POPS_BANK_A;
        out->pending_bank = POPS_BANK_A;
        return;
    }

    out->bank = (pops_bank)c->sequencer.bank;
    out->buttons = c->sequencer.buttons;
    out->pending_bank = (pops_bank)c->sequencer.pending_bank;
    out->pending_buttons = c->sequencer.pending_buttons;
    out->pending = (c->sequencer.pending_valid != 0u);
    out->running = (c->sequencer.running != 0u);
    out->column = c->sequencer.column;
    out->flash_count = c->sequencer.flash_count;
}

void pops_set_volume(pops_context ctx, int32_t volume_q16)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    if (c == 0) {
        return;
    }

    pops_mixer_set_volume(&c->mixer, volume_q16);
}

void pops_set_balance(pops_context ctx, int32_t balance_q16)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    if (c == 0) {
        return;
    }

    pops_mixer_set_balance(&c->mixer, balance_q16);
}

void pops_set_instr_gain(pops_context ctx, pops_instr instr, int32_t gain_q16)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;

    if (c == 0) {
        return;
    }

    if ((unsigned int)instr >= (unsigned int)POPS_INSTR_COUNT) {
        return;
    }

    pops_mixer_set_instr_gain(&c->mixer, (unsigned int)instr, gain_q16);
}

void pops_set_instr_pan(pops_context ctx, pops_instr instr, int32_t pan_q16)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;

    if (c == 0) {
        return;
    }

    if ((unsigned int)instr >= (unsigned int)POPS_INSTR_COUNT) {
        return;
    }

    pops_mixer_set_instr_pan(&c->mixer, (unsigned int)instr, pan_q16);

    /* The voice pans immediately, so a note that is already sounding moves
     * to the new position. */
    c->voices[instr].pan_q16 = c->mixer.instr_pan_q16[instr];
}

void pops_get_selection_pattern(pops_context ctx, pops_bank bank, uint16_t buttons,
                               struct pops_pattern_view *out)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;

    if (out == 0) {
        return;
    }

    if (c == 0) {
        memset(out, 0, sizeof(*out));
        return;
    }

    /* The queued selection is asked for the same way as the running one, so
     * the schedule flag is not part of the button mask here. */
    mp7_pattern_compose(c, rhythm_selection(bank, buttons), out);
}

uint64_t pops_timesig_enable_mask(pops_timesig timesig)
{
    switch (timesig) {
    case POPS_TIMESIG_2_4:
        return MP7_ENABLE_2_4;
    case POPS_TIMESIG_3_4:
        return MP7_ENABLE_3_4;
    case POPS_TIMESIG_5_4T:
        return MP7_ENABLE_5_4T;
    case POPS_TIMESIG_6_8T:
        return MP7_ENABLE_6_8T;
    case POPS_TIMESIG_7_4:
        return MP7_ENABLE_7_4;
    case POPS_TIMESIG_4_4:
        return MP7_ENABLE_4_4;
    default:
        return 0u;
    }
}

pops_timesig pops_timesig_of_mask(uint64_t mask)
{
    unsigned int timesig;

    for (timesig = 0u; timesig < (unsigned int)POPS_TIMESIG_COUNT; timesig++) {
        if (pops_timesig_enable_mask((pops_timesig)timesig) == mask) {
            return (pops_timesig)timesig;
        }
    }

    return POPS_TIMESIG_NONE;
}

pops_bank pops_rhythm_bank(pops_rhythm rhythm)
{
    if ((unsigned int)rhythm >= (unsigned int)POPS_RHYTHM_COUNT) {
        return POPS_BANK_A;
    }
    return ((unsigned int)rhythm < 10u) ? POPS_BANK_A : POPS_BANK_B;
}

uint16_t pops_rhythm_button_bit(pops_rhythm rhythm)
{
    if ((unsigned int)rhythm >= (unsigned int)POPS_RHYTHM_COUNT) {
        return 0u;
    }
    return (uint16_t)(1u << ((unsigned int)rhythm % 10u));
}

#ifdef LIBPOPS_EXTRA
bool pops_set_user_pattern(pops_context ctx,
                           unsigned int index,
                           const struct pops_user_pattern *pattern)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    struct mp7_user_slot *slot;
    unsigned int column;

    if (c == 0) {
        return false;
    }
    if (index >= (unsigned int)POPS_USER_PATTERN_COUNT) {
        return false;
    }

    slot = &c->user_patterns[index];

    if (pattern == 0) {
        /* An empty slot plays nothing and disables no column. */
        memset(slot, 0, sizeof(*slot));
        return true;
    }

    /* The time signature names the hole pattern of the grid, and the
     * library derives the step-enable mask from it. A pattern therefore
     * cannot carry a mask of its own, and an unknown signature is refused
     * instead of being turned into one. */
    if ((unsigned int)pattern->timesig >= (unsigned int)POPS_TIMESIG_COUNT) {
        return false;
    }

    /* The grid is the whole cycle: a pattern is 64 columns long, and what
     * is shorter than a bar is masked out by the mask of the signature. */
    memset(slot->notes, 0, sizeof(slot->notes));
    for (column = 0u; column < (unsigned int)MP7_PATTERN_STEPS; column++) {
        slot->notes[column] = pattern->notes[column];
    }

    slot->timesig = (uint8_t)pattern->timesig;
    slot->used = 1u;

    /* A pattern that is playing changes with the pattern: the grid is
     * rebuilt from the current playhead, like an immediate rhythm change.
     * The preset source does not read the slots, so it is left alone. */
    if ((c->sequencer.source == MP7_SOURCE_USER) &&
        (c->sequencer.running != 0u)) {
        mp7_pattern_combine(c, c->sequencer.selected);
    }

    return true;
}

bool pops_get_user_pattern(const void *ctx,
                           unsigned int index,
                           struct pops_user_pattern *out)
{
    const struct mp7_context_s *c = (const struct mp7_context_s *)ctx;
    const struct mp7_user_slot *slot;

    if (out == 0) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    if ((c == 0) || (index >= (unsigned int)POPS_USER_PATTERN_COUNT)) {
        return false;
    }

    slot = &c->user_patterns[index];
    if (slot->used == 0u) {
        return false;
    }

    memcpy(out->notes, slot->notes, sizeof(out->notes));
    out->timesig = (pops_timesig)slot->timesig;

    return true;
}

void pops_set_pattern_source(pops_context ctx, pops_pattern_source source)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;

    if (c == 0) {
        return;
    }

    if (source != POPS_PATTERN_SOURCE_USER) {
        source = POPS_PATTERN_SOURCE_PRESET;
    }

    c->sequencer.source = (uint8_t)((source == POPS_PATTERN_SOURCE_USER)
                                     ? MP7_SOURCE_USER
                                     : MP7_SOURCE_PRESET);

    /* The switch is immediate, like POPS_APPLY_NOW: the selection keeps its
     * meaning and is re-read against the other set of patterns, the playhead
     * keeps its position, and a pending change is cancelled. */
    c->sequencer.pending_valid = 0u;
    c->sequencer.pending_selected = c->sequencer.selected;
    c->sequencer.pending_bank = c->sequencer.bank;
    c->sequencer.pending_buttons = c->sequencer.buttons;

    mp7_pattern_combine(c, c->sequencer.selected);
}

pops_pattern_source pops_get_pattern_source(const void *ctx)
{
    const struct mp7_context_s *c = (const struct mp7_context_s *)ctx;

    if ((c == 0) || (c->sequencer.source != MP7_SOURCE_USER)) {
        return POPS_PATTERN_SOURCE_PRESET;
    }
    return POPS_PATTERN_SOURCE_USER;
}

void pops_set_accent_gain(pops_context ctx, int32_t gain_q16)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;

    if (c == 0) {
        return;
    }

    c->accent_gain_q16 = pops_clamp_i32(gain_q16, POPS_Q16_ONE / 2,
                                          2 * POPS_Q16_ONE);
}

int32_t pops_get_accent_gain(const void *ctx)
{
    const struct mp7_context_s *c = (const struct mp7_context_s *)ctx;

    if (c == 0) {
        return 0;
    }

    return c->accent_gain_q16;
}

void pops_set_instr_decay(pops_context ctx, pops_instr instr, int32_t decay_q16)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;

    if (c == 0) {
        return;
    }

    if ((unsigned int)instr >= (unsigned int)POPS_INSTR_COUNT) {
        return;
    }

    if (!mp7_instr_params[instr].has_decay) {
        return;
    }

    c->instr_decay_q16[mp7_instr_params[instr].slot] =
        pops_clamp_i32(decay_q16, MP7_DECAY_MIN_Q16, MP7_DECAY_MAX_Q16);
    mp7_voice_apply_decay(c, instr);
}

int32_t pops_get_instr_decay(const void *ctx, pops_instr instr)
{
    const struct mp7_context_s *c = (const struct mp7_context_s *)ctx;

    if (c == 0) {
        return 0;
    }

    if ((unsigned int)instr >= (unsigned int)POPS_INSTR_COUNT) {
        return 0;
    }

    if (!mp7_instr_params[instr].has_decay) {
        return 0;
    }

    return c->instr_decay_q16[mp7_instr_params[instr].slot];
}

void pops_set_instr_cutoff(pops_context ctx, pops_instr instr, uint32_t freq_hz)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    const struct mp7_instr_param *param;

    if (c == 0) {
        return;
    }

    if ((unsigned int)instr >= (unsigned int)POPS_INSTR_COUNT) {
        return;
    }

    param = &mp7_instr_params[instr];
    if (!param->has_cutoff) {
        return;
    }

    if (freq_hz < param->cutoff_min_hz) {
        freq_hz = param->cutoff_min_hz;
    }
    if (freq_hz > param->cutoff_max_hz) {
        freq_hz = param->cutoff_max_hz;
    }
    c->instr_cutoff_hz[param->slot] = freq_hz;
    mp7_voice_apply_cutoff(c, instr);
}

uint32_t pops_get_instr_cutoff(const void *ctx, pops_instr instr)
{
    const struct mp7_context_s *c = (const struct mp7_context_s *)ctx;

    if (c == 0) {
        return 0;
    }

    if ((unsigned int)instr >= (unsigned int)POPS_INSTR_COUNT) {
        return 0;
    }

    if (!mp7_instr_params[instr].has_cutoff) {
        return 0;
    }

    return c->instr_cutoff_hz[mp7_instr_params[instr].slot];
}

void pops_set_instr_highpass(pops_context ctx, pops_instr instr, uint32_t freq_hz)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    const struct mp7_instr_param *param;

    if (c == 0) {
        return;
    }

    if ((unsigned int)instr >= (unsigned int)POPS_INSTR_COUNT) {
        return;
    }

    param = &mp7_instr_params[instr];
    if (!param->has_highpass) {
        return;
    }

    if (freq_hz < param->highpass_min_hz) {
        freq_hz = param->highpass_min_hz;
    }
    if (freq_hz > param->highpass_max_hz) {
        freq_hz = param->highpass_max_hz;
    }
    c->instr_highpass_hz[param->slot] = freq_hz;
    mp7_voice_apply_highpass(c, instr);
}

uint32_t pops_get_instr_highpass(const void *ctx, pops_instr instr)
{
    const struct mp7_context_s *c = (const struct mp7_context_s *)ctx;

    if (c == 0) {
        return 0;
    }

    if ((unsigned int)instr >= (unsigned int)POPS_INSTR_COUNT) {
        return 0;
    }

    if (!mp7_instr_params[instr].has_highpass) {
        return 0;
    }

    return c->instr_highpass_hz[mp7_instr_params[instr].slot];
}

void pops_set_instr_tune(pops_context ctx, pops_instr instr, int32_t tune_q16)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;

    if (c == 0) {
        return;
    }

    if ((unsigned int)instr >= (unsigned int)POPS_INSTR_COUNT) {
        return;
    }

    c->instr_tune_q16[instr] =
        pops_clamp_i32(tune_q16, MP7_TUNE_MIN_Q16, MP7_TUNE_MAX_Q16);
    mp7_voice_apply_tune(c, instr);
}

int32_t pops_get_instr_tune(const void *ctx, pops_instr instr)
{
    const struct mp7_context_s *c = (const struct mp7_context_s *)ctx;

    if (c == 0) {
        return 0;
    }

    if ((unsigned int)instr >= (unsigned int)POPS_INSTR_COUNT) {
        return 0;
    }

    return c->instr_tune_q16[instr];
}
#endif /* LIBPOPS_EXTRA */

uint32_t pops_version_number(void)
{
    return POPS_VERSION_NUMBER;
}
