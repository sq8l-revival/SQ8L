// MTS-ESP client: fills a sq8l::Tuning snapshot for the engine (port addition).
//
// The engine never calls MTS-ESP itself; this refreshes one snapshot per audio block so the
// MIDI and voice paths only read arrays, and so sq8l_engine carries no dependency on the
// client library. See docs/modules/tuning.md.
#pragma once

#include <string>

#include "Tuning.h"

struct MTSClient;

namespace sq8l {

class MtsEspTuning {
public:
    MtsEspTuning();
    ~MtsEspTuning();
    MtsEspTuning(const MtsEspTuning&) = delete;
    MtsEspTuning& operator=(const MtsEspTuning&) = delete;

    // Call once per audio block, before the MIDI events are processed. The two switches are
    // per instance (Synth::mtsEnabled / mtsCorrectPitch), not settings, so they are passed in.
    // While the feature is off this only clears the snapshot, so it costs nothing.
    void refresh(bool enabled, bool correctNativeOffsets);

    const Tuning& tuning() const { return tuning_; }
    // The master's scale name, empty when none is connected (for the editor).
    const std::string& scaleName() const { return scaleName_; }

private:
    MTSClient* client_ = nullptr;
    Tuning tuning_;
    std::string scaleName_;
};

}  // namespace sq8l
