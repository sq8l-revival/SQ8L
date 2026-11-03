// SQ8L port: plugin identity. The VST2 unique ID matches the original ('SQ8L') so
// hosts can load projects saved with the original 32-bit plugin.
#pragma once

#define DISTRHO_PLUGIN_BRAND "Siegfried Kullmann"
#define DISTRHO_PLUGIN_NAME "SQ8L"
#define DISTRHO_PLUGIN_URI "urn:sq8l:sq8l"
#define DISTRHO_PLUGIN_CLAP_ID "sq8l.sq8l"
#define DISTRHO_PLUGIN_BRAND_ID SQ8L
#define DISTRHO_PLUGIN_UNIQUE_ID SQ8L

#define DISTRHO_PLUGIN_HAS_UI 1
#define DISTRHO_PLUGIN_WANT_DIRECT_ACCESS 1
#define DISTRHO_UI_DEFAULT_WIDTH 626
#define DISTRHO_UI_DEFAULT_HEIGHT 430
#define DISTRHO_UI_USER_RESIZABLE 1  // OPTIONS -> Zoom (port addition), aspect ratio kept
#define DISTRHO_UI_USE_NANOVG 0
// Linux: the editor's file dialogs come from DPF's file browser (elsewhere: native ones).
#if defined(__linux__)
#define DISTRHO_UI_FILE_BROWSER 1
#endif
#define DISTRHO_PLUGIN_IS_RT_SAFE 1
#define DISTRHO_PLUGIN_IS_SYNTH 1
#define DISTRHO_PLUGIN_NUM_INPUTS 0
#define DISTRHO_PLUGIN_NUM_OUTPUTS 2
#define DISTRHO_PLUGIN_WANT_MIDI_INPUT 1
#define DISTRHO_PLUGIN_WANT_STATE 1
#define DISTRHO_PLUGIN_WANT_FULL_STATE 1
#define DISTRHO_PLUGIN_WANT_PROGRAMS 1
#define DISTRHO_PLUGIN_WANT_TIMEPOS 1  // LFO tempo sync (port addition, issue #42)
// VST2 chunks are the original's raw bytes (571-byte "SQ8L.EDIT" record) so projects
// saved with the original 32-bit plugin load directly (see patches/dpf-vst2-compat.patch).
#define DISTRHO_PLUGIN_VST2_RAW_CHUNK_KEY "editbuffer"

// VST2 programs behave like the original: 512 library slots, edit-buffer name,
// indexed names from the library (see patches/dpf-vst2-compat.patch).
#define DISTRHO_PLUGIN_VST2_NUM_PROGRAMS 512
#define DISTRHO_PLUGIN_VST2_PROGRAM_HOOKS 1
#ifndef DISTRHO_PLUGIN_SQ8L_PARAMETER_REFRESH
#define DISTRHO_PLUGIN_SQ8L_PARAMETER_REFRESH 1
#endif
#ifdef __cplusplus
#include <cstdint>
// Optional wrapper bridge. No callback, allocation or format policy in this API.
// Each wrapper owns a generation cursor (initially 0). The implementation locks
// the existing engine mutex, copies the entire accepted view on replacement and
// advances the cursor atomically with that copy. Values include pending edits.
// Replacements advance generation; ordinary Host/Panel edits do not. A snapshot
// is an observation, never an edit or an acknowledgement of pending work.
// Call only with no engine ownership; host/UI delivery follows the return.
// Fixed observation stamp for a deferred transport of an already accepted panel
// edit. source is an opaque service identity, never dereferenced. Capture is legal
// only inside dispatchPanelEcho; validation rechecks exact revision/origin under
// the engine mutex. Neither operation accepts an edit or invokes a callback.
struct SQ8LPanelNotificationStamp {
    uint64_t generation = 0, revision = 0;
    uint32_t index = 256;
    int value = 0;
    const void* source = nullptr;
};
// Coherent read-only accepted value and exact existing edit identity. No callback.
struct SQ8LAcceptedParameter {
    uint64_t generation = 0, revision = 0;
    float value = 0.f;
};
struct SQ8LParameterRefreshHooks {
    static constexpr uint32_t kParameterCount = 256;
    virtual ~SQ8LParameterRefreshHooks() = default;
    virtual bool sq8lReadAcceptedParameter(uint32_t, SQ8LAcceptedParameter&) = 0;
    virtual bool sq8lCopyParameterRefresh(uint64_t& generation, float* values, uint32_t count) = 0;
    virtual bool sq8lCapturePanelNotification(uint32_t, float, SQ8LPanelNotificationStamp&) = 0;
    virtual bool sq8lCurrentPanelNotification(const SQ8LPanelNotificationStamp&) = 0;
};
struct Vst2ProgramHooks {
    virtual ~Vst2ProgramHooks() = default;
    virtual void vst2SetProgram(int32_t index) = 0;
    virtual int32_t vst2GetProgram() = 0;
    virtual void vst2SetProgramName(const char* name) = 0;
    virtual void vst2GetProgramName(char* out24) = 0;
    virtual bool vst2GetProgramNameIndexed(int32_t index, char* out24) = 0;
};
#endif

#define DISTRHO_PLUGIN_VST3_CATEGORIES "Instrument|Synth"
#define DISTRHO_PLUGIN_CLAP_FEATURES "instrument", "synthesizer", "stereo"
#define DISTRHO_PLUGIN_AU_TYPE aumu
