// C API: program record, SysEx conversion, sound library, edit buffer and settings.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "EditBuffer.h"
#include "Program.h"
#include "Settings.h"
#include "SoundLibrary.h"
#include "SysEx.h"

#include "capi_export.h"

using namespace sq8l;

namespace {

Program toProgram(const uint8_t* b) {
    Program p;
    std::memcpy(p.bytes, b, kProgramSize);
    return p;
}

int copyOut(const std::vector<uint8_t>& v, uint8_t* out, int cap) {
    const int n = static_cast<int>(v.size());
    if (out) std::memcpy(out, v.data(), size_t(n < cap ? n : cap));
    return n;
}

struct EditBox {
    EditBuffer eb;
    std::vector<int32_t> events;  // pairs (event, slot index or -1)
    explicit EditBox(SoundLibrary* lib) : eb(lib) {
        eb.listener = [this](EditBuffer::Event e, const Program* slot) {
            events.push_back(static_cast<int32_t>(e));
            int idx = -1;
            for (int i = 0; i < 5 && slot; i++)
                if (&eb.slot(i) == slot) idx = i;
            events.push_back(idx);
        };
    }
};

}  // namespace

// ---------------------------------------------------------------- program
SQ8L_API void sq8l_prog_init(uint8_t* out) {
    Program p = makeInitProgram();
    std::memcpy(out, p.bytes, kProgramSize);
}

SQ8L_API void sq8l_prog_convert_old(const uint8_t* src, uint8_t* out) {
    Program p = toProgram(out);
    convertOldProgram(src, p);
    std::memcpy(out, p.bytes, kProgramSize);
}

SQ8L_API int sq8l_prog_name(const uint8_t* prog, char* out, int cap) {
    const std::string s = toProgram(prog).name();
    const int n = static_cast<int>(s.size());
    std::memcpy(out, s.data(), size_t(n < cap ? n : cap));
    return n;
}

SQ8L_API void sq8l_prog_set_name(uint8_t* prog, const char* s, int len) {
    Program p = toProgram(prog);
    p.setName(std::string(s, size_t(len)));
    std::memcpy(prog, p.bytes, kProgramSize);
}

// ---------------------------------------------------------------- param table
SQ8L_API int sq8l_param_count() { return static_cast<int>(paramTable().size()); }
SQ8L_API void sq8l_param_entry(int i, uint32_t* offset, uint8_t* shift, uint8_t* type) {
    const ParamField& f = paramTable()[size_t(i)];
    *offset = f.offset;
    *shift = f.shift;
    *type = f.type;
}
SQ8L_API const uint8_t* sq8l_param_descriptor() { return paramDescriptor().data(); }
SQ8L_API int32_t sq8l_param_get(const uint8_t* prog, int i) { return getParam(toProgram(prog), i); }
SQ8L_API void sq8l_param_set(uint8_t* prog, int i, int32_t v) {
    Program p = toProgram(prog);
    setParam(p, i, v);
    std::memcpy(prog, p.bytes, kProgramSize);
}
SQ8L_API int sq8l_param_find(uint32_t off) { return findParam(off); }

// ---------------------------------------------------------------- SysEx
SQ8L_API void sq8l_sysex_from_nybbles(const uint8_t* nyb, uint8_t* out) {
    Program p = toProgram(out);
    sysex::programFromNybbles(nyb, p);
    std::memcpy(out, p.bytes, kProgramSize);
}
SQ8L_API void sq8l_sysex_to_nybbles(const uint8_t* prog, uint8_t* out, int esq1) {
    sysex::programToNybbles(toProgram(prog), out, esq1 != 0);
}
SQ8L_API void sq8l_sysex_from_sq80(const uint8_t* raw, uint8_t* out, int f192, int f196, int f13b,
                                   int level) {
    Program p = toProgram(out);
    sysex::programFromSq80Bytes(raw, p, uint8_t(f192), uint8_t(f196), uint8_t(f13b), int8_t(level));
    std::memcpy(out, p.bytes, kProgramSize);
}
SQ8L_API int sq8l_sysex_source_from(int s) { return sysex::sourceFromSq80(uint8_t(s)); }
SQ8L_API int sq8l_sysex_source_to(int s) { return sysex::sourceToSq80(int16_t(s)); }
SQ8L_API int sq8l_sysex_name_from(const uint8_t* chars, int n, char* out, int cap) {
    const std::string s = sysex::nameFromSq80(chars, n);
    const int len = static_cast<int>(s.size());
    std::memcpy(out, s.data(), size_t(len < cap ? len : cap));
    return len;
}
SQ8L_API void sq8l_sysex_name_to(const char* s, int len, int n, uint8_t* out) {
    const std::vector<uint8_t> v = sysex::nameToSq80(std::string_view(s, size_t(len)), n);
    std::memcpy(out, v.data(), v.size());
}
SQ8L_API int sq8l_sysex_parse_header(const uint8_t* d, int* type) { return sysex::parseHeader(d, type); }
SQ8L_API int sq8l_sysex_single_dump(const uint8_t* prog, uint8_t* out, int cap) {
    return copyOut(sysex::makeSingleDump(toProgram(prog)), out, cap);
}

// ---------------------------------------------------------------- library
SQ8L_API void* sq8l_lib_new(const uint8_t* backup, int size) {
    if (backup && size > 0) return new SoundLibrary(std::vector<uint8_t>(backup, backup + size));
    return new SoundLibrary();
}
SQ8L_API void sq8l_lib_free(void* l) { delete static_cast<SoundLibrary*>(l); }
SQ8L_API uint8_t* sq8l_lib_programs(void* l) { return static_cast<SoundLibrary*>(l)->raw(); }
SQ8L_API const uint8_t* sq8l_lib_header(void* l) { return static_cast<SoundLibrary*>(l)->header(); }
SQ8L_API int sq8l_lib_clean(void* l) { return static_cast<SoundLibrary*>(l)->clean(); }
SQ8L_API void sq8l_lib_set_state(void* l, const uint8_t* header, int clean) {
    auto* lib = static_cast<SoundLibrary*>(l);
    std::memcpy(const_cast<uint8_t*>(lib->header()), header, SoundLibrary::kHeaderSize);
    lib->setClean(clean != 0);
}
SQ8L_API void sq8l_lib_restore_backup(void* l, const uint8_t* d, int n) {
    static_cast<SoundLibrary*>(l)->restoreBackup(n > 0 ? d : nullptr, size_t(n > 0 ? n : 0));
}
SQ8L_API int sq8l_lib_load_library(void* l, const uint8_t* d, int n) {
    return static_cast<SoundLibrary*>(l)->loadLibrary(d, size_t(n));
}
SQ8L_API int sq8l_lib_save_library(void* l, uint8_t* out, int cap) {
    return copyOut(static_cast<SoundLibrary*>(l)->saveLibrary(), out, cap);
}
SQ8L_API int sq8l_lib_save_backup(void* l, uint8_t* out, int cap) {
    return copyOut(static_cast<SoundLibrary*>(l)->saveBackup(), out, cap);
}
SQ8L_API int sq8l_lib_load_bank(void* l, const uint8_t* d, int n, int bank) {
    return static_cast<SoundLibrary*>(l)->loadBank(d, size_t(n), bank);
}
SQ8L_API int sq8l_lib_save_bank(void* l, int bank, uint8_t* out, int cap) {
    return copyOut(static_cast<SoundLibrary*>(l)->saveBank(bank), out, cap);
}
SQ8L_API void sq8l_lib_init_library(void* l, int all) { static_cast<SoundLibrary*>(l)->initLibrary(all != 0); }
SQ8L_API void sq8l_lib_init_bank(void* l, int bank) { static_cast<SoundLibrary*>(l)->initBank(bank); }
SQ8L_API int sq8l_lib_import_sysex_bank(void* l, const uint8_t* d, int n, int start, int check) {
    return static_cast<SoundLibrary*>(l)->importSysexBank(d, size_t(n), start, check != 0);
}
SQ8L_API int sq8l_lib_export_sysex_bank(void* l, int start, uint8_t* out, int cap) {
    return copyOut(static_cast<SoundLibrary*>(l)->exportSysexBank(start), out, cap);
}
SQ8L_API int sq8l_lib_write_program(void* l, int index, const uint8_t* prog) {
    return static_cast<SoundLibrary*>(l)->writeProgram(index, toProgram(prog));
}
SQ8L_API int sq8l_lib_program_name(void* l, int index, char* out, int cap) {
    const std::string s = static_cast<SoundLibrary*>(l)->programName(index);
    const int n = static_cast<int>(s.size());
    std::memcpy(out, s.data(), size_t(n < cap ? n : cap));
    return n;
}

// ---------------------------------------------------------------- edit buffer
SQ8L_API void* sq8l_eb_new(void* lib) { return new EditBox(static_cast<SoundLibrary*>(lib)); }
SQ8L_API void sq8l_eb_free(void* e) { delete static_cast<EditBox*>(e); }
SQ8L_API int sq8l_eb_state_size() { return static_cast<int>(EditBuffer::kStateSize); }
SQ8L_API void sq8l_eb_save_state(void* e, uint8_t* img) { static_cast<EditBox*>(e)->eb.saveState(img); }
SQ8L_API void sq8l_eb_load_state(void* e, const uint8_t* img) { static_cast<EditBox*>(e)->eb.loadState(img); }
SQ8L_API int sq8l_eb_events(void* e, int32_t* out, int cap) {
    auto& ev = static_cast<EditBox*>(e)->events;
    const int n = static_cast<int>(ev.size());
    std::memcpy(out, ev.data(), sizeof(int32_t) * size_t(n < cap ? n : cap));
    ev.clear();
    return n;
}
SQ8L_API void sq8l_eb_init_program(void* e) { static_cast<EditBox*>(e)->eb.initProgram(); }
SQ8L_API void sq8l_eb_reset_ext_zone(void* e, int z) { static_cast<EditBox*>(e)->eb.resetExtZone(z); }
SQ8L_API void sq8l_eb_set_bank(void* e, int b) { static_cast<EditBox*>(e)->eb.setBank(b); }
SQ8L_API int sq8l_eb_select(void* e, int prog, int bank) { return static_cast<EditBox*>(e)->eb.selectProgram(prog, bank); }
SQ8L_API int sq8l_eb_select_index(void* e, int idx) { return static_cast<EditBox*>(e)->eb.selectIndex(idx); }
SQ8L_API int sq8l_eb_write(void* e, int prog, int bank) { return static_cast<EditBox*>(e)->eb.writeProgram(prog, bank); }
SQ8L_API void sq8l_eb_compare(void* e, int prog, int bank) { static_cast<EditBox*>(e)->eb.compare(prog, bank); }
SQ8L_API void sq8l_eb_compare_off(void* e) { static_cast<EditBox*>(e)->eb.compareOff(); }
SQ8L_API int sq8l_eb_get_chunk(void* e, uint8_t* out, int cap) {
    return copyOut(static_cast<EditBox*>(e)->eb.getChunk(), out, cap);
}
SQ8L_API int sq8l_eb_set_chunk(void* e, const uint8_t* d, int n) {
    return static_cast<EditBox*>(e)->eb.setChunk(d, size_t(n));
}
SQ8L_API int sq8l_eb_import_sysex(void* e, const uint8_t* d, int n, int check) {
    return static_cast<EditBox*>(e)->eb.importSysex(d, size_t(n), check != 0);
}
SQ8L_API int sq8l_eb_export_sysex(void* e, uint8_t* out, int cap) {
    return copyOut(static_cast<EditBox*>(e)->eb.exportSysex(), out, cap);
}
SQ8L_API void sq8l_eb_set_name(void* e, int which, const char* s, int len) {
    static_cast<EditBox*>(e)->eb.setName(which, std::string(s, size_t(len)));
}
SQ8L_API int sq8l_eb_name(void* e, int which, char* out, int cap) {
    const std::string s = static_cast<EditBox*>(e)->eb.name(which);
    const int n = static_cast<int>(s.size());
    std::memcpy(out, s.data(), size_t(n < cap ? n : cap));
    return n;
}
SQ8L_API void sq8l_eb_set_param(void* e, int i, int32_t v) { static_cast<EditBox*>(e)->eb.setParamValue(i, v); }
SQ8L_API int sq8l_eb_library_index(void* e) { return static_cast<EditBox*>(e)->eb.libraryIndex(); }

// ---------------------------------------------------------------- settings
SQ8L_API void sq8l_settings_parse(const char* text, int len, int32_t* gui6, int32_t* synth5) {
    Settings s;
    s.loadIni(std::string_view(text, size_t(len)));
    for (int i = 0; i < 6; i++) gui6[i] = s.gui[i];
    for (int i = 0; i < 5; i++) synth5[i] = s.synth[i];
}
SQ8L_API int sq8l_settings_modes(const int32_t* synth5, const uint8_t* prog, int32_t* out5) {
    Settings s;
    for (int i = 0; i < 5; i++) s.synth[i] = synth5[i];
    const Program p = toProgram(prog);
    out5[0] = s.softVoiceStealing(p);
    out5[1] = s.dca13Mode(p);
    out5[2] = s.dca4Mode(p);
    out5[3] = s.muffleMode(p);
    out5[4] = s.dcBlockMode(p);
    return 5;
}
