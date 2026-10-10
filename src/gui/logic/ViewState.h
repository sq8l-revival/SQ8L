// Output data model of the editor logic.
//
// The logic writes directly into the ported controls of the original form (layer 2,
// src/gui/EditorView.h), which hold exactly the fields of the original objects:
//
//   what the logic sets                          original object / offset             control API
//   -------------------------------------------  -----------------------------------  ---------------------------------
//   VFD characters and attributes (0 / 1 =       TLCD3 'lcd' (form +0x390), cell       view.lcd().writeText/clearCells
//     selected parameter, underlined)              rows +0x628 {dirty, char, attr}
//   bank + program number ("A000")               TLCD3 'numLcd' (+0x3b4)               view.numLcd().writeText
//   knob range / value / pixel distance          TGraphKnobB lcdKnob0..9 (+0x4e8[]):   view.knob(i).setMinValue/...
//                                                  +0x230/+0x234/+0x22c/+0x218
//   page scroll arrows (lit / dim)               TGraphButton pscrUp/DownButton +0x204 setAniIdx (0/1, 2/3)
//   SYNC / AM / MONO LEDs                        TAniDisplay ledSync/ledAm/ledMono     setValue(0 / 1)
//                                                  +0x20c
//   program name                                 TEdit progNameEdit (+0x3a8) Text      progNameEdit().setText
//   status bar (hints, "Welcome to SQ8L")        TLabel StatusLabel1 (+0x34c)          setStatusText
//   voices "used/available"                      TLabel StatusLabel2 (+0x344)          setVoicesText
//
// Everything else the logic owns (pages, parameters, menus, popups, mouse jump, dialogs) is
// in LcdControl.h / EditorController.h / Dialogs.h, with the original field offsets in the
// comments. Popup menus, message boxes, file dialogs, modal dialogs and the cursor go through
// PlatformUi.h.
#pragma once

#include "EditorView.h"

namespace sq8l::gui {
using EditorViewState = EditorView;
}  // namespace sq8l::gui
