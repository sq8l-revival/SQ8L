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
// VST2 chunks are the original's raw bytes (571-byte "SQ8L.EDIT" record) so projects
// saved with the original 32-bit plugin load directly (see patches/dpf-vst2-compat.patch).
#define DISTRHO_PLUGIN_VST2_RAW_CHUNK_KEY "editbuffer"

// VST2 programs behave like the original: 512 library slots, edit-buffer name,
// indexed names from the library (see patches/dpf-vst2-compat.patch).
#define DISTRHO_PLUGIN_VST2_NUM_PROGRAMS 512
#define DISTRHO_PLUGIN_VST2_PROGRAM_HOOKS 1
#ifdef __cplusplus
#include <cstdint>
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
