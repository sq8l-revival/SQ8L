// What the editor logic needs from the synth engine (the original reaches these through
// TplugEditForm +0x4d4 CplugMaster, +0x4d8 CeditBuf, the global CsoundLib and CplugConfig, and
// CmidiIO for SEND/REQ).
//
// Notifications in the other direction are posted to the editor like the original's window
// messages (EditorController::post*): the master posts WM_APP+3 (0x8003) for
//   * a program loaded into the edit buffer (EditBuffer::Event::ProgramLoaded): wParam 0x10002;
//   * a settings change (CplugConfig listener): wParam 0, lParam = setting index (gui 0..5,
//     synth 0x10..0x14);
// and a received SysEx program arrives as WM_APP (0x8000) (see EditorController::sysexReceived).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "EditBuffer.h"
#include "Settings.h"
#include "SoundLibrary.h"

namespace sq8l::gui {

class EditorHost {
public:
    virtual ~EditorHost() = default;

    virtual EditBuffer& editBuffer() = 0;          // form +0x4d8
    virtual SoundLibrary& library() = 0;           // PTR_DAT_004c3178
    virtual const Settings& settings() = 0;        // master +0xfe0 (CplugConfig)
    // CplugConfig FUN_00452be8 / FUN_00452cac: change a setting; the host notifies the editor
    // (post 0x8003, wParam 0, lParam = index / 0x10 + index) when the value changed.
    virtual void setGuiSetting(int index, bool value) = 0;
    virtual void setSynthSetting(int index, int value) = 0;

    virtual void panic() = 0;                      // CplugMaster FUN_00462174 (reset all voices)
    virtual int voicesUsed() = 0;                  // FUN_00463328
    virtual int voicesMax() = 0;                   // master +0xf4c
    virtual std::string pluginDirectory() = 0;     // CplugConfig +0x1c (initial dialog folder)

    // MIDI ports for SEND/REQ (CmidiIO, unit midiio2): FormShow of TmidiSelForm rescans and
    // lists them (FUN_00478b2c, FUN_00478c30 / FUN_00478c3c).
    virtual std::vector<std::string> midiInPorts() = 0;
    virtual std::vector<std::string> midiOutPorts() = 0;
    virtual void midiCloseAll() = 0;               // FUN_00478bfc + FUN_00478bc8
    virtual bool midiOpenOut(int port) = 0;        // FUN_00478c6c + FUN_00478978 (true = opened)
    virtual void midiSendOut(const std::vector<uint8_t>& bytes) = 0;  // FUN_00478a04
    // FUN_00478c48 + FUN_004787b0: open the input for one SysEx message; the host then calls
    // EditorController::sysexReceived(data) from its MIDI thread callback.
    virtual bool midiOpenIn(int port) = 0;
    // FUN_00484254: Application.ProcessMessages until `ms` have elapsed (between SysEx sends).
    virtual void processMessagesFor(int ms) { (void)ms; }

    // ---- Additions of the port, not in the original. With portExtensions() false the editor
    // behaves exactly like the original (the differential GUI tests rely on it): no extra
    // OPTIONS items, no left click on the program number.
    virtual bool portExtensions() { return true; }
    // Change a [port] setting (Settings::port: 0 confirmLoad); the host notifies the editor
    // (wParam 0, lParam 0x20 + index) when the value changed.
    virtual void setPortSetting(int index, int value) { (void)index; (void)value; }
    // OPTIONS -> Zoom: size of the editor window in percent (100 = 626x430). setZoom resizes
    // the window; the host notifies the editor (wParam 0, lParam 0x30) when the size changed,
    // also when the user dragged the window to a new size.
    virtual int zoom() { return 100; }
    virtual void setZoom(int percent) { (void)percent; }
    // OPTIONS -> Polyphony: the playable voices of this instance, 0 = set by the program
    // (EMU -> VOICES). Per instance and saved with the plugin's state, not in SQ8L.ini.
    virtual int polyphonyOverride() { return 0; }
    virtual void setPolyphonyOverride(int voices) { (void)voices; }
};

}  // namespace sq8l::gui
