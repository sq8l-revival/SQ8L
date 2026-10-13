# GUI layer 3: editor logic

Port of the original editor's *behaviour* (unit plugEdit = TplugEditForm, unit lcdControl =
ClcdCtr, the page set up FUN_0047e340, the value formatters of unit editBuffer, the jumping
mouse, the dialogs) on top of the ported controls of layers 1-2 (`docs/modules/gui_controls.md`).
Layer numbers refer to `docs/GUI_ARCHITECTURE.md`. Ported against the original running in the
emulator.

| File | What |
|---|---|
| `src/gui/logic/EditorController.{h,cpp}` | TplugEditForm: FormShow set up, event handlers of all controls, messages / timer, menus (FILE / INFO / OPTIONS, page popup, program popup), WRITE / INIT / compare, PANIC, SYNC / AM / MONO, hints, voices counter, OPTIONS settings |
| `src/gui/logic/EditorFileOps.cpp` | FILE menu (library / bank load-save-init, SysEx program / bank import-export), MIDI port selection, SEND / REQ |
| `src/gui/logic/LcdControl.{h,cpp}` | ClcdCtr (page controller), pageML (sub-pages), pageSL (definition string parser), ClcdCtr_param (a VFD field) |
| `src/gui/logic/LcdData.h`, `data/GuiLogicData.cpp` | the 18 page definitions + per sub-page / per parameter set up, text tables, VFD character maps (generated) |
| `src/gui/logic/Formatters.{h,cpp}` | value formatters (editBuffer 0x461114..0x461c1c, ClcdCtr 0x458ff8..0x45907c), popup texts, Delphi helpers (IntToStr, Str(v:w), zero padding) |
| `src/gui/logic/MouseJump.h` | CmouseJump (save / restore the cursor around menus and knob drags) |
| `src/gui/logic/Dialogs.{h,cpp}` | TSelSingleForm (WRITE / bank select), TmidiSelForm, TModInfoForm text (FUN_0047cb80), TAboutForm text |
| `src/gui/logic/PlatformUi.h` | what the platform supplies: popup menus, message boxes, file dialogs + files, modal dialogs, info windows, cursor, focus |
| `src/gui/logic/EditorHost.h` | what the engine supplies: edit buffer, library, settings, panic, voices, MIDI ports |
| `src/gui/logic/ViewState.h` | where the logic's output lands in EditorView (table) |
| `re/scripts/extract_gui_logic_data.py` | generates `data/GuiLogicData.cpp` from the original (see below) |
| `tests/capi_gui_logic.cpp`, `tests/test_gui_extensions.py` | C API and the port's own tests |

## Using it

```cpp
EditorView view;                       // layer 2 (controls, mouse routing, rendering)
MyHost host;                           // : EditorHost  (engine side)
MyUi ui;                               // : PlatformUi  (platform side)
EditorController ctl(view, host, ui, firstInstance);
ctl.show();                            // FormShow: wires all control events, builds the pages
// input: mouse messages go to the view (view.mouseMove / mouseDown / mouseUp /
//        mouseDoubleClick in form coordinates), the view calls the handlers installed by
//        the controller; plus
ctl.contextMenu(x, y);                 // WM_CONTEXTMENU (after a right click)
ctl.nameEditFocus(focused, text);      // program name edit focus in / out (text committed on out)
ctl.nameEditKeyDown(key);              // its OnKeyDown (VK_RETURN ends editing)
ctl.sysexReceived(data, size);         // SysEx program on the MIDI input after REQ / receive
ctl.idle(ms);                          // per 20 ms: timerTick() + pump()  (or call them yourself)
// engine notifications (host side), like the original's PostMessage(WM_APP+3):
ctl.post(kMsgNotify, 0x10002);         // a program was loaded into the edit buffer
ctl.post(kMsgNotify, 0, index);        // a setting changed (gui 0..5, synth 0x10 + 0..4)
```

Output: the controls of `EditorView` (VFD cells, program number display, knob ranges / values,
scroll arrows, LEDs, program name, status bar, voices) - see `ViewState.h` - and calls of
`PlatformUi`. Everything runs on the UI thread: the platform must hand `sysexReceived` over from
its MIDI thread (the original calls it from the winmm callback and only posts a message).

### Message queue and time

The original is driven by its window messages; the controller keeps the same queue
(`post` / `pump`): WM_COMMAND of a chosen popup item (dispatched on the next pump, like the VCL),
WM_APP (SysEx received), WM_APP+1 (refresh), WM_APP+2 (20 ms timer tick: VFD message timeout,
every 3rd tick the voices counter), WM_APP+3 (engine notifications). `idle(ms)` = what the
oracle's GuiState.idle does: per 20 ms one timer tick and a pump.

### PlatformUi contract

* `popupMenu(items, x, y)` - TPopupMenu.Popup at form point (x, y): items in the VCL's shape
  (caption with the '&' hotkeys the VCL inserts for the DFM menus, checked, radio, disabled,
  separator, bar break, sub-menus). Return the chosen item's `id`, 0 if dismissed; the logic
  dispatches it on the next pump. Separators, disabled items and sub-menu parents can't be chosen.
* `messageBox(text, caption, flags)` - Application.MessageBox (MB_OK / MB_OKCANCEL / MB_YESNO),
  returns IDOK / IDCANCEL / IDYES / IDNO.
* `fileDialog(request, path)` - TOpenDialog / TSaveDialog.Execute: InitialDir (the plugin
  folder without its trailing backslash), filter in VCL form ("text|*.ext"), default extension,
  FileName (the dialog keeps the last chosen file; the program export proposes
  folder\sanitized-name). `readFile` / `writeFile`: whole files.
* `runModal(dialog)` - show a `ModalDialog` (Dialogs.h: caption, button captions, list items,
  ItemIndex, COMPARE check box) and forward the user's actions to it (`clickItem`,
  `dblClickItem`, `keyDown`, `clickOk`, `clickCancel`, `SelSingleDialog::clickCompare /
  clickBank`) until `modalResult != 0`; closing the window = `clickCancel()`. Between actions
  call `EditorController::pump()` (the original's modal loop dispatches the editor's messages;
  the COMPARE function reloads programs while the dialog is open). Dialogs are centred on form
  point (315, 214).
* `showModInfo(lines)`, `showAbout(text)` - TModInfoForm / TAboutForm (modal in the original,
  closed by a click or a key; nothing returns to the editor).
* `cursorPos()` / `setCursorPos(p)` - the "jumping mouse" (form coordinates).
* `setCursorVisible(v)` - (port) hide the cursor while a knob is turned (issue #25); the
  default does nothing, so a platform without it simply keeps the cursor.
* `focusForm()` - focus moves from the name edit to the form (the platform then reports the
  edit's focus loss with `nameEditFocus(false, text)`).

### EditorHost (engine interfaces used)

`EditBuffer` (current program, parameters through `param` / `setParamValue`, `selectProgram`,
`writeProgram`, `compare` / `compareOff`, `initProgram`, `setName`, `importSysex` /
`exportSysex`, `libraryIndex`, bank / program number, modified flag), `SoundLibrary` (program
names, load / save / init library and banks, SysEx bank import / export, write protection),
`Settings` (restore mouse after menu / knob, compare on write, swap up/down, key capture mode,
right click scrolls display, synth settings for the OPTIONS radio items), `setGuiSetting` /
`setSynthSetting` (the host posts the WM_APP+3 notification when a value changed), `panic`,
`voicesUsed` / `voicesMax`, `pluginDirectory`, MIDI ports (`midiInPorts` / `midiOutPorts`,
`midiOpenIn` / `midiOpenOut`, `midiSendOut`, `midiCloseAll`), `processMessagesFor(ms)`.
No engine change was needed.

## Pages (lcdSetup, FUN_0047e340)

`re/scripts/extract_gui_logic_data.py` runs the original's set up in the emulator, records the 18
page definition strings it passes to ClcdCtr_pageML.setText (trace at 0x45a2dc) and dumps the
resulting objects: per sub-page title, menu group, parameter base, popup indent / hidden flags;
per parameter hint, edit buffer index, formatter / popup text / change callback, popup layout.
The port parses the same strings with the ported parser (pageML splits "\&" pairs into
sub-pages, pageSL parses `[label{knob,name,width,min,max}]` fields, digits outside brackets skip
columns) and applies the recorded set up. Result: 18 pages, 33 sub-pages, 364 parameters,
identical to the original's objects field by field (tested).

## Original behaviour reproduced (selection)

* VCL menus: automatic '&' hotkeys only on the DFM menus' first level (the first unused letter or
  digit), radio items with GroupIndex (program popup: bank items in group 1, programs in group 0),
  checks set before Add don't turn siblings off, bar breaks every 32 programs, separators "".
* OPTIONS -> DCA4 smoothing: the original checks the wrong item (value 1 = "HARD", tag 34).
* Value popups: ids = value + 0x10000; signed parameters start with an extra "0" item and a
  separator; empty disabled bar-break items fill later columns.
* A parameter of only two values is a soft button, not a knob (port, issue #43): the knob slot
  shows `lcdToggle<i>` instead and a single click flips the value — on the button, or on the
  parameter's field in the display, which no longer starts a drag (and so no longer hides the
  cursor either). `lcdKnobRange` decides from the range it is handed; the knob behind the
  button keeps the value, so a `knobChanged` is what the flip finally goes through.
  FUN_0045b2b0 already flips a range of two rather than showing a two-item menu of it; with
  the single click doing that, the double click must not, or clicking twice and double
  clicking would end the other way round from each other.
* Value popups are opened by a double click on the VFD cell or on the knob, and (port) by
  a right click on the knob: `contextMenu` routes a point inside a knob with a parameter
  to `cellDoubleClick`, so both gestures land in the same place. A knob the page does not
  use falls through to the page popup, like the background; so does a two-state slot, whose
  knob is hidden under its soft button and which `contextMenu` therefore passes over.
* Import program: "try to load anyway" retries with the same header check (so it fails again).
* Mouse jump: the first saved position is kept until restored. Popup menus are all it is
  used for now. A knob turn hides the cursor and locks the pointer instead (port, issue
  #25): `EditorController::hideCursor` calls `EditorView::lockPointer` at the point the
  turn started, and unlocking parks the cursor back there. While it is locked the view
  puts the cursor back after every move and hands the controls the distance travelled, so
  the turn is not limited by the screen; `EditorView::warpCursor` reports where the cursor
  really ended up, so a platform that refuses to move it (XWayland) still tracks right.
* MIDI port dialog without ports: ItemIndex -1, OK returns -1 (ModalResult 8).
* Dialog InitialDir: TOpenDialog.SetInitialDir drops the trailing backslash.
* The mouse wheel (port, issue #35; the original ignores WM_MOUSEWHEEL): `wheel(x, y, delta,
  keys)` turns the knob under the point, or the one behind the display field under it. A
  two-state field is a button and ignores it (issue #43), as does a slot the page has no
  parameter for. One notch is one step, or `kWheelCtrlStep` with Ctrl held; Shift means
  nothing, a notch being already the smallest move there is. OPTIONS -> Mouse -> Natural
  scrolling ([port] naturalScroll) inverts it for a touchpad.
  **What a notch is, is learned, not assumed**: platforms report wildly different magnitudes,
  so the smallest delta ever seen is taken for one notch and a notch fires once 0.75 of it has
  accumulated. A device sending a whole detent at once and one trickling small values
  therefore both step once per notch, and a delta several notches long steps several times.
  The accumulator belongs to one knob at a time and is dropped when the pointer reaches
  another, so a part notch cannot leak across controls. The platform must pass the delta
  **unrounded** (`SQ8LUI::onScroll`), or there is nothing to calibrate from.

## Key capture (CkeyCapt)

The original hooks the keyboard of the editor's thread (SetWindowsHookEx WH_KEYBOARD and
WH_CALLWNDPROC, unit 0x478d14) while the program name edit has the focus and while the WRITE /
select / MIDI dialogs are open, so that the host doesn't take the keys; the key capture mode
setting (-1 = off) is passed along. This is platform work: the controller exposes
`keyboardCaptured()` (true in those periods when the mode is >= 0) and `keyCaptMode()`; the
platform grabs the keyboard accordingly.

## Verification

`tests/test_gui_extensions.py` (through `tests/capi_gui_logic.cpp`), in CI. The OPTIONS
layout is not asserted there: the test locates the items it clicks by caption, and the
grouping is left to inspection.

## OPTIONS layout (port)

```
Polyphony                           >   per instance, 0 = set by the program (EMU -> VOICES)
Voice stealing mode                 >
Emulation preferences               >
------------------------------------
Mouse                               >   restore after popup menus / knob turning, - , display scroll
Down arrow loads next program
------------------------------------
Ask before loading banks/libraries
------------------------------------
MTS-ESP                             >   Enable, - , Correct SQ-80 per-key pitch offsets
------------------------------------
Zoom                                >
```

Polyphony leads: how many voices there are, then what happens when they run out, then the rest
of the emulation. The original's "Mouse position is restored after..." submenu and its separate
"Right click on display -> scroll page" item are one `Mouse` submenu, with the children reworded
because the originals only read correctly under the old parent's phrasing. `MTS-ESP` carries the
checkmark of its own `Enable` child, so the state shows without opening the submenu. Everything
except Voice stealing mode, Emulation preferences and Mouse is a port addition.

**Indices are recorded as the items go in.** `buildMenus` keeps a `size_t` per cached
`MenuNode*` and resolves the pointers once the vector is final, because hard-coded positions
silently rewired the wrong setting whenever the layout changed.

## Changes outside src/gui/logic

* `CMakeLists.txt`: `add_subdirectory(src/gui/logic)`, `sq8l_gui_logic` linked into
  `sq8l_testapi`.
* `oracle/gui.py` (additive): `menu_tree` nodes also carry `radio`, `default` and `break`.
* `oracle/winapi.py`: VirtualAlloc with MEM_RESERVE at a given address fails (the Delphi memory
  manager tries to grow its region in place there; accepting it overlapped the emulator's heap
  and crashed the oracle after many popup menus).

## Notes / open points

* The LEDs (TAniDisplay) fit their size to the GIF frame (14 x 14) when first painted; before
  that they are 10 x 10. Mouse hit tests depend on it, so the test paints both sides once
  after opening (the oracle had never painted them).
* Not verified against the original: the platform side of key capture, the look of the dialogs,
  word wrap in the modulation usage window, MIDI timing (`processMessagesFor`).
