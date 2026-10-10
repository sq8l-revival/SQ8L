# GUI layer 3: editor logic

Port of the original editor's *behaviour* (unit plugEdit = TplugEditForm, unit lcdControl =
ClcdCtr, the page set up FUN_0047e340, the value formatters of unit editBuffer, the jumping
mouse, the dialogs) on top of the ported controls of layers 1-2 (`docs/modules/gui_controls.md`).
Layer numbers refer to `docs/GUI_ARCHITECTURE.md`. Verified differentially against the original
running in the emulator (`tests/test_gui_logic.py`).

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
| `tests/capi_gui_logic.cpp`, `tests/test_gui_logic.py` | C API and differential test |

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
* Value popups are opened by a double click on the VFD cell or on the knob, and (port) by
  a right click on the knob: `contextMenu` routes a point inside a knob with a parameter
  to `cellDoubleClick`, so both gestures land in the same place. A knob the page does not
  use falls through to the page popup, like the background.
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
* WM_MOUSEWHEEL is ignored (as in the original).

## Key capture (CkeyCapt)

The original hooks the keyboard of the editor's thread (SetWindowsHookEx WH_KEYBOARD and
WH_CALLWNDPROC, unit 0x478d14) while the program name edit has the focus and while the WRITE /
select / MIDI dialogs are open, so that the host doesn't take the keys; the key capture mode
setting (-1 = off) is passed along. This is platform work: the controller exposes
`keyboardCaptured()` (true in those periods when the mode is >= 0) and `keyCaptMode()`; the
platform grabs the keyboard accordingly.

## Verification

`SQ8L_TESTAPI=$PWD/build/libsq8l_testapi.dylib .venv/bin/python tests/test_gui_logic.py [scenario ...]`

The original editor runs in the emulator (oracle/gui_driver.Editor); the port is an EditorView +
EditorController with the same engine state (library, edit buffer, settings copied from the
emulated process). Both get the same input (Win32 mouse messages in form coordinates, timer
ticks, WM_CONTEXTMENU, focus, MIDI input) and the same scripted answers (popup menu paths,
message box results, file paths, dialog actions). After every step the complete state is
compared: VFD and program display cells (character + attribute), all knob fields, button / LED
states, status texts, name edit, page controller (page, sub-page, locks, message timer, every
field's value / text / highlight), form fields, mouse jump, the edit buffer image, settings,
optionally the library, and every event: popup menus (full trees with flags and position),
message boxes, file dialogs, modal dialog contents, info windows, SetCursorPos, MIDI output.

To run the modal dialogs of the original the test patches PeekMessage / DispatchMessage (the
VCL modal loop gets the posted messages, then the messages of the scripted user actions) and
emulates the LISTBOX / multi-line EDIT / check box messages the dialogs use, the file dialogs
(answers written into OPENFILENAME, files in `oracle/sandbox/T/gl`) and winmm MIDI (port names,
open, SysEx out recorded, SysEx in through the input callback).

| Scenario | Content | Checks |
|---|---|---|
| pages | page tree field by field; every value (min-1 .. max+1) of every parameter of every sub-page through the page controller; the value popup of every parameter | 22 241 |
| basic | every page button and sub-page: 10 knob drags (vertical, horizontal fine), VFD click / double click with a popup choice / drags up and down on every field | 2 821 |
| programs/menus | up / down / bank, program popups (numLcd, name), page popup at many places, every OPTIONS item, FILE / INFO, buttons, hints over every control and VFD cell | 319 |
| dialogs | 17 WRITE / compare dialog scripts, modulation usage of 40 programs, about, program name editing | 73 |
| files | library / bank save-load-init (answers OK / cancel, junk / empty / missing files), SysEx program and bank import / export | 96 |
| midi | port dialogs (open failing / succeeding), SEND dump bytes, REQ, received dumps (complete, short, foreign, corrupt) | 38 |
| host | 30 program changes from the host | 66 |
| random | 3 seeds x 350 random actions of all kinds | 1 050 |

Result (full run, about 55 s): 26 705 checks, 0 differences. Events compared: 931 popup menus,
63 message boxes, 36 file dialogs, 109 modal dialogs, 54 modulation usage windows, 6 about
windows, 10 MIDI outputs, 2 527 cursor jumps.

Coverage (pages x parameters x actions), printed at the end of the run:

| Action | Parameters (of 364) |
|---|---|
| set through the page controller, every value | 364 |
| value popup shown | 364 |
| selected by a VFD click | 248 |
| changed by a VFD drag | 234 |
| changed by a knob drag / horizontal fine drag | 200 / 216 |
| changed by a popup choice | 117 (+21 random) |
| changed by any GUI gesture | 248 of the 248 editable ones (the other 116 are labels, min = max) |

All 18 pages and 33 sub-pages; all 21 page buttons; every OPTIONS / FILE / INFO item.

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
