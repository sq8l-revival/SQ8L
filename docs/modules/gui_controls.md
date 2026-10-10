# GUI layers 1-2: graphics core, assets, controls, EditorView

Port of the original editor's *look and controls* (not its logic), pixel-exact against the
GUI oracle. Layer numbers refer to `docs/GUI_ARCHITECTURE.md`.

| File | What |
|---|---|
| `src/gui/Bitmap.h` | `Color` (0x00RRGGBB; `fromTColor()` for Delphi's 0x00BBGGRR), `Rect` (GDI, exclusive), `ImageView`, `Bitmap` |
| `src/gui/Canvas.{h,cpp}` | TCanvas subset with GDI semantics: FillRect, FrameRect, MoveTo/LineTo (end point excluded), Draw (opaque 1:1 blit), TextOut via `TextRenderer` |
| `src/gui/TextRenderer.h` | interface for text in Windows fonts (status bar, name edit); supplied by the platform layer, none = no text |
| `src/gui/Sprite.{h,cpp}` | `Sprite` = TAniGIF; `assets::` background + the 8 sprite strips (decoded once, thread-safe) |
| `src/gui/data/` | `GuiData.h` + generated `GuiBackground.cpp`, `GuiSprites.cpp` (`re/scripts/extract_gui_assets.py`) |
| `src/gui/GuiFpu.h` | x87 helpers: `roundEven` (Delphi Round at RC=nearest), `mulExt` (80-bit constant product) |
| `src/gui/controls/Control.{h,cpp}` | TControl/TWinControl subset: bounds, visibility, hint, surface, erase+paint, VCL mouse dynamic methods + events |
| `src/gui/controls/LcdDisplay.*` | TLCD3 (unit LCD3) |
| `src/gui/controls/Knob.*` | TGraphKnobB (unit graphKnobB) |
| `src/gui/controls/GraphButton.*` | TGraphButton (unit GraphButton) |
| `src/gui/controls/AniDisplay.*` | TAniDisplay (unit AniDisplay) |
| `src/gui/controls/StaticControls.*` | TPanel, TLabel, TEdit (program name), picture-less TImage (menu areas) |
| `src/gui/EditorView.*` | the form: all controls at their runtime positions, z-order, composition, mouse routing |
| `tests/capi_gui.cpp`, `tests/test_gui_*.py` | C API and differential tests against the emulated original |

## Assets

`python re/scripts/extract_gui_assets.py` opens the original editor in the emulator and reads
the bitmaps *the original decoded*: the form background is `Form.Brush.Bitmap` (FormShow,
plugEdit 0x483d9c, draws the `ImgBack` TJPEGImage into a TBitmap and makes it the form's
pattern brush, then frees ImgBack); every TAniGIF strip is the array of frame TBitmaps built by
TAniGIF.Loaded (0x457200) with the original's own `CGIFDecoder` (TBitmap+0x20 → TBitmapImage,
+8 = HBITMAP → oracle surface). Stored palettized (GIF strips <= 256 colours, background 1673
colours with uint16 indices).

| Sprite | Frames | Size | Use |
|---|---|---|---|
| KnobGif | 128 | 40x40 | knobs |
| charGIF | 272 | 11x18 | VFD: 68 glyphs x 4 attributes (plain, underline, dot, both) |
| numCharGIF | 17 | 18x23 | red 7-segment: blank, 0-9, A, b, c, d, P, U |
| LedGIF | 2 | 14x14 | LED off / on |
| ButtGIF | 6 | 44x22 | grey, red, green (each up / pressed) |
| smallButtGIF | 2 | 30x18 | SYNC/AM/MONO (up / pressed) |
| upDownGif | 4 | 30x18 | up, up pressed, down, down pressed |
| scrButtGif | 4 | 20x18 | page scroll: up dim, up lit, down dim, down lit |

## Painting model

Every control of the form is a windowed TCustomControl except the menu TImages (graphic
controls). The window surfaces are composed over the background in z-order (no overlaps).
`Control::paintWindow` = erase with the control's brush colour (ParentColor: the form colour
0x1A1B24; for the DoubleBuffered controls - knob, button, LCD - this happens in the memory DC
of TWinControl.WMPaint) then `paint()` then the graphic children (labels on panels).
`EditorView::render()` repaints everything like the oracle's `paint_all` (cheap).

## Controls

Field offsets are those of the original objects (used by `tests/test_gui_state.py`).

### LcdDisplay (TLCD3, 0x45733c-0x4587d8)
* geometry: char size from the GIF (+0x1f8/+0x1fc), gaps (+0x200/+0x204), frame (+0x208/
  +0x20c), cols/rows (+0x218/+0x21c); `width = 2*frameW + (charW+gapX)*cols - gapX` (likewise
  height). Any geometry setter clears all cells (FUN_00457f38).
* cells (+0x628): `{dirty, char, attr}`; char → glyph through a 256-entry map (+0x228) built by
  `setCharMap(chars, numAttr)` (FUN_00457b90: every code → 0, then `map[chars[i]] = i`);
  frame = `attr + glyph*numAttr`, `attr >= numAttr` → 0.
* `writeText(col, row, text, attr)` (FUN_00457fb4): clipped (negative col allowed); changed cells
  become dirty; sets the "something changed" flag (+0x631). `update()` (FUN_0045832c) repaints.
* Paint (0x458344): persistent back buffer (+0x62c); when its size differs from the control:
  resize, fill with colBack, 2-pixel bevel (colBorderLo top/left, colBorderHi bottom/right, the
  inner line at half intensity), redraw all; otherwise only dirty cells. Colours are only used at
  that moment (like the original).
* Mouse: MouseDown/Up/Move/DblClick convert to cells (FUN_00458558: `(x-frameW)/(charW+gapX)`,
  truncating) and fire `onCellMouseDown(Sender, Button, Shift, col, row)` (+0x648),
  `onCellMouseUp` (+0x650), `onCellMouseMove(Sender, Shift, col, row)` (+0x658),
  `onDblClickPos(x, y)` (+0x660, unused by SQ8L), `onCellDblClick(col, row)` (+0x668), after the
  TControl events.

### Knob (TGraphKnobB, 0x4796ec-0x47b3e4)
* Value model (Single): value +0x22c, min +0x230, max +0x234, range +0x238, step +0x23c,
  pixFactor +0x244 (value per pixel), snapToZero +0x248, snapZone +0x24c (= 0.04*range, 80-bit
  constant), maxPixDist +0x218, maxFinePixDist +0x21c, minFineFac +0x1fc.
* `setValue` (FUN_0047aa8c): step > 0 → `Round(v/step)*step`; clamp; angle update.
  `setMinValue`/`setMaxValue` (0x47abd0/0x47acd0): recompute range, snapZone,
  `pixFactor = range/(maxPixDist*0.5)`, angle factor; `setMaxPixDist` (0x47aa44):
  `pixFactor = range/maxPixDist` (no 0.5!); `setValueStep` (0x47add0).
* Frame shown: `Round((value-min)/(max-min) * 128)` clamped to 0..127 (x87 53-bit, half-even).
* Drag: MouseDown(left) → `beginDrag`: save cursor (OnGetMousePos +0x2e8 or Mouse.CursorPos),
  `acc = value ± snapZone`. MouseMove while captured → `dragMove`:
  `dx = min(|x-downX|+1, maxFinePixDist)`, `fac = Shift ? minFineFac : max(1-dx/maxFinePixDist,
  minFineFac)`, `acc += (lastY-y)*fac*pixFactor`, value = 0 inside the snap zone else
  `acc ∓ snapZone`; fires `onChange` (+0x2e0). A move without the capture ends the drag.
  MouseUp → `endDrag`: restore the cursor (OnRestoreMousePos +0x2f0 or Mouse.CursorPos).
  DblClick → `cancelDrag` (no restore) then OnDblClick, then MouseDown(ssDouble) starts a drag.
* Not ported (unused by SQ8L): caption/value text and value TEdit, TImageList and vector
  renderers. **No mouse-wheel support** (the original ignores WM_MOUSEWHEEL, checked in the
  oracle).
* Port addition — `setActive(false)`: the knob is drawn faded halfway towards the backdrop
  the frame itself carries in its corner pixel (`Canvas::drawFaded`; the HD renderer blends
  the whole knob over the background with the same amount, `Knob::kFadeAmount`). The editor
  turns it off for the knobs the current display page has no parameter for, which turn
  nothing — issue #24.

### GraphButton (TGraphButton, 0x47ba3c-0x47c80c)
* frame = `AniIdx + (HasTwoFrames && pressed)`; layout (FUN_0047c100) resizes the control to
  the GIF frame (+PressDisplacement); hover frame (FrameRect in frameColor) when
  frameWidth > 0 (the page scroll arrows: 1, TColor 0x5f7c0e).
* MouseDown(left) → pressed; MouseMove: inside → Mouse.Capture := self + OnMouseMove, outside →
  release capture, hover off, press cancelled; MouseUp(left) → Click if still pressed.
  TControl.Click is overridden empty, so OnClick only comes from MouseUp.

### AniDisplay (TAniDisplay, 0x47b3e4-0x47ba3c)
`frame = Round((value-min)/(max-min)*(numFrames-1) + startFrame)` (index == count → blue
rectangle fallback); resizes itself to the GIF frame on Paint (10x10 → 14x14 for the LEDs).

### Panel / Label / NameEdit / ImageArea
Colour fills; text through `TextRenderer` (MS Sans Serif 8 = Font.Height -11 for the status
labels, colour TColor 10521997; Arial -11, TColor 9140344 on black for the program name,
drawn at (1, 0)). `Label::setCaption` auto-sizes with the renderer (DT_CALCRECT, right edge kept
for StatusLabel2 / taRightJustify).

## EditorView (state model)

`EditorView` owns: `lcd()`, `numLcd()`, `knob(0..9)` (lcdKnob0-9: 0-4 top row, 5-9 bottom row,
x = 118 + 96*i), `button(name)` (33 TGraphButtons, form names), `ledSync()`, `ledAm()`,
`ledMono()`, `statusPanel2()`/`statusLabel2()` (voices), `panel1()`/`statusLabel1()` (status
bar), `progNameEdit()`, `image(name)` (menuFileImage, menuOptImage, menuInfoImage,
menuPanicImage), `form()`. The constructor reproduces the form after FormShow (LCD 44x2,
charGIF, gaps 1/3, frame 4, charset `kLcdCharset` + lowercase→uppercase; numLcd 4x1 numCharGIF,
charset `kNumLcdCharset` = `" 0123456789ABCDPU"`; knobs Radius 20, no caption/value,
MinFineFac 0.2, MaxFinePixDist 100, DoRestoreMousePos; scroll arrows' hover frame; z-order).

Input: `mouseMove/mouseDown/mouseUp/mouseDoubleClick(x, y, MK_* keys)` in form coordinates,
routed like Windows + VCL: to the capture window if any, else the topmost visible child, else
the form; inside a window to its graphic children (TWinControl.IsControlMouseMsg).
WM_LBUTTONDOWN captures (csCaptureMouse) and sets csClicked, WM_LBUTTONUP releases and calls
Click if inside, WM_LBUTTONDBLCLK = DblClick + MouseDown(ssDouble). `onContextMenu` is called
after a right button up. `getCursorPos`/`setCursorPos` back the knobs' cursor restore.

## Verification

```
cmake -S . -B build -G Ninja -DSQ8L_BUILD_PLUGIN=OFF && cmake --build build
SQ8L_TESTAPI=$PWD/build/libsq8l_testapi.dylib .venv/bin/python tests/test_gui_render.py
SQ8L_TESTAPI=$PWD/build/libsq8l_testapi.dylib .venv/bin/python tests/test_gui_controls.py
```

* `test_gui_render.py`: 162 original states (start-up, all 21 page buttons with their
  scrolled sub-pages, knob drags incl. horizontal distance and Shift on both rows, hover frames
  and hints, pressed and press-cancelled buttons, MONO/SYNC/AM on/off, program up/down, bank
  switches, LCD clicks/drags/double/right clicks, voices counter with notes playing, programs
  45/135/301/383 at start-up). The state of every original control is read from emulator
  memory and loaded into a fresh C++ EditorView: **0 differing pixels in 43 million pixels**
  compared outside the masked text (116 distinct frames, 51 distinct LCD contents, 62 knob
  frames). Masked: StatusLabel1, StatusLabel2 (label rectangles clipped to their panels) and
  the progNameEdit rectangle; their non-text pixels around are compared.
* `test_gui_controls.py`: all 435 sprite frames + background; EditorView defaults (411 checks);
  4000 random TGraphKnobB method calls on an original knob vs the C++ knob, all 28 fields
  bit-exact after every call + frame index; 1500 random `writeText` calls (cells, dirty flags);
  600 TAniDisplay setter calls; 25 random knob/LED value frames; 30 incremental LCD repaints on
  a persistent C++ view (dirty-cell path); 715 mouse messages replayed on both (knob drags,
  double clicks, right clicks, button hover/press/leave/cross-moves, LCD cell events, menu
  areas, form) with identical capture, event streams and control state after each message.

## Notes for the editor-logic port (layer 3)

* **LCD content**: lcdControl (ClcdCtr, unit lcdControl 0x4587d8-0x45b86c, set up in
  unit_47e330 FUN_0047e340 from page definition strings) writes with `writeText(col, row,
  text, attr)` and calls `update()`. Attribute 1 (underline) marks the selected parameter
  (label + value cells); 0 elsewhere; nothing blinks. Unknown characters map to glyph 0 (blank),
  lowercase is shown as uppercase. The welcome text stays ~1 s after opening.
* **numLcd**: `writeText(0, 0, "A000", 0)` (bank letter + 3 digits, FUN_00484304).
* **Knobs**: per parameter (FUN_00484700): `setMinValue(min)`, `setMaxValue(max)`,
  `setMaxPixDist(clamp(Trunc((max-min+1)*1.5625), 40, 180))`; ValueStep 1 → integer values.
  A page without a parameter for a knob reports the range 0, 0, which the port also takes as
  `setActive(false)` (the faded knob above).
  OnChange = lcdKnobChange. Drags can also start on the LCD: the form's lcd OnMouseDown finds
  the knob for the cell and calls `knob.beginDrag`, lcd OnMouseMove → `dragMove`, OnMouseUp →
  `endDrag` (plugEdit 0x48452c/0x484598/0x484570). The form installs OnGetMousePos /
  OnRestoreMousePos (mouseJump, FUN_00483a5c/FUN_00483a84) and OnDblClick (0x485148).
* **Buttons**: page buttons fire OnClick = pageButtonClick (Tag = page id: buttWav 15,
  buttOsc1 16, ...); the logic sets `AniIdx` (0 grey, 2 red, 4 green). Scroll arrows: up 0/1,
  down 2/3 (dim/lit); they cycle sub-pages.
* **LEDs**: `setValue(0.0 or 1.0)` from the program's mono/sync/AM flags (FUN_004843cc).
* **Hints**: most controls have OnMouseMove = MouseMoveHint: when the sender changes, the status
  bar shows `Sender.Hint` (`EditorView::setStatusText`). Hovering LCD parameters shows their
  hint (lcd cell-move handler); knobs show their parameter's hint (OnMouseMove 0x485170).
* **Voices**: the 20 ms timer writes "used/available" to StatusLabel2 (`setVoicesText`).
* **Text**: provide a `TextRenderer` (the oracle uses macOS' Microsoft Sans Serif, so text
  pixels are not a reference). The program name edit box needs real keyboard editing
  (uppercase, max 15 characters) from the platform layer.
