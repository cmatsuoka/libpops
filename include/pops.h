/*
 * pops.h - public API of the Mini Pops library.
 *
 * The original device is a 15-voice percussion unit. This library is a integer-only
 * (32-bit fixed point) reimplementation of its sound generation and its 20
 * factory rhythm patterns. It renders a single stereo mix into a caller-supplied
 * buffer. It has no I/O and no floating point. A context is created with
 * pops_create_context(), which also builds the shared full-period sine table on
 * first use.
 *
 * Building with -DLIBPOPS_EXTRA adds a second, host-writable set of 20
 * pattern slots and the switch that points the sequencer at them. Without
 * the macro the library is the preset-only build.
 */

#ifndef LIBPOPS_POPS_H
#define LIBPOPS_POPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Library version. major/minor/patch are packed as 0x00MMmmpp.
 * The plain integer constants keep POPS_VERSION_NUMBER usable in #if. */
#define POPS_VERSION_MAJOR 0
#define POPS_VERSION_MINOR 6
#define POPS_VERSION_PATCH 1
#define POPS_VERSION_NUMBER \
    ((POPS_VERSION_MAJOR << 16) | (POPS_VERSION_MINOR << 8) | POPS_VERSION_PATCH)

/* Sample rate used when the caller passes 0 to pops_create_context() or
 * pops_set_sample_rate(). The FM-1 runs at 44100. */
#define POPS_DEFAULT_SAMPLE_RATE 44100u

/* The 15 instruments, in voice-slot order. */
typedef enum {
    POPS_INSTR_BASS = 0,
    POPS_INSTR_SNARE_1,
    POPS_INSTR_SNARE_2,
    POPS_INSTR_SMALL_BONGO,
    POPS_INSTR_LARGE_BONGO,
    POPS_INSTR_CONGA,
    POPS_INSTR_COWBELL,
    POPS_INSTR_CLAVES,
    POPS_INSTR_HIHAT,
    POPS_INSTR_RIMSHOT,
    POPS_INSTR_CYMBAL_1,
    POPS_INSTR_CYMBAL_2,
    POPS_INSTR_TAMBOURINE,
    POPS_INSTR_GUIRO,
    POPS_INSTR_QUIJADA,
    POPS_INSTR_COUNT = 15
} pops_instr;

/* The 20 factory rhythms: 10 panel buttons x 2 banks. */
typedef enum {
    POPS_RHYTHM_WALTZ = 0,
    POPS_RHYTHM_SAMBA,
    POPS_RHYTHM_MAMBO,
    POPS_RHYTHM_RUMBA,
    POPS_RHYTHM_TANGO,
    POPS_RHYTHM_BOSSANOVA,
    POPS_RHYTHM_ROCK_1,
    POPS_RHYTHM_ROCK_3,
    POPS_RHYTHM_MARCH_2_4,
    POPS_RHYTHM_SWING,
    POPS_RHYTHM_JAZZ_WALTZ,
    POPS_RHYTHM_MERINGUE,
    POPS_RHYTHM_CHA_CHA,
    POPS_RHYTHM_BEGUINE,
    POPS_RHYTHM_HABANERA,
    POPS_RHYTHM_SLOW_ROCK,
    POPS_RHYTHM_ROCK_2,
    POPS_RHYTHM_ROCK_4,
    POPS_RHYTHM_MARCH_6_8,
    POPS_RHYTHM_LATIN_SWING,
    POPS_RHYTHM_COUNT = 20
} pops_rhythm;

/* The two positions of the Rhythm Section Control Lever. */
typedef enum {
    POPS_BANK_A = 0,
    POPS_BANK_B = 1
} pops_bank;

/* The time signature of a pattern.
 *
 * The signature is the metric the grid is built on. It names the hole
 * pattern of the grid - which columns are skipped - and with it the grid
 * the pattern plays on. A pattern never carries a step-enable mask of its
 * own: the library derives the mask from the signature, so a pattern cannot
 * ask for a hole pattern the hardware does not have.
 *
 * The grid skips the columns that are not part of the metric. The 2/4 and
 * the 6/8 share one hole pattern, the compound one, so their masks are the
 * same and the name only records the label the rhythm carries on the panel.
 *
 * The 5/4 and the 6/8 carry a T: they play on the compound grid, where a
 * beat holds six columns instead of eight, so their grouped columns keep
 * the triplet feel and their loop is slower than the same columns on the
 * straight grid. The T says so, and it leaves the plain names free for a
 * straight 5/4 or 6/8 if the library ever grows one.
 *
 * The zero value is 4/4, so a pattern that is only zeroed is a straight
 * 64-column loop.
 *
 * POPS_TIMESIG_NONE is not a signature but the absence of one: it is the
 * answer of pops_timesig_of_mask() for a mask that belongs to no signature,
 * and it is what a pattern view carries when the patterns that are combined
 * carry different signatures. It is not a valid input anywhere. */
typedef enum {
    POPS_TIMESIG_NONE = -1,
    POPS_TIMESIG_4_4 = 0,
    POPS_TIMESIG_2_4 = 1,
    POPS_TIMESIG_3_4 = 2,
    POPS_TIMESIG_5_4T = 3,
    POPS_TIMESIG_6_8T = 4,
    POPS_TIMESIG_7_4 = 5,
    POPS_TIMESIG_COUNT = 6
} pops_timesig;

/* The step-enable mask of a time signature: a 64-bit word, one bit per
 * column, and a disabled column takes no time.
 */
uint64_t pops_timesig_enable_mask(pops_timesig timesig);

/* The signature a mask belongs to, or POPS_TIMESIG_NONE when it belongs to
 * none of them. The masks of the signatures are the only masks the library
 * hands out, so a mask that is not one of them names no signature. It is
 * the inverse of pops_timesig_enable_mask(), except that the compound mask
 * is the mask of the 2/4 and of the 6/8 alike: it names the 2/4. A pattern
 * that carries the compound mask keeps the name of its own meter, so the
 * signature of a pattern comes from the pattern and not from its mask. */
pops_timesig pops_timesig_of_mask(uint64_t mask);

/* Schedule flags for pops_select_rhythm(), OR-ed into the button mask.
 * POPS_APPLY_NOW is zero, so the immediate case needs no flag. */
#define POPS_APPLY_NOW 0x0000u
#define POPS_APPLY_NEXT_CYCLE 0x8000u

/* The opaque context. */
typedef void *pops_context;

/* Return the version of the library the process is linked against, packed as
 * POPS_VERSION_NUMBER. This reports the compiled library, so a host can compare
 * it with the header's POPS_VERSION_NUMBER to detect a mismatched build. */
uint32_t pops_version_number(void);

/* The number of columns in a pattern grid. Every grid is this long; a
 * consumer always sees the whole cycle. */
#define POPS_PATTERN_COLUMNS 64u

/* A view of a pattern: the complete grid of the cycle, ready to print and
 * to play.
 *
 * The same struct carries the view of one rhythm and the view of a whole
 * selection of rhythms, so a host needs one kind of object for the pattern
 * card and one call to fill it.
 *
 * `notes` is the grid, 64 columns of 16-bit instrument masks: bit n is set
 * when instrument n (in pops_instr order) plays in that column, so bit 0 is
 * the Bass Drum. It holds every voice of the pattern, the rhythm's own
 * instruments and the shared Tambourine/Guiro/Quijada layer (bits 12..14)
 * already merged into it. Bit 15 is the accent flag of a user-pattern
 * column; it is clear for the factory rhythms.
 *
 * `enable` is a 64-bit mask, one bit per column; disabled columns take no
 * time. It is the mask of the pattern that is played, which for a selection
 * of rhythms is the AND of the masks of the rhythms in it.
 *
 * `compound` says the pattern plays on the compound grid, where a beat holds
 * six columns instead of eight. The grid follows the masks of the patterns
 * that are combined, not the combined mask: a selection of a 5/4 and a 4/4
 * pattern plays compound, although its combined mask is the one of the 5/4.
 *
 * `timesig` is the signature of the pattern, or POPS_TIMESIG_NONE when the
 * patterns that are combined carry different signatures. It is the meter of
 * the selection, and it is not the signature of the combined mask: a 4/4 and
 * a 3/4 pattern combined are a mixed selection, although their combined
 * mask is the one of the 3/4.
 */
struct pops_pattern_view {
    uint16_t notes[POPS_PATTERN_COLUMNS];
    uint64_t enable;
    uint8_t compound;
    pops_timesig timesig;
};

/* Allocate and initialize a context. A sample_rate of 0 selects
 * POPS_DEFAULT_SAMPLE_RATE. Returns NULL when the allocation fails.
 *
 * The caller owns the returned context and releases it with
 * pops_destroy_context(). */
pops_context pops_create_context(uint32_t sample_rate);

/* Release a context returned by pops_create_context(). A null context is
 * ignored. Every context must be released exactly once: the handle cannot be
 * validated, so releasing it twice, or releasing a pointer that
 * pops_create_context() did not return, is undefined behaviour. */
void pops_destroy_context(pops_context ctx);

/* Change the sample rate.
 *
 * The biquad coefficients depend on the rate, so every voice is re-initialized
 * and the envelopes and filter state of any sounding voice are lost. */
void pops_set_sample_rate(pops_context ctx, uint32_t sample_rate);

/* Reset the voices, the sequencer and the mix controls to their defaults,
 * keeping the sample rate.
 *
 * In an extended build (-DLIBPOPS_EXTRA) the user-defined pattern slots
 * are preserved, but the pattern source is reset to the factory rhythms. */
void pops_reset(pops_context ctx);

/* Render `frames` stereo interleaved frames into `stereo_out`.
 *
 * `stereo_out` must have room for `frames` stereo frames (2 * frames
 * int16_t values). The buffer is cleared in bounded chunks, so any
 * `frames` value is safe on a 32-bit target as long as the buffer is
 * really that large.
 *
 * The library is not thread-safe: pops_process() and the setters must never
 * run at the same time. A panel control that is changed from another thread
 * has to be applied while the audio thread is blocked, or from the audio
 * thread itself. pops_create_context() and pops_destroy_context() must not run
 * at the same time either. */
void pops_process(pops_context ctx, int16_t *stereo_out, size_t frames);

/* Trigger one instrument. `velocity` is 0..127; 0 means note-off.
 *
 * The velocity value is accepted but instruments don't respond to it.
 * Note-off is meaningful only for the gated Guiro; every other instrument
 * is a one-shot and ignores it.
 *
 * The Guiro gate has two holders: the sequencer's own pattern gate and the
 * note the caller triggers here. The voice sounds while either one holds it,
 * so a triggered note is not cut off by the pattern and the pattern is not
 * cut off by a note-off. A note that arrives while the gate is already open
 * does not restart the ratchet. */
void pops_trigger(pops_context ctx, pops_instr instr, uint8_t velocity);

/* Select the pressed rhythm buttons.
 *
 * `bank` is the Rhythm Section Control Lever position. `buttons` is a mask of
 * the ten physical rhythm buttons: bit k is button k (0..9), and bits 10..14
 * are ignored. Several buttons in the same bank are OR-ed together, so a
 * selection can never mix bank A and bank B.
 *
 * OR POPS_APPLY_NEXT_CYCLE into `buttons` to apply the selection at the next
 * pattern cycle instead of immediately. POPS_APPLY_NOW is zero, so the plain
 * button mask means "apply now":
 *
 * - With POPS_APPLY_NOW the selection takes effect at the current playhead
 *   position, without restarting the pattern, and cancels any pending change.
 * - With POPS_APPLY_NEXT_CYCLE the current cycle finishes with the old
 *   selection; the new selection is applied when the last enabled column wraps
 *   to the first, and starts from its first enabled column. A later
 *   POPS_APPLY_NEXT_CYCLE call replaces an earlier pending selection.
 *
 * buttons = 0 means no button is pressed. The sequencer keeps running in that
 * case: the main rhythm instruments fall silent and the fixed
 * Tambourine/Guiro/Quijada layer continues.
 *
 * With either flag, a call starts the sequencer if it was stopped, and a
 * selection made while it is stopped takes effect immediately.
 *
 * A deferred change that only moves the lever is applied like any other:
 * with no rhythm button pressed both banks play the same percussion layer,
 * so the rhythm does not change, but the bank indication still follows the
 * lever at the cycle wrap. */
void pops_select_rhythm(pops_context ctx, pops_bank bank, uint16_t buttons);

/* Stop the sequencer immediately (the panel's Stop switch): silence, reset the
 * playhead and cancel any pending change. The last bank/button selection is
 * retained for pops_get_panel_state(). This is distinct from selecting no
 * rhythm buttons, which keeps the percussion layer running. */
void pops_stop_rhythms(pops_context ctx);

/* Set the internal tempo in quarter notes per minute: `bpm` whole beats plus
 * `frac_q16` 1/65536ths of a beat. (120, 0) is 120 BPM, (120, 32768) is
 * 120.5 BPM. The combined value is clamped to 1.0..240.0; default 120.0.
 *
 * (0, 0) is the sentinel of the external step clock: the sequencer stops
 * advancing against the sample clock and pops_next_step() advances the
 * pattern one step per call, so a host that receives sync events can drive
 * the steps. Any other value (including (0, n) with n > 0) resumes the
 * internal clock; pops_set_tempo_us() has no sentinel and its 0 clamps to
 * the fastest tempo.
 *
 * The beat period is stored as an exact whole-sample count plus a fraction
 * that is accumulated across beats, so a fractional tempo does not drift
 * against the ideal grid. */
void pops_set_tempo(pops_context ctx, uint16_t bpm, uint16_t frac_q16);

/* Set the internal tempo in microseconds per quarter note, the tempo format
 * of MIDI files: 500000 is 120 BPM. Clamped to 250000 (240 BPM) and
 * 60000000 (1 BPM); 0 clamps to 250000.
 *
 * The conversion into a beat period is exact, so a host feeding MIDI-file or
 * MIDI-clock tempos does not accumulate drift. */
void pops_set_tempo_us(pops_context ctx, uint32_t us_per_quarter);

/* Advance the pattern by one step under the external step clock, and return
 * whether a step was played.
 *
 * The mode is entered with pops_set_tempo(ctx, 0, 0). In it the sample
 * clock does not move the playhead: pops_process() renders the sounding
 * voices but the pattern stands still until this call. Each call is one
 * step of the grid: the first one fires the column the playhead rests on
 * (the first step after a start), and every further call moves the playhead
 * to the next enabled column and fires it. A column the step-enable mask
 * disables is skipped, a queued rhythm selection is applied when a call
 * wraps the cycle, and the pilot/tempo lamp follows the playhead as usual.
 *
 * The host owns the step length entirely, so the caller has to pace the
 * calls from its own sync source (e.g. every third or fourth MIDI clock
 * tick, or one call per step event). The beat period of a previous tempo is
 * dormant while the mode lasts. A Guiro gate the pattern holds open stays
 * open while the calls pause.
 *
 * The call returns false, and does nothing, when the mode is not entered,
 * when the transport is stopped or when the pattern has nothing to play.
 * The library is not thread-safe: call it where the setters are called,
 * never while pops_process() is running on another thread. */
bool pops_next_step(pops_context ctx);

/* True while the sequencer transport is running, including the no-key case
 * where only the fixed percussion layer plays. False after pops_stop_rhythms()
 * or before the first pops_select_rhythm(). */
bool pops_rhythm_running(const void *ctx);

/* A snapshot of the panel state for a host UI.
 *
 * `bank` and `buttons` are the selection last applied by pops_select_rhythm()
 * or by a pending change at the cycle wrap. `buttons` is the ten-button mask
 * (bits 10..15 are always clear). The selection is retained across
 * pops_stop_rhythms(), which only stops the transport.
 *
 * `pending_*` describe a selection made with POPS_APPLY_NEXT_CYCLE and not yet
 * applied; they are meaningful only when `pending` is true.
 *
 * `column` is the current grid column, 0..63. `flash_count` is a monotonic
 * count of the lamp flashes produced by the sequencer. It advances on three
 * grid points: at the first beat after the transport starts, when the
 * playhead reaches the first enabled column at or after column 31 - the last
 * column of the first 32-column half - and when the playhead wraps back to
 * the first enabled column. It never decreases and is not reset by
 * pops_stop_rhythms(): a fresh context starts it at zero, and only
 * pops_reset() zeroes it again.
 *
 * The host can drive the two pilot/tempo lamps from this snapshot: light the
 * lamp for `bank` while stopped; while running, pulse that lamp whenever
 * `flash_count` changes. The host owns the visible pulse length. */
struct pops_panel_state {
    pops_bank bank;
    uint16_t buttons;
    pops_bank pending_bank;
    uint16_t pending_buttons;
    bool pending;
    bool running;
    uint8_t column;
    uint32_t flash_count;
};

/* Copy the current panel state into `out`. A null `out` is ignored and
 * nothing is written. A null or invalid context yields a zeroed snapshot
 * (bank A, not running). The library is not thread-safe: do not call this
 * while pops_process() is running on another thread. */
void pops_get_panel_state(const void *ctx, struct pops_panel_state *out);

/* Global mix controls, mapping the panel's remaining faders.
 *
 * Gains are Q16 units: 1.0 is exactly 65536. The values are linear
 * multipliers; the knob taper is a host-side concern.
 *
 * Volume is the panel value 0..4 (0..262144). It is not the master gain: the
 * mixer applies a fixed 0.5 factor, so panel 1.0 is a master gain of
 * 0.5 and panel 2.0 is unity.
 *
 * Balance is 0..1 (0..65536), default 0.5; it crossfades the Bass Drum
 * against Hihat/Cymbal 1/Cymbal 2 only.
 *
 * instr_gain is a per-instrument gain: 0..2 (0..131072). 0 is silence, 1.0 is
 * the level the original unit plays the instrument at and 2.0 is twice that
 * level. For the Tambourine, the Guiro and the Quijada, whose Additional
 * Volume faders top out at 1.0x, that 1.0 is the top of their fader; for
 * every other instrument it is the fixed level the panel gives it.
 *
 * Tambourine, Guiro and Quijada default to 0.5, the position their panel
 * Additional Volume faders sit at; every other instrument defaults to 1.0. A
 * host that never touches the gains gets the library's default mix unchanged.
 *
 * The gain is applied per block, so a change takes effect at the next block
 * boundary.
 *
 * instr_pan is a per-instrument pan position: 0..1 (0..65536), where 0 is
 * hard left, 65536 is hard right and 32768 - the default of every
 * instrument - is centre. The pan law is linear, so a centred voice is half
 * in each channel. The pan takes effect at once, also on a note that is
 * already sounding. */
void pops_set_volume(pops_context ctx, int32_t volume_q16);
void pops_set_balance(pops_context ctx, int32_t balance_q16);
void pops_set_instr_gain(pops_context ctx, pops_instr instr, int32_t gain_q16);
void pops_set_instr_pan(pops_context ctx, pops_instr instr, int32_t pan_q16);

/* Set the reverb send level of one instrument. `send_q16` is in Q16 units,
 * 0..65536: 0 sends nothing, 1.0 sends the instrument's post-gain, post-pan
 * signal to the bus at full level. The default is 0 for every instrument, so
 * enabling the reverb does not change the factory mix until a host raises a
 * send. Each instrument may use a different send level.
 *
 * The reverb's room size, wet level, damping and width are fixed at build
 * time (see LIBPOPS_FX_REVERB), so the send level is the only per-instrument
 * reverb control. The bus is bypassed completely when no instrument sends to
 * it; in a build without the reverb the sends are stored but the effect never
 * renders.
 *
 * A null context or an out-of-range instrument is ignored. */
void pops_set_instr_fx_send(pops_context ctx, pops_instr instr,
                           int32_t send_q16);

/* Return the reverb send level of one instrument in Q16 units. Returns 0
 * when `ctx` is null or `instr` is out of range. */
int32_t pops_get_instr_fx_send(const void *ctx, pops_instr instr);

/* Copy a view of a selection of patterns into `out`.
 *
 * `buttons` is the ten-button mask of `bank`, the same mask
 * pops_select_rhythm() takes; POPS_APPLY_NEXT_CYCLE in it is ignored, so the
 * running selection and the queued one can be asked for the same way. The
 * patterns that are combined are the factory rhythms, or the user slots
 * when the context points at the user source.
 *
 * The view is the composition the sequencer plays: the notes of the patterns
 * are OR-ed together, their masks are AND-ed, and the shared percussion
 * layer is merged into the factory rhythms. It does not touch the sequencer,
 * so it can be asked for a selection that is not playing.
 *
 * A selection of one pattern is the view of that pattern: a host that shows
 * a single rhythm asks for the one button of that rhythm, which
 * pops_rhythm_bank() and pops_rhythm_button_bit() hand out. With no button
 * pressed the view is the fixed percussion layer of the preset source, and
 * an empty grid in the user source.
 */
void pops_get_selection_pattern(pops_context ctx, pops_bank bank, uint16_t buttons,
                               struct pops_pattern_view *out);

/* Map a factory rhythm to the bank and button bit that select it on the
 * panel. These helpers let a name/list based interface build a bank/button
 * selection. An out-of-range rhythm returns POPS_BANK_A and a zero button
 * bit. */
pops_bank pops_rhythm_bank(pops_rhythm rhythm);
uint16_t pops_rhythm_button_bit(pops_rhythm rhythm);

#ifdef LIBPOPS_EXTRA
/* ---------------------------------------------------------------------------
 * Extended build: user-defined patterns.
 *
 * Define LIBPOPS_EXTRA when building the library to get a second set of 20
 * pattern slots that the host fills itself, and a switch that points the
 * sequencer at those slots instead of at the 20 factory rhythms. The factory
 * rhythms stay read-only in either build: the sequencer never writes them.
 *
 * Without the macro the library is the preset-only build it has always been
 * - the 20 factory rhythms, the shared percussion layer, and no extra RAM
 * and no extra entry points.
 * ------------------------------------------------------------------------ */

/* The number of user pattern slots. They are addressed the way the factory
 * rhythms are: slots 0..9 are the bank A buttons, slots 10..19 are the bank
 * B buttons. */
#define POPS_USER_PATTERN_COUNT 20u

/* The number of columns in a user pattern grid. */
#define POPS_USER_PATTERN_COLUMNS POPS_PATTERN_COLUMNS

/* Accent flag for a user-defined pattern column. When this bit is set in a
 * column, every instrument in that column (except the gated Guiro) is played
 * louder. It occupies the 16th bit because the 15 instruments fill bits 0..14. */
#define POPS_PATTERN_ACCENT ((uint16_t)(1u << POPS_INSTR_COUNT))

/* Which set of patterns the sequencer plays. */
typedef enum {
    POPS_PATTERN_SOURCE_PRESET = 0, /* the 20 factory rhythms */
    POPS_PATTERN_SOURCE_USER = 1    /* the 20 user-defined slots */
} pops_pattern_source;

/* A user-defined pattern.
 *
 * `notes` is POPS_USER_PATTERN_COLUMNS columns of 16-bit instrument masks:
 * bit n is set when instrument n (in pops_instr order) plays in that column,
 * and POPS_PATTERN_ACCENT (bit 15) marks an accented column. Unlike a factory
 * rhythm, a user pattern may use every instrument, the Tambourine, Guiro and
 * Quijada included: a user pattern is a complete pattern and not a rhythm
 * plus the shared percussion layer, so the shared layer is never added to it.
 *
 * `timesig` is the time signature of the pattern. It is the only way to
 * shape the grid: the library derives the step-enable mask from it, so the
 * pattern cannot carry a mask of its own and cannot ask for a hole pattern
 * the hardware does not have. A 2/4, 5/4 or 6/8 pattern plays on the compound
 * (shuffle) grid, every other signature plays straight. An unknown signature
 * is rejected.
 *
 * The grid is always the whole cycle: a pattern is 64 columns long, and
 * what is shorter than a bar is masked out by the mask of the signature,
 * which the library derives. There is no period to say how long the
 * repeating part of the grid is: a groove that repeats every sixteen
 * columns is written sixteen times.
 *
 * Every member of the struct is an input: the grid and the metric are what
 * the host writes, and the library derives the rest. */
struct pops_user_pattern {
    uint16_t notes[POPS_USER_PATTERN_COLUMNS];
    pops_timesig timesig;
};

/* Copy a pattern into one of the user slots.
 *
 * `index` is 0..POPS_USER_PATTERN_COUNT-1 and follows the panel: slots 0..9
 * are the bank A buttons, slots 10..19 the bank B buttons. The pattern is
 * copied, so the caller keeps ownership of `pattern` and may release it
 * right away.
 *
 * A null `pattern` clears the slot. An empty slot plays nothing and
 * disables no column, so it behaves like a silent pattern when it is
 * selected on its own.
 *
 * A pattern that is playing changes with the pattern: the grid is rebuilt
 * from the current playhead, like an immediate rhythm change.
 *
 * Returns false when `ctx` is null, `index` is out of range or the pattern
 * carries an unknown time signature; the slots are left alone in that case. */
bool pops_set_user_pattern(pops_context ctx,
                           unsigned int index,
                           const struct pops_user_pattern *pattern);

/* Copy the pattern in one of the user slots into `out`.
 *
 * Returns false when `ctx` is null, `index` is out of range, `out` is null
 * or the slot is empty; an empty or invalid slot yields a zeroed pattern.
 * The copy is the pattern as it was stored, so a set/get round trip returns
 * the same grid and time signature. */
bool pops_get_user_pattern(const void *ctx,
                           unsigned int index,
                           struct pops_user_pattern *out);

/* Point the sequencer at the factory rhythms or at the user slots.
 *
 * The switch is immediate, like POPS_APPLY_NOW: the current bank/button
 * selection keeps its meaning and is re-read against the other set of
 * patterns, the playhead keeps its position and a pending change is
 * cancelled. */
void pops_set_pattern_source(pops_context ctx, pops_pattern_source source);

/* The set of patterns the sequencer plays. */
pops_pattern_source pops_get_pattern_source(const void *ctx);

/* ---------------------------------------------------------------------------
 * Extended build: accent intensity and per-voice synthesis tweaks.
 *
 * These controls are not part of the original panel; they are provided for
 * hosts that want to shape the sound beyond the factory defaults. All of
 * them are reset to their defaults by pops_reset().
 * ------------------------------------------------------------------------ */

/* Set the accent gain multiplier for accented user-pattern columns.
 *
 * gain_q16 is in Q16 units (1.0 = 65536). The default is 1.2 (78643).
 * The control range is 0.5..2.0, so 1.0 means "no accent boost" and
 * values above it make accented columns louder. Clamped to 0.5..2.0.
 *
 * The value is read when a column fires, so a change takes effect on
 * the notes triggered after it, not on the ones already sounding. */
void pops_set_accent_gain(pops_context ctx, int32_t gain_q16);

/* Return the current accent gain multiplier in Q16 units.
 *
 * Returns 0 when `ctx` is null. */
int32_t pops_get_accent_gain(const void *ctx);

/* Set the decay time multiplier for an instrument.
 *
 * `instr` selects the instrument. Every instrument takes the decay
 * except the Guiro: its length is the gate the rhythm holds open, so it
 * has no decay of its own to scale. The call is ignored for instruments
 * without a decay so the API can be extended later without a signature
 * change.
 *
 * 1.0 is the factory decay, and it is the default for every
 * instrument. Values below 1.0 shorten the decay; values above 1.0
 * lengthen it. On the dual-tone instruments the multiplier scales both
 * tones' decays together, so the instrument keeps its shape. Clamped
 * to 0.2..5.0.
 *
 * A note that is already sounding is not cut off: it keeps playing and
 * decays at the new rate from the next sample. */
void pops_set_instr_decay(pops_context ctx, pops_instr instr, int32_t decay_q16);

/* Return the decay time multiplier of an instrument in Q16 units, where
 * 65536 is 1.0 (the factory decay).
 *
 * The value is the one the library holds, after the clamp of
 * pops_set_instr_decay(), so it is what the voice actually plays.
 *
 * Returns 0 when `ctx` is null, `instr` is out of range, or the
 * instrument has no decay multiplier. */
int32_t pops_get_instr_decay(const void *ctx, pops_instr instr);

/* Set the filter cut-off frequency for an instrument in Hz.
 *
 * `instr` selects the instrument. The supported instruments are
 * POPS_INSTR_SNARE_1, POPS_INSTR_SNARE_2 and POPS_INSTR_HIHAT; the call
 * is ignored for any other instrument so the API can be extended
 * later without a signature change.
 *
 * The cut-off is the center frequency of the instrument's band-pass:
 * the Snare 1 and the Snare 2 range over 1000..15000 Hz, and the
 * Hihat over 2000..15000 Hz. The factory defaults are 5500 Hz for the
 * two snares and 10000 Hz for the Hihat.
 *
 * The Hihat also has a high-pass, which the cut-off does not move: its
 * factory value is 1000 Hz, it removes the low end of the noise, and
 * moving it above the band-pass would only dull the sound instead of
 * opening it up.
 *
 * The filter is stable only below the sample rate's Nyquist
 * frequency, so the cut-off is also limited to two fifths of the
 * sample rate. At 44100 Hz that is above 15000 Hz, so the whole
 * documented range is available at the default rate. */
void pops_set_instr_cutoff(pops_context ctx, pops_instr instr, uint32_t freq_hz);

/* Return the cut-off frequency of an instrument in Hz.
 *
 * The value is the one the library holds, after the clamp of
 * pops_set_instr_cutoff(). The sample-rate limit of the filter is not
 * part of it: it is applied when the filter coefficients are computed,
 * together with the tuning multiplier of pops_set_instr_tune().
 *
 * Returns 0 when `ctx` is null, `instr` is out of range, or the
 * instrument has no settable cut-off. */
uint32_t pops_get_instr_cutoff(const void *ctx, pops_instr instr);

/* Set the noise high-pass frequency of an instrument in Hz.
 *
 * `instr` selects the instrument. The supported instruments are
 * POPS_INSTR_CYMBAL_1 and POPS_INSTR_CYMBAL_2; the call is ignored for
 * any other instrument so the API can be extended later without a
 * signature change.
 *
 * The high-pass is the low-end filter of a noise voice, ahead of its
 * band-pass: it removes the low end of the noise before the band-pass
 * sees it. The band-pass of the two Cymbals is fixed at its factory
 * center of 5600 Hz, so the high-pass is the tone control of their
 * voice: lowering it lets more of the low band through and fattens the
 * splash, and at the top of the range the voice is the factory tick.
 *
 * The range is 2000..5500 Hz. The ceiling is the factory value, which
 * sits a hundred Hz below the band-pass center, so the cymbal can never
 * be played thinner than the factory sound and the band-pass can never
 * end up under the high-pass; the floor keeps the metallic voices out
 * of the low band the way the Hihat's cut-off floor does.
 *
 * The value is the corner at the factory pitch: the tuning multiplier
 * of pops_set_instr_tune() composes with it the way it composes with
 * the cut-off, moving the high-pass and the band-pass together. The
 * factory defaults are 5500 Hz for both Cymbals.
 *
 * The filter is stable only below the sample rate's Nyquist frequency,
 * so the frequency is also limited to two fifths of the sample rate.
 * At 44100 Hz that is above 5500 Hz, so the whole documented range is
 * available at the default rate. */
void pops_set_instr_highpass(pops_context ctx, pops_instr instr, uint32_t freq_hz);

/* Return the noise high-pass frequency of an instrument in Hz.
 *
 * The value is the one the library holds, after the clamp of
 * pops_set_instr_highpass(). The sample-rate limit of the filter is not
 * part of it: it is applied when the filter coefficients are computed,
 * together with the tuning multiplier of pops_set_instr_tune().
 *
 * Returns 0 when `ctx` is null, `instr` is out of range, or the
 * instrument has no settable high-pass: the Tambourine, the Hihat and
 * the Snares have one, but it is fixed. */
uint32_t pops_get_instr_highpass(const void *ctx, pops_instr instr);

/* Set the tuning multiplier of an instrument.
 *
 * `instr` selects the instrument. Every instrument takes the tuning.
 *
 * The multiplier scales what is pitched in the voice: the body
 * oscillator of the sine, dual-tone, Guiro and Quijada engines, the
 * second tone of the dual-tone engine as well, and, in the noise
 * engines, the band-pass center composed with the cut-off of
 * pops_set_instr_cutoff() and the high-pass, scaled from its corner the
 * same way - the factory corner, or the one set by
 * pops_set_instr_highpass() for the two Cymbals - so the voice
 * transposes whole. The Guiro's ratchet rate and the Quijada's click
 * clock are timing, not pitch, and stay at the factory rate.
 *
 * Values are Q16 units: 65536 is the factory pitch and the default,
 * 32768 is an octave down and 131072 is an octave up. Clamped to
 * 0.5..2.0.
 *
 * A note that is already sounding follows the change: the oscillators
 * and the filter retune from the next sample, without cutting the note
 * off. */
void pops_set_instr_tune(pops_context ctx, pops_instr instr, int32_t tune_q16);

/* Return the tuning multiplier of an instrument in Q16 units, where
 * 65536 is 1.0 (the factory pitch).
 *
 * The value is the one the library holds, after the clamp of
 * pops_set_instr_tune(), so it is what the voice actually plays.
 *
 * Returns 0 when `ctx` is null or `instr` is out of range. */
int32_t pops_get_instr_tune(const void *ctx, pops_instr instr);
#endif /* LIBPOPS_EXTRA */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBPOPS_POPS_H */
