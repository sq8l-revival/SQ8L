# Tuning (MTS-ESP)

Port addition. The original plug-in has no counterpart, so nothing here is verified against
it; what *is* verified is that the engine is byte-identical to the original while the feature
is off (`tests/test_render.py`, both configurations).

Sources: `src/engine/Tuning.{h,cpp}` (the snapshot and the math), the per-voice clock in
`src/engine/Doc.{h,cpp}`, one line in `Master::controlUpdate` and one guard in
`Master::midiNoteOn`. Tests: `tests/test_tuning.py` (the math alone) and
`tests/test_tuning_engine.py` (the clock inside `Doc`, and the `Master`/`Synth` wiring).

## The snapshot

`sq8l::Tuning` holds the master's retuning ratio per key (`MTS_RetuningAsRatio`, its
deviation from 12-ET, 1.0 = none), a 128-bit "key not in the scale" mask and three flags. Both
modes work from the ratio; the absolute frequency is never needed, so the client is asked for
one number per key rather than two. The plug-in refreshes one per audio block, before the MIDI events are dispatched; the
engine only ever reads it. No MTS-ESP call happens on the MIDI path or in the voice loop, and
the engine carries no dependency on the client library — the tests drive it with synthetic
tunings.

`active()` is `enabled && connected`: the option is on *and* a master is present. The engine
does nothing at all when the snapshot is inactive or the pointer is null.

The two switches belong to the **instance**, not to `SQ8L.ini`: `Synth::mtsEnabled` and
`Synth::mtsCorrectPitch`, both off by default, saved in the plugin's own state in a spare
header byte of the chunk (`0x1d`, bit 0 enable, bit 1 correct-pitch) beside the polyphony
override. A global would not be recalled with a project and would be shared by every instance
in it. A chunk with both off is byte-identical to the original's, since the original writes 0
there and 0 means both off.

## Why the clock and not the frequency register

The obvious injection point is `DocOscParams::pitchMod`, which `Doc::update` adds to the
pitch before the table lookup. It is reachable from one line and is honoured on every control
tick, so it retunes held notes — but `docPitchToFreq` quantizes hard. It shifts a
1/256-semitone pitch down to a 1/32-semitone index, then interpolates `kDocPitchTable`, a
table of **integer 16-bit DOC frequency registers** at 1/16-semitone spacing:

| Quantity | Value |
|---|---|
| Index step | 2.91–3.15 cents, uniform |
| Error over one semitone of retune, in 1/256 steps | max 2.98 cents, RMS 1.62 cents |

Underneath that the register's own LSB varies by key and wave: 0.243 cents at key 84, 0.971
at key 60, 1.940 at key 48, 7.746 at key 24 (wave 0), and 44.97 cents at wave 68 key 12,
where the DOC `resolution` field is already 7. No amount of table interpolation fixes the
bass, because there the register itself is the floor.

Instead, each voice gets its own **resampler clock**. `Doc::render` produces a new DOC sample
whenever the 30-bit resampler phase wraps, so a voice's effective DOC clock is
`sampleRate * phaseInc / 2^30` and scaling `phaseInc` scales that voice's oscillator pitch by
exactly the same ratio:

| Sample rate | `phaseInc` | 1 LSB | Upward headroom |
|---|---|---|---|
| 44100 | 936318848 | 1.85e-6 cents | +2.37 semitones |
| 48000 | 860242944 | 2.01e-6 cents | +3.84 semitones |
| 96000 | 430121472 | 4.03e-6 cents | +15.84 semitones |

`phaseInc` is `Trunc(Single(docRate / sampleRate * 2^30))` — note the `Single`, which
quantizes it to float32 granularity (64 at this magnitude) before truncating. Upward range is
bounded because `render` assumes at most one DOC step per host sample, which needs
`phaseInc < 2^30`; downward it is unbounded.

Nothing in the oscillator is touched: `freq`, `accMask`, `shift`, `resolution`, `bank`, `page`
and the wave ROM index sequence are exactly what the original computes. Timbre is therefore
bit-identical and the three oscillators keep their programmed detune as ratios. Hard sync, the
AM path and one-shot wave durations are all driven by the DOC clock, so they scale with the
pitch — which is what transposing should do.

## The math

Per voice, at each full control tick. `pBase` comes from `Doc::basePitch` for oscillator 0's
wave at semitone 0, fine 0 — the voice's root pitch, which is the anchor.

```
deviation = tuning.ratio[key]                        the master's own MTS_RetuningAsRatio
absolute: deviation *= idealRegister(anchor) / docPitchToFreq(anchor)

off      = 3072 * log2(deviation)
n        = round(off / 256)                          whole semitones
pitch   += n * 256                                   joins the bend on the existing path
ratio    = docPitchToFreq(anchor) * 2^(off/3072) / docPitchToFreq(anchor + n*256)
phaseInc = round(nominalPhaseInc * ratio)
```

where `anchor = pBase - 16`, the bias the original applies before the lookup.

Three properties make this exact rather than merely close:

* The denominator is *the register the engine will really use*, so the ratio absorbs the
  table's quantization, the table's own rounding, and any clamping of `n`.
* `off == 0` gives `ratio` exactly `1.0`, so an untuned voice is bit-identical.
* `n = round(off/256)` keeps the residual within half a semitone, so `ratio` stays in
  [0.971, 1.030] — far inside the headroom at the lowest supported sample rate — while
  arbitrarily large retunings are still supported.

**Relative** mode takes the master's own retuning and nothing else, so it assumes nothing
about the reference pitch. **Absolute** mode additionally divides out the SQ-80's own per-key
offset, which is entirely the pitch table's integer rounding: the real register against
`idealRegister`, the smooth exponential the table approximates
(`kIdealRef * 2^(pitch/3072)`, derived in `Tuning.cpp` from A440, the DOC rate and the wave-0
geometry).

Comparing *registers* rather than frequencies is what makes absolute mode work. The sounding
pitch is the register times `docRate / 2^(res+17)` times the number of cycles the wavesample's
table holds, and those factors change together across a multisample split — PIANO's record
changes at MIDI 55/56, where the resolution goes 3 → 1 and the table holds a quarter as many
cycles. They are all exact powers of two chosen so the instrument plays in tune, so they
cancel out of `register / idealRegister` and never need to be modelled. An earlier version
derived the sounding pitch physically, read PIANO's keys ≤ 55 as two octaves flat and
"corrected" them upward — which dragged the patch's other oscillators with it, since one clock
serves the whole voice.

Measured: for a sub-semitone retune the pitch table contributes nothing (`n == 0`) and the
clock delivers the whole thing at about 1e-13 cents of ratio error, against the full requested
deviation if the clock is ignored.

## Why relative is the default

The SQ-80's own tuning is not equal temperament. From the pitch table's integer rounding,
wave 0 across keys 24–108 deviates from 12-ET by **−1.483 to +0.547 cents**, key dependent and
also wave dependent, and A4 sounds at **439.9456 Hz** (−0.214 cents). That wobble is part of
the instrument, so by default only the master's *deviation* from 12-ET is applied and a master
running plain 12-ET is a perfect no-op. *Correct SQ-80 per-key pitch offsets* switches to
absolute, which irons the wobble out.

Note that absolute mode anchors the voice's root pitch, oscillator 0's wave at semitone 0 and
fine 0. One clock serves the whole voice, so the other two oscillators are corrected by the
same factor and keep their own table error, bounded by the same few cents; an oscillator
detuned by SEMI or FINE likewise carries its own. Measured over keys 24–108, the correction
absolute mode applies stays under 2.5 cents for every wave, rising towards 20 cents only at
the very bottom of the keyboard where the frequency register itself is coarse.

## The smoothing poles

The DCA (2 ms) and AM (0.2 ms) smoothers are one-poles evaluated **once per DOC sample**, so a
changed clock would stretch their time constants. Each voice therefore carries its own poles,
computed for its own clock:

```
pole = 0.01 ^ (1 / (T * docRate * ratio + 1)),   gain = 1 - pole
```

`Doc::setAmpTarget` and `Doc::render` read the voice's values instead of the `Doc`'s, so 2 ms
stays 2 ms and 0.2 ms stays 0.2 ms at any retuning.

`delphiPower` runs CORE-MATH's correctly rounded `exp`/`log` and is far too slow for every
control tick of every voice, so the poles are recomputed only when the residual crosses a
**1/64-semitone** step (4 units of 1/256, i.e. 1.5625 cents, with the rounding boundary at
0.78 cents). That bounds the time-constant error at 0.09% while the pitch keeps full
precision. At `ratio == 1.0` the nominal values are **copied verbatim** rather than
recomputed, so the untuned path cannot drift by a float LSB.

## Where the state lives

`Doc::clock[]`, an array of `VoiceClock`, is a member of `Doc` **outside the original's object
image** — deliberately not a `DocVoice` field. `tests/capi_doc.cpp` maps the global `Doc`
fields one offset at a time (`globalFields`) but round-trips the voice blocks with a
whole-struct `memcpy` of 0x200 bytes, so state kept inside `DocVoice` would be overwritten
from the original's image on load and written back into the compared image on save, breaking
the differential tests the moment a field held a non-zero value. `Doc` already carries a
port-only member in that region (`capacity`), and there is no `static_assert` on `sizeof(Doc)`.

`setSampleRate`, `setDocRate`, `resetVoice` and `startVoice` all put the affected voices back
on the nominal clock, so a reused slot never inherits the previous note's tuning.

## Known behaviour

* **Glide.** The offset of the *destination* key applies throughout a glide, so the glide
  trajectory is the original's, shifted. Interpolating the offset along the glide was
  considered and dropped as unnecessary.
* **Pitch bend and glide stay quantized.** They ride the original's table path, so bends sound
  the same whether or not a master is connected.
* **Extreme scales saturate.** `n` is reduced until `pBase + n*256` is inside the table, so
  `docPitchToFreq`'s octave fold never engages, and `phaseInc` is clamped to
  `[1, 2^30 - 1]`. A scale transposing several octaves at the keyboard extremes therefore
  saturates smoothly rather than dropping an octave or freezing the resampler.
* **A master reporting a non-finite or non-positive frequency** retunes nothing at all; the
  voice keeps the nominal clock.
* **The engine is omni** (`Master::midiNoteOn` discards the channel), so the client asks for
  channel −1 and there is no per-note or MPE tuning.
