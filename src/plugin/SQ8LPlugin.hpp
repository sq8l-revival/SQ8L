// SQ8L port: DPF plugin wrapper around the engine (sq8l::Synth).
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "DistrhoPlugin.hpp"
#include "MtsEspTuning.h"
#include "SharedLibrary.h"
#include "Synth.h"

START_NAMESPACE_DISTRHO

class SQ8LPlugin : public Plugin, public Vst2ProgramHooks, public sq8l::MasterNotify {
public:
    SQ8LPlugin();
    ~SQ8LPlugin() override;

    // ---- shared with the editor (DISTRHO_PLUGIN_WANT_DIRECT_ACCESS)
    // Everything that touches the engine outside run() must hold this lock. The audio
    // thread blocks on it, so UI sections must stay short (modal UI releases it).
    std::recursive_mutex& engineMutex() { return mutex_; }
    sq8l::Synth& synth() { return synth_; }
    sq8l::Settings& settings() { return *shared_.settings; }
    // (port) The connected MTS-ESP master's scale name, empty when there is none.
    const std::string& mtsScaleName() const { return mts_.scaleName(); }
    // Editor notifications from the master (CSynth 0x487bb8: WM_APP+3 posted to the form),
    // queued under the engine lock and drained by the editor.
    struct EditorMessage {
        uint32_t wParam;
        int32_t lParam;
    };
    std::vector<EditorMessage> takeEditorMessages() {
        std::vector<EditorMessage> out;
        out.swap(editorMessages_);
        return out;
    }
    void notifyEditor(int32_t a, int32_t b, int32_t c) override {
        if (editorMessages_.size() >= 64) editorMessages_.erase(editorMessages_.begin());  // editor closed
        editorMessages_.push_back({(static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) & 0xFFFF), c});
    }

    // ---- VST2 program semantics of the original (CSynth v010/v011/v012/v013/v047)
    void vst2SetProgram(int32_t index) override;
    int32_t vst2GetProgram() override;
    void vst2SetProgramName(const char* name) override;
    void vst2GetProgramName(char* out24) override;
    bool vst2GetProgramNameIndexed(int32_t index, char* out24) override;

protected:
    const char* getLabel() const override { return "SQ8L"; }
    const char* getDescription() const override { return "Ensoniq SQ-80 emulation (port of SQ8L 0.91b)"; }
    const char* getMaker() const override { return "Siegfried Kullmann"; }
    const char* getHomePage() const override { return "https://www.kvraudio.com/product/sq8l-by-siegfried-kullmann"; }
    const char* getLicense() const override { return "Freeware"; }
    uint32_t getVersion() const override { return d_version(0, 9, 1); }

    void initProgramName(uint32_t index, String& name) override;
    void loadProgram(uint32_t index) override;
    void initState(uint32_t index, State& state) override;
    String getState(const char* key) const override;
    void setState(const char* key, const char* value) override;
    void sampleRateChanged(double sr) override;
    void run(const float**, float** outputs, uint32_t frames, const MidiEvent* midi, uint32_t midiCount) override;

private:
    // Declared before synth_: the engine notifies (notifyEditor) while it is constructed.
    std::vector<EditorMessage> editorMessages_;
    mutable std::recursive_mutex mutex_;
    sq8l::SharedLibrary::Handle shared_;
    // (port) MTS-ESP: declared before synth_, which is handed a pointer to its snapshot.
    sq8l::MtsEspTuning mts_;
    sq8l::Synth synth_;
    std::vector<sq8l::RawMidiEvent> events_;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SQ8LPlugin)
};

END_NAMESPACE_DISTRHO
