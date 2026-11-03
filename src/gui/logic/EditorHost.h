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
#include "SoundParameters.h"
#include "Settings.h"
#include "SoundLibrary.h"

namespace sq8l::gui {

class EditorHost {
public:
    virtual ~EditorHost() = default;

    // Caller owns the engine. These semantic hooks never depend on a GUI framework.
    virtual int soundValue(unsigned index) {
        const auto* p = sound::parameter(index);
        return p ? sound::read(*p, editBuffer().current()) : 0;
    }
    // AM raw2 is a legacy panel state: LED off and the next AM click selects1.
    // Hosts with accepted state override this projection so a newer edit wins.
    virtual int soundButtonValue(unsigned index) {
        const auto* p = sound::parameter(index);
        if (p && p->field == 325 && editBuffer().current().s8(ofs::AmMod) == 2) return 2;
        return soundValue(index);
    }
    virtual void soundEdit(unsigned index, int value) {
        const auto* p = sound::parameter(index);
        if (p && sound::write(*p, editBuffer().current(), value)) editBuffer().setModified(true);
    }
    virtual void soundBegin(unsigned) {}
    virtual void soundEnd(unsigned) {}
    virtual uint64_t soundGeneration() { return 0; }

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

    // ---- Additions of the port, not in the original.
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
    // OPTIONS -> MTS-ESP. Of this instance, like the polyphony override above: the plugin
    // saves them in its own state, not in SQ8L.ini, so a project recalls them and two
    // instances can differ.
    virtual bool mtsEnabled() { return false; }
    virtual void setMtsEnabled(bool on) { (void)on; }
    virtual bool mtsCorrectPitch() { return false; }
    virtual void setMtsCorrectPitch(bool on) { (void)on; }
    // OPTIONS -> Support MIDI Program Change (issue #40). Of this instance and saved with the
    // plugin's state, like the switches above. On for a new instance, off for a project saved
    // by the original or by an earlier port.
    virtual bool programChangeEnabled() { return false; }
    virtual void setProgramChangeEnabled(bool on) { (void)on; }
    // OPTIONS -> Output gain (issue #52): the cut of this instance in dB, one of
    // Settings::kOutputCutsDb. Of this instance and saved with the plugin's state, like the
    // two above; setting it also records the value as where a newly created instance starts
    // (the SQ8L.ini key outputCut), the only part of it the user's ini keeps.
    virtual int outputCut() { return 0; }
    virtual void setOutputCut(int db) { (void)db; }
    // The connected master's scale name, empty when none is connected. Polled with the voices
    // counter, since a master may change its scale at any time.
    virtual std::string mtsScaleName() { return {}; }
};

}  // namespace sq8l::gui
