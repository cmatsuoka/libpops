# libpops - a vintage drum machine engine library

*This library is under development and the API may be slightly unstable.*

A small (just a few dozen kilobytes), lightweight, target-agnostic **C,
integer-only, 32-bit fixed-point** audio library that implements the 15 voices
of a vintage Japanese drum machine and its 20 preset rhythm patterns. Check
the [demo](https://cmatsuoka.github.io/poppy-web) to hear what it sounds
like.

The library renders a single stereo mix into a caller-supplied buffer. It does
no I/O and no floating point. Memory allocation happens only when a context is
created: the context itself, plus the shared sine table (built once per
process).

## Available rhythm patterns

There are two banks of preset rhythms. Bank A contains Waltz, Samba, Mambo, Rumba,
Bossa Nova, Rock 1, Rock 3, March 2/4, and Swing. Bank B has Jazz Waltz, Meringue,
Cha-Cha, Beguine, Habanera, Slow Rock, Rock 2, Rock 4, March 6/8 and Latin Swing.
Patterns in the same bank can be combined by selecting multiple rhythms at the same
time.

A host that wants patterns of its own has a second, host-writable set of 20
patterns in the library. See [User-defined patterns](#user-defined-patterns).

## Build

```sh
make            # builds libpops.a
make check      # builds and runs the test harness
make play       # builds the OpenAL rhythm player example
```

`make play` additionally needs OpenAL (`pkg-config openal`, or `-lopenal`).
The player is written to `build/examples/play_rhythm`:

```sh
./build/examples/play_rhythm --tempo 140 --bank B --buttons=1,3
```

The library is built with the `LIBPOPS_EXTRA` macro defined, so it carries
the user pattern slots. A minimal, preset-only library — the 20 factory
rhythms and nothing else.

Define `LIBPOPS_FX_REVERB` when building to add the optional send reverb.
Without it there is no delay memory and the render loop has no reverb code;
the reverb control API stays available and the effect is bypassed.

Requirements: a C99 compiler and `make`. The library is built with
`-std=c99 -Wall -Wextra -Werror`; the build also enables
`-Wfloat-conversion -Wdouble-promotion` when the compiler supports them, so a
floating-point operation in the library is a build error. Unwind tables are
disabled where the compiler supports the flags.

## Using it

```c
#include <pops.h>

pops_context ctx = pops_create_context(44100);
if (ctx == NULL) { /* out of memory */ }

int16_t block[128 * 2];
pops_process(ctx, block, 128);      /* from the audio ISR */

pops_destroy_context(ctx);
```

Rhythm selection mirrors the panel: a bank A/B switch and a 10-bit mask of
the pressed rhythm buttons. A selection applies immediately by default; OR
`POPS_APPLY_NEXT_CYCLE` into the mask to defer it to the next pattern cycle. A
button mask of 0 means no button is pressed. Use `pops_stop_rhythms()` to stop
the player.

```c
pops_set_volume(ctx, 65536);                 /* panel 1.0 -> 0.5x master gain */
pops_set_balance(ctx, 32768);                /* center */
pops_set_instr_gain(ctx, POPS_INSTR_QUIJADA, 65536);
pops_set_instr_pan(ctx, POPS_INSTR_QUIJADA, 32768);   /* center, 0 = left, 65536 = right */
pops_set_tempo(ctx, 120, 0);                 /* 120 BPM; frac_q16 = 1/65536 beats */
pops_set_tempo_us(ctx, 500000);              /* MIDI tempo format: 500000 us is 120 BPM */

/* Bank A, Waltz + Samba buttons latched, applied now. */
pops_select_rhythm(ctx, POPS_BANK_A,
    pops_rhythm_button_bit(POPS_RHYTHM_WALTZ) |
    pops_rhythm_button_bit(POPS_RHYTHM_SAMBA));

/* Switch to bank B button 0 (Jazz Waltz) at the next cycle. */
pops_select_rhythm(ctx, pops_rhythm_bank(POPS_RHYTHM_JAZZ_WALTZ),
    pops_rhythm_button_bit(POPS_RHYTHM_JAZZ_WALTZ) | POPS_APPLY_NEXT_CYCLE);

/* no buttons: percussion only, applied now */
pops_select_rhythm(ctx, POPS_BANK_A, 0);
```

The panel state is available for a UI or lamp driver without callbacks:

```c
struct pops_panel_state state;
pops_get_panel_state(ctx, &state);

/* stopped: light the lens for state.bank; running: pulse it when
 * state.flash_count advances. */
```

`struct pops_panel_state` carries the applied bank and button mask, any pending
selection, the transport state, the current grid column and a monotonic lamp
flash counter. The flash counter advances on two grid points: the first
enabled column at or after column 31, and the cycle wrap. The host owns the
visible pulse length and the library supplies the events.

The Guiro is the only gated voice. Its gate is shared between the pattern
player and the note you trigger yourself: the voice sounds while either one
holds it, so a note you hold is not cut off by the pattern, the pattern is not
cut off by your note-off, and a note that arrives while the gate is already
open does not restart the ratchet.

The library is not thread-safe: `pops_process()` and the setters must not run at
the same time, and `pops_create_context()` / `pops_destroy_context()` must not run
at the same time as either.

## User-defined patterns

The library ships with the 20 factory rhythms and a second set of 20 pattern
slots that the host fills itself, plus a switch that points the sequencer at
those slots instead of at the factory rhythms. The factory rhythms stay
read-only in either mode.

A user pattern is a complete pattern and not a rhythm plus the shared
percussion layer: its grid may use every instrument, the Tambourine, Guiro
and Quijada included, and the shared layer is never added to it.

```c
#include <pops.h>

struct pops_user_pattern pattern;

/* 64 columns of 16-bit instrument masks, bit n = instrument n plays. */
for (unsigned int column = 0; column < 64; column++) {
    pattern.notes[column] = ((column % 8) == 0)
        ? (uint16_t)((1u << POPS_INSTR_BASS) | (1u << POPS_INSTR_COWBELL))
        : 0;
}
pattern.timesig = POPS_TIMESIG_4_4;  /* the metric of the grid */

pops_set_user_pattern(ctx, 0, &pattern);  /* slot 0 = bank A, button 0 */
pops_set_pattern_source(ctx, POPS_PATTERN_SOURCE_USER);
pops_select_rhythm(ctx, POPS_BANK_A, pops_rhythm_button_bit(POPS_RHYTHM_WALTZ));
```

The time signature is the only way to shape the grid: the library derives the
step-enable mask from it, so a pattern cannot carry a custom mask (sorry, no
3/5 patterns for now). Valid signatures are 2/4, 3/4, 4/4, 5/4, 6/8 and 7/4.
`pops_timesig_enable_mask()` returns the mask of each of them. The 2/4, 5/4 and
6/8 signatures play on the compound grid; the others play straight.

`pops_get_user_pattern()` reads a slot back, `pops_set_user_pattern(ctx, index,
NULL)` empties one, and `pops_reset()` keeps the slots but returns to the
preset source. A pattern that is playing changes with the pattern.

## Synthesis tweaks

The extended build (`LIBPOPS_EXTRA`, which is the default) exposes a few
synthesis parameters that the original unit keeps fixed. They are not panel
controls, and they start from the factory values, so a context that never
touches them sounds exactly like the preset build.

```c
pops_set_accent_gain(ctx, 78643);                        /* accented columns, 1.2x (Q16 units) */
pops_set_instr_decay(ctx, POPS_INSTR_SNARE_1, 65536);    /* Snare 1 decay, 1.0x (Q16 units) */
pops_set_instr_decay(ctx, POPS_INSTR_BASS, 65536);       /* Bass decay, 1.0x (Q16 units) */
pops_set_instr_cutoff(ctx, POPS_INSTR_SNARE_1, 5500);    /* Snare 1 band-pass, Hz */
pops_set_instr_cutoff(ctx, POPS_INSTR_SNARE_2, 5500);    /* Snare 2 band-pass, Hz */
pops_set_instr_cutoff(ctx, POPS_INSTR_HIHAT, 10000);     /* Hihat band-pass, Hz */
pops_set_instr_highpass(ctx, POPS_INSTR_CYMBAL_1, 5500); /* Cymbal 1 high-pass, Hz */
pops_set_instr_tune(ctx, POPS_INSTR_BASS, 131072);       /* Bass pitch, 2.0x (Q16 units) */
pops_set_instr_tune(ctx, POPS_INSTR_COWBELL, 98304);     /* Cowbell pitch, 1.5x (Q16 units) */
```

The accent gain applies to user-pattern columns that set the
`POPS_PATTERN_ACCENT` bit, and is read when the column fires, so a change
takes effect on the notes triggered after it. The decay controls scale the
factory decay time of every instrument that has one: all of them but the
Guiro, whose length is the gate the rhythm holds open rather than a decay.
On the Conga and the Cowbell the multiplier scales both tones' decays
together, so the instrument keeps its shape. A note that is already
sounding keeps playing and decays at the new rate. The cut-offs move the
band-pass centre of the instrument, for all three of them. The Hihat also
has a high-pass, which the cut-off does not move: its factory value of
1000 Hz removes the low end of the noise, and opening a high-pass past
the band-pass would only dull the sound instead of opening it up.

The two Cymbals are the other end of the chain: their band-pass is fixed
at its factory centre of 5600 Hz, so the high-pass is the tone control of
their voice. `pops_set_instr_highpass()` moves it over 2000..5500 Hz. The
ceiling is the factory corner of 5500 Hz, a hundred Hz below the
band-pass centre, so the band-pass can never end up under the high-pass
and the cymbal can never be thinned past the factory sound; the floor is
the same 2000 Hz the Hihat's cut-off starts at, because below it the
metallic voices carry low-mid wash that belongs to no metallic voice.
Lowering it lets more of the low band through and fattens the splash; at
the top of the range the voice is the factory tick. The Snares, the
Hihat and the Tambourine keep their high-passes fixed, so a call for
those instruments is ignored.

The tuning multiplier transposes an instrument: 65536 is the factory
pitch, 131072 is an octave up, 32768 is an octave down, and the setter
clamps the range to 0.5..2.0. It scales what is pitched in the voice:
the body oscillator of every instrument, the second tone of the Conga
and the Cowbell as well, and, in the noise voices, the band-pass centre
composed with the cut-off (the Cymbal and the Tambourine, which have no
cut-off, use their factory band-pass) and the high-pass. The noise
voices' high-passes follow the tuning so that the voice transposes
whole: the Snare 1, Snare 2 and Hihat high-passes sit far below their
band-passes and go along inaudibly, while the Cymbal and the Tambourine
carry theirs close to or above their band-pass and hold their character
at every setting. The stored high-pass is the corner at the factory
pitch, so the Cymbal's corner moves with the multiplier the same way
its band-pass does. The Guiro's ratchet and the Quijada's click clock are
timing, not pitch, and stay at the factory rate. A note that is
sounding follows the change, and a scaled centre or high-pass is
limited to two fifths of the sample rate like the cut-offs are.
None of them costs anything per sample: the accent is folded into the gain
the mixer already applies to a voice, and the others only recompute
coefficients between blocks.

`pops_get_instr_decay()`, `pops_get_instr_cutoff()`,
`pops_get_instr_highpass()` and `pops_get_instr_tune()` read the values
back. The high-pass getter returns 0 for an instrument that has no
*settable* high-pass: the Tambourine, the Hihat and the Snares have one,
but it is not theirs to change.

The cut-offs are also limited to two fifths of the sample rate, because a
biquad is only stable below Nyquist. At 44100 Hz that limit is above
15000 Hz, so the whole documented range is available at the default rate.
The high-passes are limited the same way, and at 44100 Hz the limit is
above their 5500 Hz ceiling.

`pops_reset()` restores the factory defaults for all of them.

## Reverb

Build with `-DLIBPOPS_FX_REVERB` to compile the optional stereo reverb. It is
a send effect: every instrument has its own send level, and the wet return is
added to the dry mix.

```c
pops_set_instr_fx_send(ctx, POPS_INSTR_SNARE_1, 65536);  /* full send */
pops_set_instr_fx_send(ctx, POPS_INSTR_BASS, 0);         /* no send */
```

The reverb's room size, level, damping and width are fixed at build time, so
the send level is the only per-instrument reverb control. The defaults are a
mid-size room, a 70% wet return, 10% damping and 70% width. Override them
with the build macros `MP7_FX_ROOM_SIZE_Q16`, `MP7_FX_LEVEL_Q16`,
`MP7_FX_DAMPING_Q16` and `MP7_FX_WIDTH_Q16` (Q16 values, 65536 = 1.0).

The send levels default to 0, so enabling the reverb does not change the
factory mix until a send is raised. The bus is bypassed completely when no
instrument sends to it; the delay memory is allocated once with the context.
`pops_get_instr_fx_send()` reads the send values back. Without the build macro
the reverb never renders.
