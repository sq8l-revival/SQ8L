# Keyboard control of the editor (issue #45)

Design document. Status: **not implemented**. Written against `main` at `56a1bfc`.

## Intent

Let the editor be driven from the keyboard: the parameter under focus edited with the arrow
keys, the pages reached with Alt chords, the programs and banks stepped with Ctrl chords, and
the focus itself moved with Tab. Off by default, and when off the editor behaves exactly as it
does today — no key is consumed, no focus is taken from the host, nothing new is drawn.

Success criteria:

* Every key in §2 does what the issue says it does, verified through the C API tests.
* Which knob has focus is visible at a glance, in the classic frame and in HD, at any zoom.
* A key the editor has no use for reaches the host, so the host's own shortcuts (space above
  all) keep working while the editor window holds the keyboard.
* A project reopened in the DAW comes back on the page and parameter it was left on.
* With the feature off, the host chunk stays byte for byte what it is today.

## Scope

In: the seven value keys of the issue body, Tab traversal, the Alt and Ctrl chords of the issue
comment, the focus rectangle, an OPTIONS → Keyboard submenu with two switches, the per-instance
page memory, and — riding along because it shares the default-value definition — the knob's
double click setting the default instead of opening the value menu (§2a).

Out: editing the program name from the keyboard (it is a platform text box and already has its
keys), keyboard operation of the menus and dialogs (the drawn ones already have it, the native
ones get it from the OS), and any shortcut for FILE/INFO/OPTIONS, WRITE, INIT or PANIC — a
destructive action behind a single keystroke in a window that may have taken focus by accident
is a trap, and the issue does not ask for one.

## What already exists

The reason the editor half of this is small:

| Need | Already there |
|---|---|
| move a parameter by N, clamped, through the same route a mouse turn takes | `EditorController::stepKnobParam` (`EditorController.cpp:1161`), written for the wheel, issue #35 |
| a selected parameter, drawn as the VFD underline | `LcdParam::highlight`, `EditorController::highlightParam` (`:791`) — set by a field click, a turn, a wheel notch, a toggle flip; cleared when a page is built |
| which knob slots a page actually uses | `LcdSubPage::paramForKnob` / `knobParam`, and the faded-slot test in `knobAt` (`:1110`) |
| select a page, a sub-page, a view | `pageButton(tag, cycle)` (`:864`), `LcdController::nextSubPage`, `selectPage`, `selectPageSub` |
| step bank and program | `EditorController::selectBank` / `selectProgram`, what the UP/DOWN buttons already call |
| a per-value default | `makeInitProgram()` + the free `getParam(const Program&, index)` |
| a `[port]` ini setting with an OPTIONS checkbox | `naturalScroll`, issue #35 — `Settings::kPortKeys`, `setPortSetting`, notification `0x20 + index` |
| a per-instance switch with an OPTIONS checkbox | `Support MIDI Program Change`, issue #40 — an `EditorHost` accessor pair and a flag bit in the chunk |
| an overlay drawn over the finished frame | `EditorView::drawMtsLabel` |
| a keyboard channel into the UI, VK-mapped | `SQ8LUI::onKeyboard` (`src/plugin/SQ8LUI.cpp:624`), today used only by the drawn menus and by REAPER's stolen Return |
| per-instance state in the host chunk | `Synth::getChunk` / `setChunk`, spare header bytes |

Nothing in the engine's audio path is touched by any of this.

## Design

### 1. Focus model

**Focus is the selected parameter.** Not a second piece of state beside it: `LcdParam::highlight`
*is* the focus, and the rectangle of §4 is drawn on the knob slot of the highlighted parameter.
The consequences are deliberate:

* Everything that already moves the selection — a click on a knob, a turn, a wheel notch, a
  toggle flip, a click on the parameter's VFD field — moves the focus, and Tab moves both.
* The underline and the rectangle can never disagree, because they are one fact rendered twice.
* While the feature is on, a page change lands focus somewhere (§6), so the VFD always shows one
  parameter underlined. The original only underlined after a click. This is a visible departure
  and an accepted one: GUI differential parity is not a goal, and a focus that is sometimes
  nowhere makes the arrow keys do nothing for no visible reason.

One addition to what sets it: a knob **mouse-down** does not currently select anything (only a
value change does, through `lcdKnobChange`). The issue asks for "each clicked knob should take
keyboard focus", so `wireControls` gains an `onMouseDown` on each knob that calls
`highlightParam(sp->knobParam(i))`. A click on a faded slot selects nothing, as `knobAt` already
decides for the wheel.

Tab and Shift+Tab walk the slots 0..9 that the current sub-page gives a parameter, wrapping, and
skip a slot the page does not use. A two-state slot (the soft button of issue #43) is included:
it has a parameter, and the value keys work on it.

### 2. Key map

`EditorController::keyDown(int vk, uint32_t mods) -> bool`, a new entry point beside `wheel()`,
returning whether the editor used the key. `mods` is a new small enum (`kModShift`, `kModCtrl`,
`kModAlt`) rather than the view's `MK_*` flags, which have no Alt bit.

| Key | Action |
|---|---|
| Up / Down | `stepKnobParam(focus, +1 / -1)` |
| PgUp / PgDn | `stepKnobParam(focus, +10 / -10)` |
| Home / End | the parameter's `min` / `max` |
| Del | `getParam(makeInitProgram(), p->index)` — what INIT would give this parameter |
| Tab / Shift+Tab | next / previous usable slot |
| Alt+1 / 2 / 3 | `pageButton(16 / 32 / 48, true)` — OSC1 / OSC2 / OSC3 |
| Alt+F | `pageButton(64, true)` — FILTER |
| Alt+A | `pageButton(80, true)` — DCA4 |
| Alt+L | the LFO group: pages of `buttLfo1..4` (111, 127, 143, 159) |
| Alt+E | the ENV group: `buttEnv1..4` (175, 191, 207, 223) |
| Alt+M | the MAT group: `buttMat1..3` (224, 240, 256) |
| Alt+X | `pageButton(15, true)` — OSC1-3 Mix |
| Alt+U | `pageButton(287, true)` — MODES |
| Alt+V | `nextSubPage()` — the views of the current page |
| Ctrl+Up / Down | previous / next bank |
| Ctrl+Left / Right | previous / next program in the bank |

Notes:

* The button tags are `page << 4 | sub`, with `sub == 0xf` meaning "any", which is what
  `pageButton` already reads. The tags above are the ones `wireControls` installs
  (`EditorController.cpp:469`), and `cycle = true` is what a button click passes, so **a chord
  is exactly its button**: Alt+1 from elsewhere goes to OSC1, Alt+1 again cycles the page's
  views to DCA1, Alt+1 from DCA1 comes back to OSC1 — the same sequence as clicking `buttOsc1`
  repeatedly. Likewise Alt+X and Alt+U cycle the views of their pages, as their buttons do.
* The three group chords are the exception, because no single button is their equivalent: each
  advances to the next page of its group (LFO1→2→3→4→1), rather than cycling the current page's
  views. Alt+V remains the way to reach the views of a group page, so Alt+L and Alt+V are two
  independent axes once you are inside the LFO pages.
* Page memory does not override a chord that names its own sub-page; see §6.
* `stepKnobParam` is refactored into `setKnobParamValue(knob, v)` plus a step wrapper, so Home,
  End and Del take the same route (set the knob, then `knobChanged`) and the undo/modified
  bookkeeping cannot drift between the two.
* 10 for PgUp/PgDn is the existing `kWheelCtrlStep`, so a page key and a Ctrl+notch move the
  same distance. The constant is shared rather than repeated.
* Ctrl+arrows ignore OPTIONS → "Down arrow loads next program" (`swapProgUpDn`). That setting
  exists because the on-screen UP/DOWN buttons are ambiguous about which way is "next"; an arrow
  key is not, and silently inverting it would be surprising.
* Anything not in the table returns false and is not consumed.

### 2a. Double click on a knob sets the default

Now that a parameter has a default (the Del key's definition above), the knob's double click is
worth more spent on it than on a menu that is already two gestures away. So `k.onDblClick`
(`EditorController.cpp:364`) stops calling `ctr_->cellDoubleClick(p->x, p->y)` and sets the
INIT value instead, through the same `setKnobParamValue` the keys use.

The value menu loses nothing: a **right** click on the knob opens it (the port already routes a
point inside a knob to `cellDoubleClick` from `contextMenu`), and so does a double click on the
parameter's **field in the display**, which is untouched. The asymmetry between the knob and its
display field is deliberate — the knob is where a reset is wanted and the field is where a list
of values is — but it is an asymmetry, and worth a second look once it has been used.

Two details:

* A two-state slot is unaffected. Its knob is hidden behind the soft button of issue #43, so
  there is no knob to double click, and a double click on the button is two flips that land back
  where they started.
* `Knob::DblClick` is `cancelDrag`, then `OnDblClick`, then `MouseDown(ssDouble)`, which begins
  a fresh drag. The order works in our favour: the default is in place before `beginDrag` reads
  the value, so holding and moving after a double click drags on from the default.

This is not gated by `Enable keyboard control` — it is a mouse gesture, and it makes sense
whether or not the keys are in use. It is also, strictly, not what issue #45 asks for; it is
recorded here because it shares the default-value definition, and it should be noted on the
issue so the change is not a surprise in the PR.

### 3. Focus acquisition and pass-through

This is the part that is not cheap, and the part that cannot be settled by reading code.

**How keys reach us.** Three paths, and they do not behave alike:

* **VST3** — `IPlugView::onKeyDown`, implemented by DPF (`DistrhoUIVST3.cpp:262`). The return
  value tells the host whether we used the key, so returning false *is* the pass-through: the
  host goes on to handle it itself. Free and correct.
* **VST2** — `effEditKeyDown`, same shape (`DistrhoPluginVST2.cpp:275`). Same conclusion.
* **CLAP** — DPF implements no keyboard extension at all. The only way a key arrives is the
  plugin's own native window holding real keyboard focus, and then pugl swallows it: every
  handled message in `puglWinWndProc` returns without reaching `DefWindowProc`
  (`pugl-upstream/src/win.c:909`), so an unconsumed key dies in our window instead of reaching
  the host.

**Measured on Windows** (probe, §8) — REAPER, FL Studio and Live 12:

| | keys arrive | unconsumed key reaches the host |
|---|---|---|
| REAPER, VST2 / VST3 | yes, after a click in the editor | yes — space transported, Tab moved the chrome focus |
| REAPER, CLAP, stage A (no grab) | no | — |
| REAPER, CLAP, stage B (grab on click) | yes | yes for what REAPER kept; no for what it hands over |
| FL Studio, stage B | yes, immediately, no click needed | yes |
| Live 12, stage B | yes | yes |

Two host behaviours worth designing around, both from FL:

* **FL plays its typing keyboard at the same time** as it sends us the keys, until a click lands
  inside our window, after which the virtual keyboard goes quiet. So the click is not what makes
  keys arrive there — it is what tells FL the plugin owns the keyboard. The focus grab decided
  below should put us in that state from the start, which would stop the double action without a
  click; worth verifying in FL once it is built, since it is the one host where the grab earns
  something beyond the keys themselves.
* **FL delivers Tab as a release only** — the press never arrives, presumably eaten by FL's own
  focus navigation. A handler that acts on presses would never see Tab there. §7 takes it.

Space was the key that looked anomalous, and it is REAPER rather than us: the host claims the
keys bound to its own global actions before dispatching to the focused child window, which is
what its per-FX **"Send all keyboard input to plug-in"** switch turns off. With the switch in
its default position space never reaches the plugin at all — confirmed by turning it on, after
which space does arrive. Nothing in pugl singles space out; its only key filter drops auto
repeats (`win.c:636`).

So the two halves of the problem are confirmed, they are *separate*, and the second is smaller
than it first looked:

* **VST2 and VST3 need no platform code at all.** Keys arrive through the host channel and
  "not handled" is its return value, so the host keeps every key we do not take. The only key
  REAPER loses is one we deliberately consume — Tab, which is the point of it.
* **CLAP needs the focus grab, and forwarding only for the keys REAPER hands over.** By default
  the host keeps its own shortcuts regardless of us, so the forwarding matters to the user who
  has turned "Send all keyboard input to plug-in" on — which is, admittedly, exactly the user
  who wants this feature, and for whom space would otherwise go dead. It is worth building, but
  it is not a blocker and its absence costs only that configuration.

So on the native path two things are needed that the host channels give for free.

**Taking focus.** Nothing gives our child window the keyboard until something asks for it, and
the host decides where to send its key channel from the platform's focus — so in REAPER no key
arrives, in any format, until a click inside the editor has happened. That is the annoyance to
remove.

**Decided: grab once when the editor window appears, and never again on our own initiative.**
Only while `Enable keyboard control` is on. Opening the plugin and typing then works with no
click at all; after the user touches anything else in the host's chrome around our window, the
keys belong to the chrome until they click back into the editor, which is the behaviour the
probe already shows. The grab belongs on the first idle tick rather than in the constructor,
where the window is not yet attached to the host's — `askHostForSize` makes the same
distinction for the same reason.

The alternative considered and rejected was to grab again whenever the pointer enters our window
(with the foreground-window check needed to stop a mouse merely passing over a floating window
from activating it). It needs no click ever, but it means the editor and the host's own chrome
trade the keyboard as the mouse moves, and in REAPER that chrome is inches away. Predictability
won.

`Window::focus()` is the mechanism (`puglGrabFocus`); `PlatformUi::focusForm()` already does
`SetFocus(impl_->hwnd)` on Windows (`PlatformUiWin.cpp:312`), so the editor logic has an
interface for it if the grab ever needs to come from there rather than from the UI.

**Giving keys back.** For a key we do not consume, on the native path only:

* Windows: re-post it to the host — `PostMessage(GetAncestor(hwnd, GA_ROOT), WM_KEYDOWN, vk, …)`.
  DPF's `KeyboardEvent` carries `ev.keycode`, so the message can be rebuilt. Which window to
  post to is not obvious and is worth a few minutes of probing when it is built: the root
  ancestor, the immediate parent and the foreground window are three different windows in a
  floating FX window, and REAPER's accelerator table does not necessarily watch all three.
  Space in CLAP is the test case, since it is already known to be the key that goes missing.
* macOS: the key belongs to the next responder. Reaching that from DPF's event callback is not
  obviously possible without touching `pugl_mac.m`.
* Linux: `XSendEvent` to the parent of the embedded window.

Windows is the one I am confident about. macOS and Linux are **best effort, and may need a DPF
patch we would then carry**. This is the main risk of the whole feature (see Risks), and the
reason the switch is off by default: with it off we never take focus and never consume anything,
so no host can be made worse by the feature existing.

**The probe settles it before any of this is written.** See §8.

### 4. The focus indicator

A 1 px rectangle immediately outside the focused widget's bounds, in the VFD's colour
(`#1dfdc2`): around the knob's 40×40 slot, or around the 44×22 soft button when the slot shows
one. Slots are 96 px apart and 40 px wide, so there is room on every side and nothing collides.

Two renderers must agree:

* **Classic** — drawn over the finished frame in `EditorView::render`, after the controls, the
  way `drawMtsLabel` already is. A control's own surface cannot carry it: the rectangle is
  outside the control's bounds, and the surfaces are composed without overlap.
* **HD** — a matching item in `HdRenderer::capture`, taking its colour from the VFD font's own
  derived lit colour rather than a second hardcoded constant, so display and rectangle match at
  any scale. HD's repaint is driven by `changedTiles` diffing the classic frame, so if only one
  of the two drew the rectangle, moving focus would either not repaint or repaint wrongly.

`EditorView` grows one piece of state for it (`focusKnob`, -1 = none), set by the controller.
The rectangle is drawn only while `Enable keyboard control` is on.

### 5. Settings and the OPTIONS menu

A `Keyboard` submenu below `Mouse`:

```
Mouse                               >
Keyboard                            >   Enable keyboard control
                                        Page memory                  per instance
Down arrow loads next program
------------------------------------
...
```

The two switches have different scope, and the submenu holds them anyway — the menu already
mixes them elsewhere (Output gain and Polyphony are per instance, sitting among global items),
and these two belong together for the reader even though they are saved in different places.

**Enable keyboard control** is a preference of the user: one new `[port]` ini key, appended so
that no existing index moves (`kPortOutputCut` stays 2), `Settings::kNumPort` 3 → 4.

| key | default | meaning |
|---|---|---|
| `keyboardControl` | 0 | the feature: keys, focus rectangle, focus grab |

Wiring is the `naturalScroll` pattern: `menuKeyboardControl_` cached as an index in
`buildMenus`, a click handler calling `host_.setPortSetting`, and `settingsChanged()` reading it
back when the host notifies `0x20 + index`.

**Page memory** is per instance, and travels in the plugin's own state — §6. It is wired like
`Support MIDI Program Change` (issue #40) instead: a `MenuNode` whose handler calls a new
`EditorHost::setPageMemory`, read back from `host_.pageMemory()`. Two instances in a project can
differ, and a project recalls each one's choice, which a global ini key could not do.

`Page memory` is disabled, not hidden, while `Enable keyboard control` is off — the submenu
should show what it holds.

### 6. Page memory, and where it lives

**Page memory off.** A page change focuses the first slot the sub-page uses. Alt+L / Alt+E /
Alt+M always enter their group at its first page (LFO1, ENV1, MAT1) and cycle forward from
there while you stay in the group.

**Page memory on.** A sub-page you have not visited focuses its first usable slot; one you have
focuses the parameter you last had focused there. Alt+L / Alt+E / Alt+M return to the page of
that group you were last on, and cycle only once you are already inside it. A chord that names
its own sub-page still wins: Alt+1 is OSC1, never DCA1.

**Where the state lives.** In the engine (`Synth`), not in the controller, so that closing and
reopening the editor window does not forget it — which in a DAW is a common gesture. The editor
reaches it through two new `EditorHost` accessors, in the family of `polyphonyOverride` and
`mtsEnabled`:

```cpp
struct EditorPlace {        // opaque to the engine
    int8_t page = -1, sub = -1, knob = -1;   // where the editor is
    int8_t group[3] = {-1, -1, -1};          // last LFO / ENV / MAT page
    int8_t focusBySub[72];                   // the focused slot per page*4 + sub, all -1
};
virtual const EditorPlace& editorPlace();
virtual void setEditorPlace(const EditorPlace&);
virtual bool pageMemory();           // the switch itself, also per instance
virtual void setPageMemory(bool);
```

`Synth` stores it and never interprets it. The controller writes it whenever the page, sub-page
or focus changes, and reads it in `show()` to restore (after `ctr_->selectPage(0)`, replacing
that choice when a saved place is present). The flat index reserves four slots per page, which
is one more than any page needs: the widest of the 18 page definitions parses to three
sub-pages.

**What of it the chunk carries.** The chunk is a fixed 0x23b bytes and `setChunk` accepts no
other size, so there is no room for the 72-byte table. There is room for the summary: bytes
`0x0e..0x17` are ten bytes the original writes as zero and never validates — `library.md`
documents them as "then zeros", and `EditBuffer::setChunk` checks only the type, `0x01..0x03`
and the magic. They are the same kind of space `0x1b..0x1e` already uses for polyphony, the
switch bits and the output cut.

| offset | content |
|---|---|
| 0x0e | marker `'K'`, written only when there is a place to save |
| 0x0f | page |
| 0x10 | sub-page |
| 0x11 | focused slot, 0xff = none |
| 0x12..0x14 | last LFO / ENV / MAT page |
| 0x15..0x17 | reserved, 0 |

The **switch** rides in the flag byte `0x1d`, whose bits 5-7 are still free, as bit 5 — and
**inverted: the bit set means page memory off**. The inversion is the point and must not be
"tidied up": an absent bit has to read as *on*, because that is what both a new instance and a
project saved before this existed should do, and because leaving the switch at its default then
writes nothing. This is the opposite choice from `kChunkProgramChange` (issue #40), which wanted
an old project to read as off; the two sit in the same byte, so the comment there has to say
why they differ.

Written in `Synth::getChunk` beside the existing additions (per `sq8l-chunk-additions-in-synth`:
the differential tests compare `EditBuffer` directly, so a port addition must not go in
`EditBuffer`). The place bytes go in only when page memory is on and a place exists. So a chunk
from an instance that never turned keyboard control on is byte for byte the original's, and a
chunk without the marker puts the instance back to "no saved place", which is what every old
project and the original produce.

The per-sub-page table is deliberately **not** persisted: it does not fit, and it is not what
the request is about. On load the table is seeded with the one saved entry, so the project opens
on the page, view and parameter it was left on; the rest of the table fills in again as you
browse.

### 7. Platform plumbing

* The `kVk*` enum moves from `src/gui/drawn/PlatformUiDrawn.h` into `src/gui/logic/PlatformUi.h`
  (which `PlatformUiDrawn.h` already includes, so nothing else moves) and gains `kVkTab` and the
  letter and digit codes the chords need. The editor logic now needs these codes, and the drawn
  layer is the wrong owner for them.
* `SQ8LUI::onKeyboard` gains the native-build branch it does not have today: translate to a VK
  and modifiers, call `controller_->keyDown` under the engine lock, and on false fall through to
  the pass-through of §3. The existing early returns stay: a drawn modal, a drawn name box, a
  waiting editor and the native name box all keep first claim on the keyboard.
* **An orphaned release counts as a press.** Because FL delivers Tab's release and not its press
  (§3), the UI keeps a note of which keys it consumed the press of; a release that arrives for a
  key in the map whose press was never seen is handed to `keyDown` as though it were the press,
  and a release whose press we did consume is dropped. Auto-repeat is unaffected — it arrives as
  presses, and the single release that follows is ignored. The quirk belongs here rather than in
  `EditorController`, which should know about keys and not about hosts.
* Alt chords on Windows arrive as `WM_SYSKEYDOWN`, which pugl does translate
  (`pugl-upstream/src/win.c:926`), so no extra work there.

### 8. The probe, before the rest

What arrives, in which format, in which host, is unknown, and the design above has a cheap
branch point depending on the answer. So the first commit is a throwaway:
`SQ8LUI::onKeyboard`, under the existing `SQ8L_UI_DEBUG`, writes each event into the MTS-ESP
scale label (`EditorView::setMtsText`) instead of into stderr, which a plugin in a host loses.
This works without any suppression of the scale poll: `updateMtsScale` only pushes when the name
differs from its cache, and with MTS-ESP off both are empty, so it never writes the label at all.
309 px of italic MS Sans at the top of the window is enough for `dn 0x26 UP alt=0 ctrl=0`.

Two stages, both discardable:

* **A** — report only, no focus grab. Press Up, Tab, Alt+1 and Ctrl+Left in REAPER and Bitwig
  across CLAP, VST3 and VST2. Whatever shows in the label is what we get for free.
* **B** — the same with `focus()` on a knob click. The difference between A and B is exactly how
  much the focus grab buys, which is the question §3 cannot answer from here.

If a chord turns out to be universally eaten by hosts (Tab and the Alt chords are the likely
ones), that is worth knowing before it is specified into the tests.

**Outcome** (Windows; REAPER, FL Studio, Live 12; commit `ab0d9c7`): the table in §3. The probe
did its job — the focus policy of §3, the CLAP forwarding and the orphaned-release rule of §7
are all decided by measurement rather than by guesswork, and the VST2/VST3 half of the feature
turns out to need no platform code at all. The probe commit is reverted before the feature is
built; macOS and Linux can be checked by reinstating it from git history when the time comes.

## Testing

`tests/capi_gui_logic.cpp` + `tests/test_gui_extensions.py`, the way the wheel was tested:

* `sq8l_gl_key(v, vk, mods)` beside `sq8l_gl_wheel`, and `sq8l_gl_focus_knob(v)` to read the
  focused slot for assertions.
* Value keys: each of the seven on a knob slot, on a signed parameter, at both ends of the range
  (clamping, and no change at the end), on a two-state slot, and on a page whose slot is faded.
  Del against a program that differs from INIT in that parameter.
* Tab: wraps, skips unused slots, Shift+Tab goes the other way.
* Chords: each one lands on the expected page and sub-page; Alt+V cycles; the three group chords
  cycle; Ctrl+arrows move program and bank and stop at the ends the way the buttons do.
* Chords repeat like their buttons: Alt+1 twice lands on DCA1 and a third time back on OSC1,
  the same sequence `sq8l_gl_mouse` clicking `buttOsc1` three times produces — asserted against
  each other rather than against hardcoded page numbers.
* Page memory: off → first slot every time, groups always enter at their first page; on → a
  revisited sub-page restores its parameter, a group chord returns to its last page.
* The switch: with `keyboardControl` off, every key above is a no-op and `focusKnob` stays -1.
* Chunk: a round trip with page memory on restores page, sub-page and slot; the switch itself
  survives the round trip; a chunk with no flag bit loads as page memory **on**; a chunk from an
  instance that never enabled keyboard control is byte-identical to today's; a chunk without the
  `'K'` marker loads as "no place".
* Double click on a knob sets the INIT value, on a program whose value differs from INIT, and
  does **not** open the value popup; a right click on the knob and a double click on the
  parameter's display field still do open it.
* The rectangle is asserted in `sq8l_hd_check` (HD) and through a classic frame comparison — the
  pixel at a known point beside the focused knob, and its absence when the feature is off.

## Documentation

* `docs/modules/gui_logic.md` — the key map, the focus model, the OPTIONS layout block (which
  the doc keeps verbatim), page memory, and the knob double click in the "original behaviour
  reproduced" list, where the value popup's gestures are currently described.
* `docs/modules/gui_controls.md` — the focus rectangle under `EditorView`, as the MTS label and
  the faded knob are.
* `docs/modules/library.md` — the chunk header table: `0x0e..0x17` is no longer only zeros, and
  `0x1d` gains bit 5 with its inversion spelled out.
* `docs/modules/settings.md` — the one new `[port]` key.
* `README.md` — a bullet, as #35 and #52 got; the double click, which changes a gesture that
  already worked; and a line on what a host decides for itself, since the keys that reach a
  plugin are the host's choice and REAPER's "Send all keyboard input to plug-in" is the switch
  users will ask about.

## Risks

1. **Pass-through on the native path (§3).** No longer a guess on Windows: CLAP needs it, but
   only for the keys the host hands over, and a host keeps its own shortcuts either way. macOS
   and Linux remain unmeasured, and reaching the next responder from DPF's callback may need a
   patch to `pugl_mac.m` that we would then carry. If it cannot be done, the honest fallback is a
   README line: in CLAP on those platforms, a key the host gave us and we do not use is lost
   while the editor holds focus. The switch defaults to off, so no one loses anything they have
   today, and VST2/VST3 users lose nothing either way.
2. **Hosts that eat the chords.** Tab and Alt are heavily used by hosts. The probe tells us
   which survive; the ones that do not are not a bug we can fix, and should be documented
   rather than worked around.
3. **The always-underlined parameter (§1).** A visible change from the original, only while the
   feature is on. If it turns out to grate, the fallback is the third option that was considered
   and rejected: no focus until the first click or Tab, at the cost of the rectangle not always
   being on screen.
4. **Chunk space is nearly gone.** After this, three bytes (`0x15..0x17`) and two bits (6-7 of
   `0x1d`) are the whole of the free header. Anything bigger will have to grow the chunk, which
   is its own design.
5. **The inverted page-memory bit (§6).** A clear bit meaning "on" is the opposite of the
   neighbouring `kChunkProgramChange`, and reads like a mistake to anyone who finds it later.
   Mitigated by the comment and by the test that asserts a bitless chunk loads as on; if it
   still feels like a trap at review, the alternative is a default of off and a bit meaning on,
   at the cost of every old project opening with page memory off.

## Order of work

1. The probe (§8), both stages, discarded afterwards.
2. `setKnobParamValue` and the default-value helper, then the knob double click (§2a) — the
   smallest useful piece, testable on its own and independent of everything else.
3. `keyDown` + the focus model + the key map, with the tests. No platform code: the C API drives
   it, so all of §1, §2 and §6 can be finished and tested before any host is involved.
4. The focus rectangle in both renderers.
5. The OPTIONS submenu, the ini key and the per-instance switch.
6. The chunk bytes and the `EditorPlace` accessors.
7. The platform plumbing of §3 and §7, informed by the probe.
