// C API: the MTS-ESP retune math (sq8l::computeRetune) for tests/test_tuning.py.
#include <cstdint>

#include "Tuning.h"

#include "capi_export.h"

SQ8L_API void sq8l_tuning_compute_retune(int32_t enabled, int32_t absolute, int32_t connected,
                                         const double* ratio, int32_t key, int32_t pBase,
                                         uint32_t nominalPhaseInc, int32_t* pitchOffsetOut,
                                         double* clockRatioOut) {
    sq8l::Tuning t;
    t.enabled = enabled != 0;
    t.correctNativeOffsets = absolute != 0;
    t.connected = connected != 0;
    for (int i = 0; i < 128; i++) t.ratio[i] = ratio[i];
    const sq8l::VoiceRetune r = sq8l::computeRetune(t, key, pBase, nominalPhaseInc);
    *pitchOffsetOut = r.pitchOffset;
    *clockRatioOut = r.clockRatio;
}
