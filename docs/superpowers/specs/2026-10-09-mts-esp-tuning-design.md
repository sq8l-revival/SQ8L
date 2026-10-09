# MTS-ESP support via per-voice DOC clock scaling

Design document. Status: **implemented** on branch `mts-esp`. Written against `main` at
`88ba6b9`; PR #26 has since merged and the notes below are updated accordingly. Where the
implementation departed from this document, the departure is recorded in place.

## Dependencies

**PR #11 (polyphony per program) has merged** — `d17e913` … `88ba6b9`. One
prediction in the earlier draft of this document was wrong and is corrected
throughout: #11 did *not* remove the Polyphony entry from OPTIONS. It reshaped it
into a **per-instance override** ("Set by program (EMU->VOICES parameter)", then
1 to 64 voices), saved with the plugin's state rather than in `SQ8L.ini`. It
stays in the menu, and §7a gives it a home.

**PR #26, "Remove HD Graphics option, improve drawing at 100% zoom, much faster
repaints"** has merged (`4e220d1`, `8cafc9c`, `f4b5634`). It overlapped the GUI half of this
design, which is now built on top of it; the engine half never overlapped it.

| File | PR #26 | Effect on this design |
|---|---|---|
| `src/gui/logic/EditorController.{h,cpp}` | −9/−2 | deletes the `HD graphics` item, `hdClick`, `menuHd_` — §7a's layout loses its last entry |
| `src/gui/text/StbTextRenderer.{h,cpp}` | +18/−2 | grid fitting changes `embolden`'s meaning — §7b must retune, see there |
| `src/engine/Settings.h` | −6 | removes `hd`; this design adds two `[port]` keys to the same file |
| `src/gui/hd/HdRenderer.{h,cpp}`, `tests/hd_check.cpp` | +116/−38 | HD becomes always-on — §8's "both modes" premise is gone |
| `src/plugin/SQ8LUI.cpp` | +19/−40 | `changedTiles()` repaint — helps §7b rather than hindering it |
| `tests/test_gui_extensions.py` | +3/−6 | strips HD assertions; §7a adds to the same file |
| `README.md` | +5/−6 | removes the HD bullet; this design adds two |

Both halves are implemented. The engine half was built and gated first, against `main`.

Untouched by #26, so nothing under the engine half has moved: `Doc.{h,cpp}`,
`Master.{h,cpp}`, `Synth.{h,cpp}`, `VoiceModules.h`, `SQ8LPlugin.{hpp,cpp}`,
`CMakeLists.txt`, `tests/capi_doc.cpp`, `tests/render_check.cpp`.

## Intent

Give SQ8L MTS-ESP client support with pitch accuracy far below the audible
threshold, without altering the oscillator engine's arithmetic, and with
byte-identical output when the feature is off or no master is connected.

The feature is a port addition in the same family as *Zoom* and *Ask before
loading banks/libraries* — a global preference saved in `SQ8L.ini`. Everything
the original did stays exactly as it was, and the new exactness applies only to
the microtuning offset.

Success criteria:

* Tuning error from the retuning mechanism itself below 0.01 cents.
* Timbre indistinguishable from stock: the frequency register, wave ROM index
  sequence, bank/page, resolution and the three oscillators' relative detune are
  all exactly what the original computes.
* DCA (2 ms) and AM (0.2 ms) smoothing time constants unaffected by retuning.
* The 1176-render regression suite byte-identical in two configurations: feature
  off, and feature on with no master present.

## Why the obvious approach does not work

The natural injection point is `DocOscParams::pitchMod` (1/256 semitone, added
after the pitch lookup at `Doc.cpp:370`). It is reachable from a single line in
`Master::controlUpdate` and is honoured on every control tick, so it retunes held
notes. But the lookup it feeds quantizes hard.

`docPitchToFreq` (`Doc.cpp:85`) shifts a 1/256-semitone pitch down to a
1/32-semitone index, then interpolates `kDocPitchTable`, a table of **integer
16-bit DOC frequency registers** at 1/16-semitone spacing. Measured:

| Quantity | Value |
|---|---|
| Index step | 2.91–3.15 cents, uniform |
| Error sweeping one semitone of retune in 1/256 steps | max 2.98 cents, RMS 1.62 cents |

Underneath that, the register's own LSB varies by key and wave: 0.243 cents at
key 84, 0.971 at key 60, 1.940 at key 48, 7.746 at key 24 (wave 0), and 44.97
cents in the worst case found (wave 68, key 12, where `res` is already 7). So
`pitchMod` alone cannot deliver fine tuning, and no amount of table
interpolation fixes the bass.

A secondary mechanism was evaluated and **rejected**: raising the DOC `resolution`
field by `k` while scaling `freq` and `acc` by `2^k` is an algebraically exact
transformation (the ROM index `acc >> shift` is invariant, and so is
`freq << (sizeLog2-1-res)`, which drives the interpolator order). It reaches
0.030 cents on 51% of wave/key pairs and ≤1 cent on 88%, but the 27 sampled and
one-shot waves (49–74) already sit at `res = 7` and cannot improve. Recorded here
because the invariants are useful knowledge, not because we are using it.

## Mechanism

Each voice resamples the DOC output to the host rate with a 30-bit phase
accumulator (`Doc.cpp:578`):

```
vc.phase = (vc.phase + phaseInc) & kPhaseMask;      // kPhaseMask = 0x3fffffff
```

with `phaseInc = Trunc(Single(docRate / sampleRate * 2^30))` (`Doc.cpp:155`) -- note the
`Single` rounding, which quantizes the increment to float32 granularity (64 at this
magnitude) before the truncation. A new DOC
sample is produced whenever that wraps, so **the effective DOC clock of a voice is
`sampleRate * phaseInc / 2^30`**, and scaling `phaseInc` scales that voice's
oscillator pitch by exactly the same ratio.

| Sample rate | `phaseInc` | Fraction of 2^30 | 1 LSB | Upward headroom |
|---|---|---|---|---|
| 44100 | 936318848 | 0.8720 | 1.85e-6 cents | +2.37 semitones |
| 48000 | 860242944 | 0.8012 | 2.01e-6 cents | +3.84 semitones |
| 88200 | 468159424 | 0.4360 | 3.70e-6 cents | +14.37 semitones |
| 96000 | 430121472 | 0.4006 | 4.03e-6 cents | +15.84 semitones |
| 192000 | 215060736 | 0.2003 | 8.05e-6 cents | +27.84 semitones |

Upward range is bounded because `phaseInc` must stay below `2^30` for `render`'s
one-DOC-step-per-host-sample assumption (`if (vc.phase <= vc.prevPhase)`) to hold.
Downward it is unbounded.

The mechanism touches nothing in the oscillator: `o.freq`, `o.accMask`, `o.shift`,
`o.resolution`, `o.bank`, `o.page` and the ROM index sequence are untouched, so
timbre is bit-identical and the oscillators' programmed detune is preserved as
ratios. Hard sync, the AM path and one-shot wave durations are all driven by the
DOC clock, so they scale with pitch — which is what transposition should do.

Validation of the model: the oscillator's cycle rate is
`freq * docRate / 2^(res+17)`. Evaluating it for wave 0 yields 32.7135 Hz at key
24, 261.5615 Hz at key 60 and 4185.42 Hz at key 108 — recognisable pitches,
confirming the clock relationship the design rests on.

## SQ8L is not in equal temperament

A consequence of the table's integer rounding, measured for wave 0 across keys
24–108:

* Deviation from 12-ET: **−1.483 to +0.547 cents**, key-dependent, and also
  wave-dependent.
* A4 (key 69) sounds at **439.9456 Hz**, i.e. −0.214 cents.

This wobble is part of the instrument, which is why relative mode is the default
(see below) and why "off" is the default state of the feature.

## Scope

In scope:

* Per-voice DOC clock scaling driven by an MTS-ESP master.
* Per-voice DCA/AM smoothing correction so the time constants are immune to the
  clock change.
* Two tuning modes, relative (the default *when enabled*) and absolute. The
  feature itself defaults to off.
* Note filtering (`MTS_ShouldFilterNote`).
* OPTIONS → MTS-ESP submenu with two checkable items, *Enable* and *Correct
  SQ-80 per-key pitch offsets*, and a checkmark on the parent entry when enabled.
* **Regrouping the OPTIONS menu** into the layout in §7a, as one unconditional
  tree, with the OPTIONS menu dropped from the oracle comparison and its layout
  left untested.
* **Replacing the menu's hard-coded index bookkeeping** with indices recorded at
  push time (§7a) — a precondition for the layout being free to change.
* The master's scale name in the top bar. Its emphasis — bold, italic or plain —
  is left open and settled by eye at the end (§7b).

Out of scope (non-goals):

* Making pitch bend or glide exact. They stay on the original's quantized table
  path, so bends sound the same whether or not a master is connected.
* MPE or per-note channel tuning. The engine is omni — `Master::midiNoteOn`
  already discards the channel — so `MTS_NoteToFrequency` is called with channel
  −1.
* Any change to the frequency register path, the `resolution` field, or
  `docPitchToFreq`.
* MTS-ESP master (`libMTSMaster`) support.

## Design

### 1. Tuning snapshot

The engine gains a plain value type with no dependencies:

```cpp
// src/engine/Tuning.h
struct Tuning {
    bool     enabled = false;         // OPTIONS -> MTS-ESP -> Enable
    bool     correctNativeOffsets = false;  // ... -> Correct SQ-80 per-key pitch offsets
    bool     connected = false;       // a master is present
    double   hz[128] = {};            // target frequency per key
    uint32_t filtered[4] = {};        // 128-bit mask: key excluded from the scale
    bool active() const { return enabled && connected; }
};
```

The two flags are independent booleans rather than a tri-state mode, matching the
two checkable menu items. `correctNativeOffsets` selects **absolute** tuning;
clear, the default, it selects **relative**. The option is named for what it does
— SQ8L's per-key pitch offsets are real and measurable (see above) — rather than
for the abstraction.

The plugin refreshes one snapshot per block; the engine only reads arrays. No
external call happens in the MIDI path or the voice loop, which keeps the
realtime path free of the MTS-ESP shared-memory API and makes the engine testable
with synthetic tunings.

Rejected alternatives:

* `libMTSClient` inside `sq8l_engine` — pulls `dlopen` into the static library
  that `sq8l_render_check` and every `capi_*` test binary link, and makes engine
  behaviour depend on an external process.
* A `TuningSource` virtual queried per voice per control tick — same plumbing, a
  virtual call in the voice loop, and no benefit while the synth is omni.

### 2. Plugin layer

New `src/plugin/MtsEspTuning.{h,cpp}` owning the vendored client:

* `SQ8LPlugin` ctor registers the client, dtor deregisters it.
* At the top of `run()`, before `processEvents`, refresh the snapshot: `connected`
  from `MTS_HasMaster`, `hz[k]` from `MTS_NoteToFrequency(client, k, -1)`,
  `filtered` from `MTS_ShouldFilterNote`, and the scale name from
  `MTS_GetScaleName`.
* When the mode is `Off`, skip the refresh entirely and leave the snapshot
  inactive, so the feature costs nothing until enabled.

The scale name reaches the editor through the existing editor-message queue
(`SQ8LPlugin::notifyEditor` / `takeEditorMessages`), or through shared state read
under `engineMutex()`; the implementation plan picks whichever fits the existing
pattern with less new surface.

### 3. Pitch math

Per voice, in `Master::controlUpdate`, after the pitch-bend block and before the
per-oscillator loop. Definitions for a voice on key `k`:

* `res`, `s`, `fineOfs` come from `computePitch` for **oscillator 0's wave with
  `semitone = 0`, `fine = 0`** — the voice's root pitch. This is the anchor.
* `pBase = (pitchKey + s) * 256 + fineOfs`
* `E(k) = 440 * 2^((k - 69) / 12)` — the 12-ET reference
* `T(k) = tuning.hz[k]` — the master's target
* `A(k) = docPitchToFreq(pBase - 16) * docRate / 2^(res + 17)` — SQ8L's native
  absolute frequency at the anchor

*As implemented:* absolute mode does **not** compare frequencies. Deriving the sounding pitch
physically needs the number of cycles the wavesample's table holds, which is not in the data
and changes across a multisample split — PIANO's record changes at MIDI 55/56, where the
resolution goes 3 → 1 and the table holds a quarter as many cycles, so the physical model read
keys ≤ 55 as two octaves flat and "corrected" them upward. Instead absolute mode divides the
real frequency register by the smooth exponential the table approximates: the SQ-80's per-key
offset is entirely that table's integer rounding, and the resolution, cycle count and DOC rate
are exact powers of two that cancel out of the ratio. The `hz` array and the 12-ET reference
below are therefore not needed at all; the snapshot carries only the master's ratio.

The retune offset in 1/256-semitone units, by mode:

```
relative  (correctNativeOffsets clear, default):
    off = 3072 * log2(T(k) / E(k))      // apply only the master's deviation from 12-ET
absolute  (correctNativeOffsets set):
    off = 3072 * log2(T(k) / A(k))      // correct SQ8L's per-key offsets too
```

Then, identically for both modes:

```
N        = round(off / 256)                        // whole semitones
pitch   += N * 256                                 // existing accumulator
fAnchor  = docPitchToFreq(pBase - 16)
fActual  = docPitchToFreq(pBase + N*256 - 16)      // the register really used
ratio    = fAnchor * 2^(off / 3072) / fActual
phaseInc = clamp(round(nominalPhaseInc * ratio), 1, 2^30 - 1)
```

Three properties make this correct rather than merely close:

* The denominator is *the register the engine will actually use*, so the ratio
  absorbs the table's 3-cent quantization, the table's own rounding error, and
  the octave fold in `docPitchToFreq`, automatically.
* `off = 0` gives `ratio = 1` exactly, bit for bit.
* `N = round(off/256)` keeps `|residual| ≤ 0.5` semitone, so
  `ratio ∈ [0.971, 1.030]` — far inside the +2.37-semitone headroom at the lowest
  supported sample rate — while supporting arbitrarily large retunings.

In **relative mode the `docRate / 2^(res+17)` factors cancel out of the ratio**, so
that mode needs no absolute-frequency model and cannot introduce modelling error.
Absolute mode does need the model, and therefore depends on the anchor choice
above.

Clamping:

* `pBase + N*256 - 16` must stay in `[0, 0x8000)` so the octave fold in
  `docPitchToFreq` never engages. Clamp `N` to keep it there and let `ratio`
  absorb the remainder up to the `phaseInc` headroom; beyond that the pitch
  saturates. This only arises for scales that transpose several octaves at the
  keyboard extremes.
* `phaseInc` clamped to `[1, 2^30 - 1]`.

Documented behaviours that follow from the design, not bugs:

* During a glide the offset of the *destination* key applies throughout, so the
  glide trajectory is the original's, shifted. Interpolating the offset along the
  glide was considered and dropped as unnecessary.
* For sampled and one-shot waves (54 and above) there is no single meaningful
  cycle rate, so absolute mode's reference is approximate for those. Relative
  mode is unaffected.

### 4. Smoothing correction

The DCA and AM smoothers are one-poles evaluated **once per DOC sample**, so a
changed clock would stretch their time constants. Per voice, for the effective
rate `docRate * ratio`:

```
pole = 0.01^(1 / (T * docRate * ratio + 1)),   gain = 1 - pole
```

with `T = kSmoothTime` (2 ms, DCA) and `kAmSmoothTime` (0.2 ms, AM), matching
`Doc::computeSmoothing` (`Doc.cpp:141`). Four per-voice floats replace the
Doc-level values at their use sites:

* `Doc::setAmpTarget` — premultiplies the target by `gain`; already called per
  voice from both `update` and `interpolateLevels`.
* `Doc::render`, non-AM branch — `smoothPole`.
* `Doc::render`, AM branch — `amSmoothGain` and `amSmoothPole` directly.

Net effect: 2 ms stays 2 ms and 0.2 ms stays 0.2 ms at any retuning.

Recompute policy. `delphiPower` uses CORE-MATH's correctly rounded `exp`/`log`
and is too slow to run per voice per tick, so the poles are recomputed only when
the voice's residual crosses a **1/64-semitone** step (4 units of 1/256). That
bounds the time-constant error at 0.09%, while pitch keeps full precision. When
`ratio == 1.0` exactly, the Doc-level floats are **copied verbatim** rather than
recomputed, so the bit-exact path cannot drift by a float LSB.

### 5. Per-voice state placement

The state goes in a **new array member of `Doc`, outside the original's mapped
image** — *not* in `DocVoice`:

```cpp
// Per-voice resampler clock and the smoothing poles at that clock (port only;
// deliberately outside the original object image, see below).
struct VoiceClock {
    uint32_t phaseInc = 0;      // this voice's resampler increment
    float smoothPole = 0;       // DCA pole at this voice's clock
    float smoothGain = 0;       // 1 - smoothPole
    float amSmoothPole = 0;     // AM pole at this voice's clock
    float amSmoothGain = 0;     // 1 - amSmoothPole
    int32_t quantizedOffset = 0;  // residual the poles were computed for, 1/64 semitone
    bool valid = false;
};
VoiceClock clock[kMaxVoices] = {};
```

**An earlier draft put these in `DocVoice::reserved1b0[20]` (+0x1b0). That was
wrong.** `tests/capi_doc.cpp` maps the *global* Doc fields field-by-field
(`globalFields`, explicit offsets), but it round-trips the voice blocks with a
**whole-struct `memcpy` of `kVoiceSize` = 0x200 bytes** (`sq8l_doc_load`,
`sq8l_doc_save`, `sq8l_doc_load_voice`, `sq8l_doc_save_voice`). Anything stored
inside `DocVoice` would therefore be overwritten by the original's bytes on load
and written back into the saved image on save, so the differential comparison
would fail the moment a field held a non-zero value — which it does even at
`ratio == 1`, since the nominal `phaseInc` and poles are non-zero.

A separate `Doc` member avoids this completely: `globalFields` lists offsets
explicitly and never mentions it, the voice `memcpy`s do not reach it, and there
is no `static_assert` on `sizeof(Doc)`. `Doc` already carries a port-only member
in that region (`capacity`, right after `numVoices`), so the precedent exists.
`DocVoice` stays untouched at 0x200 bytes with every `static_assert` in
`Doc.cpp:14-33` intact, and the Doc-level nominal values stay at +0x2068,
+0x2080, +0x2084, +0x2088 and +0x208c where the differential tests read them.

New engine entry points:

* `Doc::setVoiceClock(uint32_t v, double ratio)` — fills `clock[v]`, copying the
  nominal values verbatim when `ratio == 1.0`.
* `Doc::setSampleRate` and `setDocRate` invalidate `clock[]` for all voices.
* `Doc::startVoice` and `resetAll` reset `clock[v]` to nominal.
* `VoiceModules` gains `docSetVoiceClock(int v, double ratio)`; `SynthModules`
  forwards it to `Doc`.

### 6. Note filtering

`Master::midiNoteOn` consults the snapshot's `filtered` mask and returns early
for an excluded key, before `noteEvent`. No external call in the MIDI path.

### 7. Settings and UI

Two `[port]` keys, following the convention that a port option is one integer:

*As implemented:* **not** `[port]` keys in `SQ8L.ini`. Both switches belong to the instance,
exactly like the polyphony override: a global would not be recalled with a project and would
be shared by every instance in it. They live in `Synth` (`mtsEnabled` / `mtsCorrectPitch`,
both off by default) and travel in the plugin's own state, in a third spare header byte of the
chunk (`0x1d`, bit 0 enable, bit 1 correct-pitch) beside the polyphony override's two. No
marker byte is needed: the original writes 0 there and 0 means both off, so a chunk with the
feature off stays byte-identical to the original's.

The editor reaches them through `EditorHost::mtsEnabled` / `setMtsEnabled` /
`mtsCorrectPitch` / `setMtsCorrectPitch`, mirroring `polyphonyOverride`, rather than through
`setPortSetting`.

#### 7a. OPTIONS menu reorganisation

The menu is regrouped as part of this work. Target layout:

```
Polyphony                           >
Voice stealing mode                 >
Emulation preferences               >
------------------------------------
Mouse                               >
Down arrow -> next program
------------------------------------
Ask before loading banks/libraries
------------------------------------
MTS-ESP                             >   [checkmark when enabled]
------------------------------------
Zoom                                >
```

`Polyphony` leads the first group. After PR #11 it is a **per-instance
override** — "Set by program (EMU->VOICES parameter)" then 1 to 64 voices, saved
with the plugin's state, not in `SQ8L.ini` — which puts it in the same family as
*Voice stealing mode* and *Emulation preferences*; the code's own comment makes
the point ("Like the emulation overrides above, but of this instance only").
First place is the reading order: how many voices there are, then what happens
when they run out, then the rest of the emulation.

Note that this makes the **very first** entry a port addition, conditional on
`portExtensions()`. With it false the menu starts at *Voice stealing mode* and
every following position shifts by one — which is precisely why the index rework
below is a precondition rather than a tidy-up.

`HD graphics` is absent because PR #26 removes the option and the editor is
always drawn at the window's resolution. `Zoom` is therefore alone in the last
group.

*As implemented:* captions do **not** end in `...`. An ellipsis means "this opens a
dialog", and every OPTIONS entry is a submenu parent, so none of them earns one; the `>` in
these sketches is just notation for the platform's own submenu arrow. The ellipses stay on
FILE's file dialogs and on INFO's two windows, *Modulation usage...* and *About...*, which do
open something. That also drops the ellipsis from the original's own "Voice stealing mode..."
and the four emulation submenus.

`Mouse` submenu:

```
Restore position after popup menus
Restore position after knob turning
------------------------------------
Right click on display -> scroll page
```

`MTS-ESP` submenu:

```
Enable
------------------------------------
Correct SQ-80 per-key pitch offsets
```

Both MTS-ESP items are checkable, and the **parent** `MTS-ESP` entry carries the
checkmark when `Enable` is on. Verified expressible without touching the platform
layer: all three backends apply `MenuItem::checked` independently of whether the
item has a submenu — Win32 sets `MFS_CHECKED` via `MIIM_STATE` alongside
`MIIM_SUBMENU` (`PlatformUiWin.cpp:40-48`), macOS sets `NSMenuItem.state`
(`PlatformUiMac.mm:205`), and the Linux drawn menus draw the mark themselves
(`PlatformUiDrawn.cpp:193`). AppKit's rendering of state on a submenu parent
should still be eyeballed on a real Mac.

**One tree, built unconditionally, and not tested at all.** The regrouping is not gated
behind `portExtensions()`.

*As implemented:* the popup-tree comparison was removed from `tests/test_gui_logic.py`
outright — for every menu, not just OPTIONS — and the OPTIONS layout assertions were removed
from `tests/test_gui_extensions.py`, which now locates the items it clicks by caption. The
menus are still driven, so every command they dispatch is still exercised and its effect on
the state and the edit buffer compared; only the trees go uncompared. The layout is left to
inspection: GUI parity is not a goal of the port, and pinning a layout that is expected to keep
moving would only generate churn.

GUI differential parity is not a project goal — the editor is being modernised,
and PR #26 is itself an instance of that (an option removed, text rendering
deliberately moved away from matching the original's hinted output). So the
earlier draft's argument for preserving a transformed comparison is moot:
`tests/test_gui_logic.py` simply stops comparing the OPTIONS tree
(`:37` lists it among the compared menus today; `:1031` does the comparison).
Every other popup — FILE, INFO, program, page — keeps comparing against the
oracle unchanged. The exclusion is narrow and specific to OPTIONS.

The port-added items keep the existing `if (host_.portExtensions())` condition
and their conditionally-assigned pointers (`EditorController.cpp:229-237`). With
extensions false the menu degrades to the original items in the new grouping,
which is acceptable and untested.

**Fix the index bookkeeping while we are here.** The cached `MenuNode*` pointers
are currently resolved by hard-coded position after the vector is built —
`menuPolyphony_ = &optionsMenu_.items[6]`, `menuZoom_ = &optionsMenu_.items[9]`
and so on (`EditorController.cpp:229-237`). Every layout change silently
renumbers them, and PR #26 had to delete `menuHd_ = &optionsMenu_.items[10]` by
hand. Placing `Polyphony` in the first group makes the following items'
positions depend on `portExtensions()`, which would make this worse.

So each pointer records its index **at push time** instead:

```cpp
const size_t iRmb = o.size();
o.push_back(item("Right click on display -> scroll page", ...));
...
menuRmbScrDisp_ = &optionsMenu_.items[iRmb];
```

This keeps the existing two-phase structure (indices during construction,
pointers once the vector is final, since pushing invalidates pointers) while
making the numbering self-maintaining. It removes the whole class of bug for
`menuRestMouseMenu_`, `menuRestMouseKnob_`, `menuRmbScrDisp_`, `menuPolyphony_`,
`menuSwapProgUpDn_`, `menuConfirmLoad_` and `menuZoom_` at once, and it is a
precondition for the layout being free to change again later.

`portExtensions()` keeps its other use at `EditorController.cpp:335` (left click
on the program number) untouched.

#### 7b. Scale name

The top bar has **247 px free** between `menuPanicImage` (ends at
x=249, `EditorView.cpp:208`) and `StatusPanel2` (starts at x=496,
`EditorView.cpp:172`), at y 4..22. A new `Label` sits at approximately
(255, 4, 235, 13), left-justified, blank when no master is connected.

Repaint: the label's updates flow through PR #26's `changedTiles()`, which
reports the tiles of a 32-pixel grid holding a changed pixel instead of one
bounding rectangle. A short status-bar-like text change is exactly the case #26
improved (its own measurement: 120 ms to 5.2 ms with the status bar following
along), so the scale name costs very little to update.

*As implemented:* the label is a transparent `Label` parented to the form and registered in
its `graphic_` list, not a `Label` inside a `Panel` of its own. A `Panel` fills its rectangle
with a flat colour, which would have covered background artwork the original leaves visible in
that span; `StatusPanel2` gets away with it because the original has a panel there too. The
emphasis is **italic** for now — a real embedded face, so no new plumbing — pending a look on
screen; the `Label::embolden` work described below was not done and is only needed if bold wins.

**Emphasis style: open, decided by eye at the end.** The label should read as
distinct from the voice counter beside it, but whether that is bold, italic or
plain is a visual judgement to make on screen, not on paper. Both routes are
available and the choice does not affect anything else in the design.

*Italic* is the cheaper of the two. `LiberationSans-Italic` is embedded as a real
face (`FontData.cpp`, `StbTextRenderer::italic_`), so `Font::italic = true`
selects it directly — no new field, no synthetic weight, and no interaction with
the grid fitting described below. The editor already uses italic for the button
captions, so it is in keeping.

*Bold* needs synthesising. Only **regular, italic and bold-italic** Liberation
Sans are embedded, and `StbTextRenderer` draws upright bold as regular
(`StbTextRenderer.h:3`); the weight would come from the existing `embolden`
parameter of `drawTextScaled`, as the panel labels use it (`HdPanel.cpp:238`,
0.33 for menu-style text). That means:

* `Label` gains an `embolden` field, plumbed through `Canvas::textOut` to the
  existing `TextRenderer::drawText(..., embolden)`.
* Only the new label sets it, so no existing control changes.
* No fourth font is added to the binary.

And if bold is chosen, **the value must be picked against PR #26's renderer, not
the old one.** #26 adds grid fitting to `drawTextScaled`: with
`sharp = clamp(2 - scale, 0, 1)` the origin snaps to the pixel grid, coverage is
pushed towards 0 or 1 by a contrast term, and critically

```
wider = max(embolden * (1 - 0.5 * sharp) * scale, 0)
```

so **emboldening is deliberately halved at 100% zoom** and fades back in as the
scale passes 200%. The contrast push already gives stems solid cores at small
sizes, so the same numeric `embolden` reads lighter at 100% and heavier at 300%
than it did before #26. Pick it by eye at 100% and check it at 300%; do not copy
0.33 across and assume it matches.

Either way, `Font::bold` semantics are deliberately **not** changed:
`HdPanel.cpp:234` sets `bold` together with `italic` to select the bold-italic
face, and emboldening that path too would double-bolden the panel labels.

Plan consequence: the `Label::embolden` plumbing is a **separate, last step**, so
it can be dropped entirely if italic or plain wins.

### 8. Rendering path

PR #26 makes the window's-resolution rendering unconditional: `OPTIONS -> HD
graphics` and the `[port] hd` key are gone, and the classic enlarged-pixel frame
remains only as a fallback where the window exceeds the largest texture, for the
drawn menus and dialogs, and as the source of the dirty rectangles.

So there is no longer a mode question. The scale label is a windowed `Label`
drawn by the text renderer, with `HdPanel` drawing only the panel artwork, and it
must be confirmed on screen rather than assumed — at 100% and at 300%, where the
grid fitting above behaves differently. `sq8l_hd_check` renders without a window
and is the cheap way to check; note #26 changes it (+28/−7), so build the check
on its updated form.

## Testing

* **Regression, byte-identical, twice.** The 1176-render suite
  (`tests/test_render.py`) must produce identical output with `mtsEsp = 0`, and
  with `mtsEsp = 1` and no master present. This is the gate before anything else
  is trusted.
* **Tuning accuracy.** Feed a synthetic `Tuning` with arbitrary per-key
  frequencies and assert the realised pitch is within 0.01 cents, read from
  `phaseInc` and `o.freq` through the C API rather than estimated from audio.
  Cover both modes, and assert relative mode with an exact 12-ET table produces
  `ratio == 1.0` and byte-identical audio.
* **Smoothing invariance.** Assert a DCA ramp's duration *in seconds* is constant
  across ratios spanning ±1 semitone, and that the AM smoother's time constant
  likewise holds.
* **Note filtering.** A filtered key produces no voice.
* **Oracle comparison: OPTIONS dropped.** `tests/test_gui_logic.py` stops
  comparing the OPTIONS tree against `oracle_gui.menu_tree`. Every other popup —
  FILE, INFO, program, page — keeps comparing unchanged, so the removal is
  specific to OPTIONS rather than a blanket relaxation.
* **No menu assertions at all**, as implemented: no tree comparison with the original and no
  layout or menu-state assertions of the port's own. `tests/test_gui_extensions.py` keeps its
  pre-existing *setting* checks (clicking an item changes the setting and the checkmark
  follows), which is what guards a menu item being wired to the wrong setting; it locates the
  items by caption so they survive reordering.
* **Clamping.** Extreme scales (several octaves of retune) stay in range and do
  not trip the octave fold.
* `tests/capi_doc.cpp` gains accessors for the new per-voice fields.

## Documentation and licensing

* `docs/modules/doc.md` — the resampler section (line 31, +0x2068) and the
  smoothing table (+0x2080/+0x2088) gain the per-voice override.
* New `docs/modules/tuning.md` — the snapshot, the two modes, the anchor, the
  measured numbers in this document.
* `docs/modules/gui_logic.md` — the regrouped OPTIONS tree, the index-at-push-time
  convention, and the note that OPTIONS is no longer compared against the oracle,
  so that is discoverable by anyone who later wonders why.
* `README.md` — two bullets under "Differences from the original": MTS-ESP
  support, and the regrouped OPTIONS menu (the existing list already documents
  the added OPTIONS items, so the regrouping belongs beside them).
* `THIRD_PARTY_NOTICES.md` — an entry for the MTS-ESP client.

**Licence: settled.** The MTS-ESP client is **0BSD**, which is permissive enough
to vendor without conditions (it does not even require attribution). An entry
goes in `THIRD_PARTY_NOTICES.md` regardless, since that file lists every
component; the 0BSD text is short.

## Risks

| Risk | Mitigation |
|---|---|
| Per-voice `Doc` state breaks differential tests | The state is a separate `Doc` member, outside the mapped image — `DocVoice` is untouched, so the whole-struct voice `memcpy` in `capi_doc` cannot see it. (The earlier `reserved1b0` plan would have broken exactly this; §5 records why.) |
| Float drift on the bit-exact path | `ratio == 1.0` copies the nominal floats instead of recomputing them. |
| Absolute mode's anchor is ambiguous across oscillators | Anchored explicitly on osc 0's root pitch and documented; relative mode needs no anchor at all. |
| 44.1 kHz has the least headroom (+2.37 semitones) | `N`-folding keeps the residual at or below 0.5 semitone, so the limit is never approached in practice. |
| Sampled/one-shot waves have no single cycle rate | Absolute mode is approximate there; noted in the docs. Relative mode unaffected. |
| PR #26 overlaps the GUI half | Engine work (§1–§6) proceeds against `main` now; GUI work (§7–§8) is built on top of #26. The clash table is in *Dependencies*. |
| Dropping OPTIONS from the oracle comparison loses an assertion | Deliberate: GUI differential parity is not a project goal. The removal is narrow — every other popup still compares against the oracle. |
| A menu item silently wired to the wrong setting | Indices recorded at push time rather than hard-coded, plus the one wiring assertion in *Testing*. |
| Emphasis style chosen on paper rather than on screen | Left open in §7b; the `Label::embolden` plumbing is the last step and is dropped if italic or plain wins. If bold is chosen, #26's grid fitting halves it at 100%, so the value is picked by eye at 100% and checked at 300%. |
| ~~MTS-ESP licence unverified~~ | Resolved: 0BSD, vendoring is unconditional. |
