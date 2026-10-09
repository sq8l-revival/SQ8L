#include "MtsEspTuning.h"

#include "libMTSClient.h"

namespace sq8l {

MtsEspTuning::MtsEspTuning() : client_(MTS_RegisterClient()) {}

MtsEspTuning::~MtsEspTuning() {
    if (client_ != nullptr) MTS_DeregisterClient(client_);
}

void MtsEspTuning::refresh(bool enabled, bool correctNativeOffsets) {
    tuning_.enabled = enabled;
    tuning_.correctNativeOffsets = correctNativeOffsets;
    if (!tuning_.enabled || client_ == nullptr || !MTS_HasMaster(client_)) {
        tuning_.connected = false;
        scaleName_.clear();
        return;
    }
    tuning_.connected = true;
    // Channel -1: the synth is omni, Master::midiNoteOn discards the channel.
    for (int k = 0; k < 128; k++)
        tuning_.ratio[k] = MTS_RetuningAsRatio(client_, static_cast<char>(k), -1);
    for (int w = 0; w < 4; w++) tuning_.filtered[w] = 0;
    for (int k = 0; k < 128; k++) {
        if (MTS_ShouldFilterNote(client_, static_cast<char>(k), -1))
            tuning_.filtered[k >> 5] |= 1u << (k & 31);
    }
    const char* name = MTS_GetScaleName(client_);
    scaleName_ = name != nullptr ? name : "";
}

}  // namespace sq8l
