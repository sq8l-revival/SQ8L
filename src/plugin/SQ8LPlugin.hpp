// SQ8L port: DPF plugin wrapper around the engine (sq8l::Synth).
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

#include "DistrhoPlugin.hpp"
#include "MtsEspTuning.h"
#include "SQ8LVersion.h"
#include "SharedLibrary.h"
#include "Synth.h"
#include "SoundParameterState.h"

START_NAMESPACE_DISTRHO

class SQ8LPlugin : public Plugin,
                   public Vst2ProgramHooks,
                   public SQ8LParameterRefreshHooks,
                   public sq8l::MasterNotify {
public:
  SQ8LPlugin();
  ~SQ8LPlugin() override;

  // ---- shared with the editor (DISTRHO_PLUGIN_WANT_DIRECT_ACCESS)
  // Everything that touches the engine outside run() must hold this lock. The
  // audio thread blocks on it, so UI sections must stay short (modal UI
  // releases it).
  std::recursive_mutex &engineMutex() { return mutex_; }
  sq8l::Synth &synth() { return synth_; }
  // Already-owned editor helpers: caller holds engineMutex(). No host callbacks.
  sq8l::sound::SoundParameterService& soundParameters() { return *parameters_; }
  // Scope an explicit synchronous UI bridge echo on this thread only.
  void dispatchPanelEcho(const sq8l::sound::EchoToken&, const std::function<void()>&);
  bool sq8lReadAcceptedParameter(uint32_t, SQ8LAcceptedParameter&) override;
  bool sq8lCopyParameterRefresh(uint64_t&, float*, uint32_t) override;
  bool sq8lCapturePanelNotification(uint32_t, float, SQ8LPanelNotificationStamp&) override;
  bool sq8lCurrentPanelNotification(const SQ8LPanelNotificationStamp&) override;
  sq8l::Settings &settings() { return *shared_.settings; }
  // (port) The connected MTS-ESP master's scale name, empty when there is none.
  const std::string &mtsScaleName() const { return mts_.scaleName(); }
  // Editor notifications from the master (CSynth 0x487bb8: WM_APP+3 posted to
  // the form), queued under the engine lock and drained by the editor.
  struct EditorMessage {
    uint32_t wParam;
    int32_t lParam;
  };
  // Copies rather than swaps: a swap would hand the reserved buffer to the
  // editor and leave the audio thread with an empty one to grow again. clear()
  // keeps the capacity.
  std::vector<EditorMessage> takeEditorMessages() {
    std::vector<EditorMessage> out = editorMessages_;
    editorMessages_.clear();
    return out;
  }
  void notifyEditor(int32_t a, int32_t b, int32_t c) override {
    if (editorMessages_.size() >= 64)
      editorMessages_.erase(editorMessages_.begin()); // editor closed
    editorMessages_.push_back(
        {(static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) & 0xFFFF),
         c});
  }

  // ---- VST2 program semantics of the original (CSynth
  // v010/v011/v012/v013/v047)
  void vst2SetProgram(int32_t index) override;
  int32_t vst2GetProgram() override;
  void vst2SetProgramName(const char *name) override;
  void vst2GetProgramName(char *out24) override;
  bool vst2GetProgramNameIndexed(int32_t index, char *out24) override;

protected:
  const char *getLabel() const override { return "SQ8L"; }
  const char *getDescription() const override { return "Ensoniq SQ-80 emulation"; }
  const char *getMaker() const override { return "Siegfried Kullmann"; }
  const char *getLicense() const override { return "Free and Open Source"; }
  const char *getHomePage() const override { return "" /* "https://www.kvraudio.com/product/sq8l-by-siegfried-kullmann" */ ; }
  uint32_t getVersion() const override { return d_version(sq8l::kVersionMajor, sq8l::kVersionMinor, sq8l::kVersionPatch); }

  void initParameter(uint32_t index, Parameter& parameter) override;
  void initPortGroup(uint32_t groupId, PortGroup& group) override;
  float getParameterValue(uint32_t index) const override;
  void setParameterValue(uint32_t index, float value) override;
  void initProgramName(uint32_t index, String &name) override;
  void loadProgram(uint32_t index) override;
  void initState(uint32_t index, State &state) override;
  String getState(const char *key) const override;
  void setState(const char *key, const char *value) override;
  void sampleRateChanged(double sr) override;
  void run(const float **, float **outputs, uint32_t frames,
           const MidiEvent *midi, uint32_t midiCount) override;

private:
  // Declared before synth_: the engine notifies (notifyEditor) while it is
  // constructed. Reserved to the cap notifyEditor enforces, in the initialiser
  // rather than the body because a program change arriving in run() reaches
  // notifyEditor on the audio thread, where push_back must not allocate.
  std::vector<EditorMessage> editorMessages_ = [] {
    std::vector<EditorMessage> v;
    v.reserve(64);
    return v;
  }();
  mutable std::recursive_mutex mutex_;
  sq8l::SharedLibrary::Handle shared_;
  // (port) MTS-ESP: declared before synth_, which is handed a pointer to its
  // snapshot.
  sq8l::MtsEspTuning mts_;
  sq8l::Synth synth_;
  std::vector<sq8l::RawMidiEvent> events_;
  std::unique_ptr<sq8l::sound::SoundParameterService> parameters_;
  std::unique_ptr<sq8l::sound::SoundParameterLifecycle> parameterLifecycle_;

  // (port, issue #42) The song position for the LFOs' tempo sync, in quarter notes, NaN
  // when there is none. bbtStyle_ is how this binary's format wrapper built the bar, beat
  // and tick that songQuarters() takes apart again.
  enum class BbtStyle { Vst, Quarters, Signature };
  BbtStyle bbtStyle_ = BbtStyle::Signature;
  double songQuarters(const TimePosition &position) const;

  DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SQ8LPlugin)
};

END_NAMESPACE_DISTRHO
