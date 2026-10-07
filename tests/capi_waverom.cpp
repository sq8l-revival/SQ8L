// C API: wave ROM lookups.
#include <cstdint>

#include "WaveRom.h"

#include "capi_export.h"

SQ8L_API const uint8_t* sq8l_waverom_data() { return sq8l::data::kWaveRom; }

SQ8L_API void sq8l_waverom_sample(uint32_t wave, int32_t key, uint8_t* page, uint8_t* waveReg) {
    const sq8l::WaveSample ws = sq8l::waveSample(wave, key);
    *page = ws.page;
    *waveReg = ws.waveReg;
}

SQ8L_API uint32_t sq8l_waverom_lfo_location(int32_t key, uint32_t wave, int32_t* sizeLog2) {
    const sq8l::WaveLocation loc = sq8l::lfoWaveLocation(key, wave);
    *sizeLog2 = loc.sizeLog2;
    return static_cast<uint32_t>(loc.data - sq8l::data::kWaveRom);
}
