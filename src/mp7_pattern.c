/*
 * mp7_pattern.c - rhythm combination and the sample-clocked sequencer.
 *
 * A rhythm is a 64-column note grid plus a 64-column step-enable mask (see
 * the patterns specification). A column of the note grid is a 16-bit
 * instrument mask: bit n is set when instrument n plays in that column.
 * Selecting several rhythms combines them by OR-ing the note masks and
 * AND-ing the enable masks.
 *
 * The grid a pattern plays on follows its enable mask: the compound mask and
 * the 5/4 mask play compound, every other mask plays straight. The
 * subdivision of a selection is therefore the compound one as soon as one of
 * the selected patterns is on the compound grid.
 *
 * The Tambourine, Guiro and Quijada hits are the same in every rhythm and
 * live in their own definition, mp7_shared_percussion_notes. It is OR-ed into
 * the combined grid of every selection, and it is the whole grid when no
 * rhythm button is pressed.
 *
 * An extended build (-DLIBPOPS_EXTRA) has a second set of patterns, the
 * user slots, and the selection is read against them when the sequencer
 * points at that source. A user pattern is complete - it carries its own
 * percussion lines - so the shared layer is not added to it. Its mask comes
 * from its time signature, like every other pattern's.
 *
 * Disabled columns take no time: the sequencer only ever visits enabled
 * columns. A column that an immediate rhythm change disables is skipped, not
 * waited out. A column lasts a fraction of a beat: the tempo is in quarter
 * notes per minute, and a beat holds eight straight columns (a 32nd note
 * each) or six compound ones (an 8th-note triplet each). The beat period is
 * held as an exact whole-plus-fraction sample count and is handed out to the
 * columns through integer remainders, so the grid drifts neither within a
 * beat nor over time. Triggers are evaluated before the sample at a column
 * boundary is rendered.
 *
 * A tempo of zero is the external step clock: the sample clock stops
 * advancing the playhead, and every step is a call to
 * mp7_pattern_next_step(), which moves the playhead the way a column
 * boundary of the sample clock does. The pacing of the steps is the host's.
 */

#include <string.h>

#include "mp7_internal.h"

/* A beat is a quarter note, and it holds eight straight columns or six
 * compound ones. The tempo is in quarter notes per minute, so the beat is
 * the only unit the sequencer needs. The tempo itself is a Q16 BPM value,
 * and the beat period derived from it is exact: whole samples plus a
 * fraction, accumulated across beats. */
#define MP7_STEPS_PER_BEAT_STRAIGHT 8u
#define MP7_STEPS_PER_BEAT_COMPOUND 6u
#define MP7_DEFAULT_TEMPO (120u * 65536u)
#define MP7_MIN_TEMPO (1u * 65536u)
#define MP7_MAX_TEMPO (240u * 65536u)

/* The MIDI-style tempo format: microseconds per quarter note. The fastest
 * allowed tempo, 240 BPM, is 250000 of them; the slowest, 1 BPM, is
 * 60000000. */
#define MP7_MAX_TEMPO_US 250000u
#define MP7_MIN_TEMPO_US 60000000u

/* The pilot/tempo lamp flashes when the playhead crosses column 31 - the
 * last column of the first 32-column half - and when the playhead wraps.
 * These are grid positions, not a musical meter. */
#define MP7_LAMP_HALF_COLUMN 31u

/* Both the rhythm grids and the shared percussion layer may store only a
 * shorter period (16 or 32 columns) that repeats to fill the 64-column
 * cycle. The period is always a power of two, so masking wraps the column.
 * The helper is in mp7_internal.h, because the pattern views need it too. */

static uint32_t steps_per_beat(const struct mp7_sequencer *seq)
{
    return (seq->subdivision == (uint8_t)MP7_SUBDIV_COMPOUND)
               ? MP7_STEPS_PER_BEAT_COMPOUND
               : MP7_STEPS_PER_BEAT_STRAIGHT;
}

/* Decide the whole-sample length of the beat that is starting and advance
 * the fractional phase by one beat.
 *
 * The phase, beat_remainder, runs in units of 1/beat_den of a sample and
 * starts at beat_den/2, the phase that rounds to nearest: the beat that is
 * starting gets its whole part plus the sample the accumulated fractions
 * have just paid for. The boundary it opens therefore sits within half a
 * sample of the ideal grid, the beats average exactly and nothing drifts. */
static void latch_beat(struct mp7_sequencer *seq)
{
    seq->beat_remainder += seq->beat_fraction;
    if (seq->beat_remainder >= seq->beat_den) {
        seq->beat_remainder -= seq->beat_den;
        seq->beat_samples = seq->beat_whole + 1u;
    } else {
        seq->beat_samples = seq->beat_whole;
    }
}

/* The length of the next column, in samples.
 *
 * The exact beat period is beat_whole + beat_fraction/beat_den samples, and a
 * whole-sample length cannot carry that exactly, so latch_beat() hands the
 * fractions out one sample at a time: one beat in every beat_den/beat_fraction
 * runs one sample longer. Over time a beat lasts exactly
 * beat_whole + beat_fraction/beat_den samples and no boundary drifts.
 *
 * A beat then holds steps_per_beat() columns, and it does not divide evenly
 * into them either: the leftover samples are handed out one at a time the
 * same way, so every column fills its exact share of the beat. Both
 * accumulators run continuously, so the clock never loses phase. */
static uint32_t column_length(struct mp7_sequencer *seq)
{
    uint32_t steps = steps_per_beat(seq);
    uint32_t length;

    /* A beat's worth of columns has been visited, so the column being
     * started here begins a new beat: latch its whole-sample length first.
     * The beat in play keeps the length it was latched with. */
    if (seq->beat_progress >= steps) {
        seq->beat_progress = 0u;
        latch_beat(seq);
    }
    seq->beat_progress++;

    length = seq->beat_samples / steps;
    seq->step_remainder += seq->beat_samples % steps;
    if (seq->step_remainder >= steps) {
        seq->step_remainder -= steps;
        length++;
    }
    if (length == 0u) {
        length = 1u;
    }
    return length;
}

static uint8_t first_enabled_column(const struct mp7_sequencer *seq)
{
    uint8_t column;

    for (column = 0u; column < (uint8_t)MP7_PATTERN_STEPS; column++) {
        if ((seq->enable & ((uint64_t)1u << column)) != 0u) {
            return column;
        }
    }
    return 0u;
}

static uint8_t next_enabled_column(const struct mp7_sequencer *seq, uint8_t from)
{
    uint8_t offset;

    for (offset = 1u; offset < (uint8_t)MP7_PATTERN_STEPS; offset++) {
        uint8_t column = (uint8_t)((from + offset) & (MP7_PATTERN_STEPS - 1));

        if ((seq->enable & ((uint64_t)1u << column)) != 0u) {
            return column;
        }
    }
    return from;
}

void mp7_guiro_gate(struct mp7_context_s *ctx,
                    uint8_t sequencer_gate,
                    uint8_t manual_gate)
{
    int held;

    ctx->sequencer.guiro_gated = sequencer_gate;
    ctx->guiro_manual_gated = manual_gate;

    /* The voice sounds while either holder keeps the gate open. */
    held = (sequencer_gate != 0u) || (manual_gate != 0u);

    if (held != 0) {
        /* Re-entering an adjacent X keeps the gate open, so the ratchet runs
         * without a release gap and is not restarted. */
        if (ctx->guiro_open == 0u) {
            mp7_voice_trigger(&ctx->voices[POPS_INSTR_GUIRO]);
            ctx->guiro_open = 1u;
        }
    } else if (ctx->guiro_open != 0u) {
        mp7_voice_release(&ctx->voices[POPS_INSTR_GUIRO]);
        ctx->guiro_open = 0u;
    }
}

void mp7_pattern_reset(struct mp7_context_s *ctx)
{
    struct mp7_sequencer *seq = &ctx->sequencer;

    /* A reset or a rhythm change can drop a Guiro gate that is still open.
     * A note the caller triggered itself is not affected. */
    mp7_guiro_gate(ctx, 0u, ctx->guiro_manual_gated);

    seq->column_fired = 0u;
    seq->column_pos = 0u;
    seq->step_remainder = 0u;
    /* A reset drops any owed start flash with the rest of the playhead
     * state, whether the transport stopped or is restarted in place (as a
     * sample-rate change does). Only a start after this reset lights the
     * lamp on its first beat again. */
    seq->start_flash_pending = 0u;
    /* The playhead restarts, so the clock restarts with it: the phase goes
     * back to half a fraction and the first beat is latched from there, the
     * same way a freshly set tempo starts. */
    seq->beat_remainder = seq->beat_den / 2u;
    latch_beat(seq);
    seq->beat_progress = 0u;
    seq->column = (seq->enable != 0u) ? first_enabled_column(seq) : 0u;
}

/* Store a new exact beat period: a beat lasts num/den samples. Whole-sample
 * lengths cannot hold a fractional period, so the period is split into its
 * whole part and a fraction that column_length() accumulates across beats
 * and hands back one whole sample at a time. The whole part of the beat in
 * play is recomputed immediately, and a faster tempo shortens the column
 * that is running, so the countdown cannot outlast it.
 *
 * The accumulated fraction carries over the change, rescaled to the new
 * denominator: it is a sub-sample phase amount, so keeping it keeps the
 * clock phase-continuous across a tempo change. */
static void set_beat_period(struct mp7_sequencer *seq, uint64_t num, uint32_t den)
{
    uint32_t whole = (uint32_t)(num / den);
    uint32_t fraction = (uint32_t)(num % den);

    if (seq->beat_den != 0u) {
        /* The sub-sample phase carries over a tempo change, rescaled to the
         * new denominator, so the clock stays phase-continuous. */
        uint64_t scaled =
            (((uint64_t)seq->beat_remainder * den) + (seq->beat_den / 2u)) /
            seq->beat_den;

        seq->beat_remainder = (scaled < den) ? (uint32_t)scaled : 0u;
    } else {
        /* A fresh context starts at half a fraction, the phase that rounds
         * to nearest. */
        seq->beat_remainder = den / 2u;
    }

    /* The period cannot be a whole-sample count at any supported rate, and
     * a beat shorter than one sample cannot be counted down; the guard is
     * for absurd sample rates only. */
    if (whole == 0u) {
        whole = 1u;
        fraction = 0u;
    }

    seq->beat_den = den;
    seq->beat_whole = whole;
    seq->beat_fraction = fraction;

    /* The beat in play takes its length from the phase just set. */
    latch_beat(seq);

    if (seq->column_pos > seq->beat_samples / steps_per_beat(seq)) {
        seq->column_pos = seq->beat_samples / steps_per_beat(seq);
    }
}

/* tempo_q16 is the tempo in BPM scaled by 65536. The exact beat period is
 * 60 * sample_rate * 65536 / tempo_q16 samples.
 *
 * tempo_q16 == 0 is the external step clock: the beat period is dormant and
 * the steps arrive from mp7_pattern_next_step() instead. The phase is
 * marked fresh, so returning to an internal tempo later starts a clean
 * beat. */
void mp7_pattern_set_tempo(struct mp7_context_s *ctx, uint32_t bpm_q16)
{
    struct mp7_sequencer *seq = &ctx->sequencer;

    if (bpm_q16 == 0u) {
        seq->external_clock = 1u;
        seq->tempo_q16 = 0u;
        seq->beat_den = 0u;
        return;
    }

    seq->external_clock = 0u;

    if (bpm_q16 < MP7_MIN_TEMPO) {
        bpm_q16 = MP7_MIN_TEMPO;
    }
    if (bpm_q16 > MP7_MAX_TEMPO) {
        bpm_q16 = MP7_MAX_TEMPO;
    }
    seq->tempo_q16 = bpm_q16;

    set_beat_period(seq, ((uint64_t)60u * ctx->sample_rate) * 65536u, bpm_q16);
}

/* us_per_quarter is the MIDI tempo format. The exact beat period is
 * sample_rate * us_per_quarter / 1000000 samples, which this converts
 * losslessly into the whole-plus-fraction period. */
void mp7_pattern_set_tempo_us(struct mp7_context_s *ctx, uint32_t us_per_quarter)
{
    struct mp7_sequencer *seq = &ctx->sequencer;

    seq->external_clock = 0u;

    if (us_per_quarter < MP7_MAX_TEMPO_US) {
        us_per_quarter = MP7_MAX_TEMPO_US;
    }
    if (us_per_quarter > MP7_MIN_TEMPO_US) {
        us_per_quarter = MP7_MIN_TEMPO_US;
    }

    /* The nearest Q16 BPM value, informational only: the timing follows the
     * exact period below. */
    seq->tempo_q16 = (uint32_t)((((uint64_t)60000000u * 65536u) +
                                 (us_per_quarter / 2u)) / us_per_quarter);

    set_beat_period(seq, (uint64_t)ctx->sample_rate * us_per_quarter, 1000000u);
}

/* The composition of a selection: the grid the sequencer plays and the card
 * prints.
 *
 * It is a pure function of the selection and of the source, so a host can
 * ask for the selection that is queued as well. The rules are the ones of
 * the hardware: the notes of the patterns that are combined are OR-ed
 * together, their masks are AND-ed, and the shared Tambourine/Guiro/Quijada
 * layer plays with every factory rhythm.
 *
 * A user pattern is a complete pattern: it carries its own percussion lines,
 * so the shared layer is not added to it. An empty slot plays nothing and
 * disables no column, so it is skipped instead of being folded into the
 * composition.
 *
 * The grid a selection plays on follows the masks of the patterns in it, not
 * the combined mask: the combined mask of a compound and a straight pattern
 * is neither of the two.
 *
 * The signature of the selection is the signature its patterns carry, and
 * POPS_TIMESIG_NONE when they do not all carry the same one. It is not the
 * signature of the combined mask: a 4/4 and a 3/4 pattern combined are a
 * mixed selection, although their combined mask is the one of the 3/4. */
void mp7_pattern_compose(struct mp7_context_s *ctx, uint32_t selected,
                         struct pops_pattern_view *out)
{
    unsigned int column;
    unsigned int index;
    int have_signature = 0;
    int mixed = 0;
    pops_timesig signature = POPS_TIMESIG_NONE;

    memset(out, 0, sizeof(*out));
    out->enable = ~(uint64_t)0;

#ifndef LIBPOPS_EXTRA
    /* The preset-only build has no user slots, so the composition does not
     * need the context. */
    (void)ctx;
#endif

#ifdef LIBPOPS_EXTRA
    /* The user slots are a complete set of patterns of their own, so the
     * selection is read against them instead of the factory rhythms. */
    if (ctx->sequencer.source == MP7_SOURCE_USER) {
        for (index = 0u; index < (unsigned int)POPS_RHYTHM_COUNT; index++) {
            const struct mp7_user_slot *slot;
            uint64_t mask;

            if ((selected & ((uint32_t)1u << index)) == 0u) {
                continue;
            }

            slot = &ctx->user_patterns[index];
            if (slot->used == 0u) {
                continue;
            }

            for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
                out->notes[column] |= slot->notes[column];
            }

            /* The mask comes from the time signature of the slot, so it is
             * one of the six the library knows. */
            mask = pops_timesig_enable_mask((pops_timesig)slot->timesig);
            out->enable &= mask;
            out->compound |= mp7_mask_is_compound(mask);

            if (!have_signature) {
                signature = (pops_timesig)slot->timesig;
                have_signature = 1;
            } else if ((pops_timesig)slot->timesig != signature) {
                mixed = 1;
            }
        }

        /* With no button pressed the user source keeps the transport running
         * and stays silent: the shared percussion layer belongs to the
         * factory rhythms. */
        out->timesig = mixed ? POPS_TIMESIG_NONE
                              : (have_signature ? signature : POPS_TIMESIG_4_4);
        return;
    }
#endif /* LIBPOPS_EXTRA */

    for (index = 0u; index < (unsigned int)POPS_RHYTHM_COUNT; index++) {
        const struct mp7_factory_rhythm *pattern;
        pops_timesig pattern_signature;

        if ((selected & ((uint32_t)1u << index)) == 0u) {
            continue;
        }
        pattern = &mp7_factory_rhythms[index];

        for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
            out->notes[column] |= pattern->notes[
                wrap_column(column, pattern->notes_period)];
        }
        out->enable &= pattern->enable;
        out->compound |= mp7_mask_is_compound(pattern->enable);

        pattern_signature = pattern->timesig;
        if (!have_signature) {
            signature = pattern_signature;
            have_signature = 1;
        } else if (pattern_signature != signature) {
            mixed = 1;
        }
    }

    /* The shared Tambourine/Guiro/Quijada layer plays with every rhythm, and
     * it is what plays on its own when no rhythm button is pressed. */
    for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
        out->notes[column] |= mp7_shared_percussion_notes[
            wrap_column(column, MP7_PATTERN_HALF_STEPS)];
    }

    out->timesig = mixed ? POPS_TIMESIG_NONE
                          : (have_signature ? signature : POPS_TIMESIG_4_4);
}

void mp7_pattern_combine(struct mp7_context_s *ctx, uint32_t selected_mask)
{
    struct mp7_sequencer *seq = &ctx->sequencer;
    struct pops_pattern_view view;
    uint32_t selected = selected_mask & ((1u << POPS_RHYTHM_COUNT) - 1u);

    seq->selected = selected;

    /* The sequencer plays the composition, so the grid it runs on is the
     * view the host prints. */
    mp7_pattern_compose(ctx, selected, &view);

    memcpy(seq->notes, view.notes, sizeof(seq->notes));
    seq->enable = view.enable;
    seq->subdivision = view.compound ? (uint8_t)MP7_SUBDIV_COMPOUND
                                      : (uint8_t)MP7_SUBDIV_STRAIGHT;

    /* A stopped transport plays nothing, so the mask is empty. */
    if ((seq->running == 0u) && (selected == 0u)) {
        seq->enable = 0u;
        seq->subdivision = (uint8_t)MP7_SUBDIV_STRAIGHT;
    }
}

void mp7_pattern_init(struct mp7_context_s *ctx)
{
    memset(&ctx->sequencer, 0, sizeof(ctx->sequencer));
    ctx->sequencer.tempo_q16 = MP7_DEFAULT_TEMPO;
    mp7_pattern_set_tempo(ctx, MP7_DEFAULT_TEMPO);
    mp7_pattern_combine(ctx, 0u);
}

/* Apply a queued selection when the current cycle wraps. The new pattern
 * starts at its first enabled column, and the caller fires it, so the first
 * hit lands one column later, exactly like an ordinary column change. */
static void apply_pending(struct mp7_context_s *ctx)
{
    struct mp7_sequencer *seq = &ctx->sequencer;

    if (seq->pending_valid == 0u) {
        return;
    }

    seq->pending_valid = 0u;

    /* The bank and the button mask are the panel indication, and they are
     * applied even when the rhythm mask itself does not change: the
     * no-button layer is the same on both banks, so a lever change is
     * invisible in the rhythm but the bank lamp still has to follow it. */
    seq->bank = seq->pending_bank;
    seq->buttons = seq->pending_buttons;

    if (seq->pending_selected == seq->selected) {
        return;
    }

    mp7_pattern_combine(ctx, seq->pending_selected);

    if (seq->enable == 0u) {
        mp7_pattern_reset(ctx);
        return;
    }

    seq->column = first_enabled_column(seq);
}

/* Move the playhead to the next enabled column and apply a queued selection
 * when the move wraps the cycle. Returns 0 when the sequencer has nothing
 * left to play. */
static int advance_column(struct mp7_context_s *ctx)
{
    struct mp7_sequencer *seq = &ctx->sequencer;
    uint8_t previous = seq->column;
    uint8_t next = next_enabled_column(seq, previous);

    /* Flash the pilot/tempo lamp when the playhead crosses column 31 and
     * when it wraps to the start of the cycle. A single-column grid
     * (next == previous) does not flash every step. */
    if (((previous < MP7_LAMP_HALF_COLUMN) &&
         (next >= MP7_LAMP_HALF_COLUMN)) ||
        (next < previous)) {
        seq->flash_count++;
    }

    seq->column = next;

    /* The column numbers increase within a cycle and wrap to a lower number
     * (or to the same column when only one is enabled) at the cycle
     * boundary. */
    if ((next <= previous) && (seq->pending_valid != 0u)) {
        apply_pending(ctx);
        if (seq->enable == 0u) {
            return 0;
        }
    }

    return 1;
}

/* Emit the hits of the current column, and handle the Guiro gate.
 *
 * An immediate rhythm change can disable the column the playhead is on. Such
 * a column takes no time, so the sequencer skips it and fires the next
 * enabled column instead of idling for a whole column. */
static void fire_column(struct mp7_context_s *ctx)
{
    struct mp7_sequencer *seq = &ctx->sequencer;
    uint8_t column;
    uint16_t hits;
    unsigned int instr;

    if ((seq->enable & ((uint64_t)1u << seq->column)) == 0u) {
        if (advance_column(ctx) == 0) {
            return;
        }
        if ((seq->enable & ((uint64_t)1u << seq->column)) == 0u) {
            return; /* nothing is enabled, so there is nothing to fire */
        }
    }

    column = seq->column;
    hits = seq->notes[column];

#ifdef LIBPOPS_EXTRA
    {
        int accented = (int)((hits & MP7_PATTERN_ACCENT_BIT) != 0u);

        hits &= MP7_PATTERN_INSTRUMENT_MASK;

        for (instr = 0u; instr < (unsigned int)POPS_INSTR_COUNT; instr++) {
            int hit = (int)((hits >> instr) & 1u);

            if (instr == (unsigned int)POPS_INSTR_GUIRO) {
                /* The gate belongs to the sequencer, but a note the caller
                 * triggered itself keeps it open as well. */
                mp7_guiro_gate(ctx, (uint8_t)(hit != 0), ctx->guiro_manual_gated);
            } else if (hit != 0) {
                struct mp7_voice *voice = &ctx->voices[instr];

                voice->accent_gain_q16 = accented
                    ? ctx->accent_gain_q16
                    : POPS_Q16_ONE;
                /* The accent is part of the voice's gain, so the cached
                 * gain has to follow it. */
                mp7_voice_refresh_gain(ctx, (pops_instr)instr);
                mp7_voice_trigger(voice);
            }
        }
    }
#else
    for (instr = 0u; instr < (unsigned int)POPS_INSTR_COUNT; instr++) {
        int hit = (int)((hits >> instr) & 1u);

        if (instr == (unsigned int)POPS_INSTR_GUIRO) {
            /* The gate belongs to the sequencer, but a note the caller
             * triggered itself keeps it open as well. */
            mp7_guiro_gate(ctx, (uint8_t)(hit != 0), ctx->guiro_manual_gated);
        } else if (hit != 0) {
            mp7_voice_trigger(&ctx->voices[instr]);
        }
    }
#endif
}

/* One step boundary: the column that was running has used up its time. The
 * playhead moves to the next enabled column and fires right away: a column
 * that a rhythm change disabled takes no time. Returns 0 when the sequencer
 * has nothing left to play. */
static int step_boundary(struct mp7_context_s *ctx)
{
    struct mp7_sequencer *seq = &ctx->sequencer;

    if (seq->column_fired != 0u) {
        seq->column_fired = 0u;
        if (advance_column(ctx) == 0) {
            return 0;
        }
    }

    if (seq->start_flash_pending != 0u) {
        /* The first beat after a start lights the lamp, the way the cycle
         * wrap does. This covers the step clock as well: every step of it
         * is a call here. */
        seq->start_flash_pending = 0u;
        seq->flash_count++;
    }

    fire_column(ctx);
    seq->column_fired = 1u;
    return 1;
}

void mp7_pattern_tick(struct mp7_context_s *ctx)
{
    struct mp7_sequencer *seq;

    if (ctx == 0) {
        return;
    }
    seq = &ctx->sequencer;

    /* The external step clock: the sample clock does not move the playhead,
     * every step is a call to mp7_pattern_next_step(). The voices are still
     * rendered, and what is sounding decays on its own. */
    if (seq->external_clock != 0u) {
        return;
    }

    if (seq->enable == 0u) {
        return;
    }

    if (seq->column_pos != 0u) {
        seq->column_pos--;
        return;
    }

    (void)step_boundary(ctx);
    seq->column_pos = column_length(seq) - 1u;
}

/* One step under the external step clock. The playhead moves the way it does
 * at a step boundary of the sample clock; only the pacing is the host's. */
int mp7_pattern_next_step(struct mp7_context_s *ctx)
{
    struct mp7_sequencer *seq;

    if (ctx == 0) {
        return 0;
    }
    seq = &ctx->sequencer;

    if ((seq->external_clock == 0u) || (seq->running == 0u) ||
        (seq->enable == 0u)) {
        return 0;
    }

    return step_boundary(ctx);
}
