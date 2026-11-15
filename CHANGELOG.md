# Changelog

## Unreleased

Fixed the lamp flash of a freshly started transport.

- The flash counter now also advances at the first beat of a started
  transport, so the downbeat lights the pilot lamp when it happens instead of
  waiting for the first enabled column past the half-cycle or the cycle wrap

## 0.6.1 (2026-11-14)

External step clock support.

- New `pops_next_step()` explicitly advances the sequencer one step when the
  transport runs on an external sync

## 0.6.0 (2026-11-14)

Better tempo precision and a cymbal tone control.

- `pops_set_tempo()` accepts a fractional BPM part in Q16 units
- New `pops_set_tempo_us()` sets the tempo in MIDI-style microseconds per
  quarter note.
- Internal drift compensation keeps the tempo exact when following an
  external clock.
- New `pops_set_instr_highpass()` moves the high-pass of the two cymbals over
  2000-5500 Hz as their tone control. Snares, hihat and tambourine keep
  their fixed high-passes

## 0.5.0 (2026-11-13)

Project renamed from libpoppy to libpops to avoid ambiguities, and sound
adjustments widened.

- Header renamed to pops.h, symbols renamed to `pops_*`, and the build,
  examples and docs updated for the new name
- New per-instrument tune (pitch multiplier) setting
- The decay multiplier now covers every instrument that has a decay (all but
  the guiro), scaling both tones of the conga and the cowbell together

## 0.4.0 (2026-11-12)

Added the reverb engine, ported from hivedrum with parameters pinned down.

- Per-instrument FX send level, with the bus bypassed completely while no
  instrument sends to it
- Room size, wet level, damping and width are fixed at build time

## 0.3.0 (2026-11-02)

API cleanup, no changes in audio output.

- The instrument-specific synthesis tweaks (snare 1 decay, snare 1 cut-off
  and the hihat cut-offs) were generalized into per-instrument decay and
  cut-off setters.
- The pattern retrieval API was refactored to hand every external consumer
  the same pattern composition information.

## 0.2.0 (2026-11-01)

User-defined patterns and the first synthesis tweaks.

- A second set of 20 host-writable user patterns, gated behind an extended
  build flag, with the pattern written on a time signature: the step-enable
  mask derives from the signature and the grid follows the mask, so the
  explicit triplet rate setting is gone
- The 5/4 signature joins the compound (shuffle) pattern group, a user
  pattern view assumes the whole 64-column cycle
- Columns can carry an accent, lifted by an accent gain in the extended
  build, which also exposes the first synthesis tweaks: snare 1 decay, snare
  1 band-pass cut-off and the hihat high-pass cut-off
- The extended build's reset keeps the user pattern slots and returns the
  pattern source to the factory rhythms
- Per-instrument pan setting, instrument gain ranges adjusted to have some
  headroom, default cowbell and bongo output levels reduced.

## 0.1.0 (2026-10-24)

First released version of the vintage drum machine engine with 15 voices and
20 preset rhythm patterns.

- Integer-only, 32-bit fixed-point DSP core with precomputed sine and voice
  tables
- Rhythm sequencer with tempo control, pattern combination in a bank and
  defer-to-next-cycle rhythm selection
- Panel state query for a host UI, including the lamp flash counter and the
  playhead column
- Voice mixer with master volume and balance and per-instrument gain
- Compact pattern storage, quadrant-stored interpolated sine table and
  selective size optimizations
