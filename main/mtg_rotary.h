#pragma once
#include <cstdint>

// GPIO8 (clockwise) and GPIO7 (counterclockwise) emit independent active-low
// pulses on this Waveshare knob. The mechanical detent is reached around the
// end of the pulse, not necessarily when the pin first goes low.
//
// Require two LOW samples to qualify an actual movement, then emit precisely
// one step only after two consecutive HIGH samples (pulse completion).
// At the existing 2 ms sampling cadence this adds ~4 ms after release,
// instead of changing the value at the first electrical contact.
struct MtgRotaryPulse {
    uint8_t lowSamples = 0;
    uint8_t highSamples = 2;
    bool ready = true;
    bool pending = false;

    bool sample(bool isLow) {
        if (isLow) {
            highSamples = 0;
            if (lowSamples < 2) ++lowSamples;
            if (ready && lowSamples >= 2) {
                ready = false;
                pending = true;
            }
            return false;
        }

        lowSamples = 0;
        if (highSamples < 2) ++highSamples;
        if (highSamples >= 2) {
            ready = true;
            if (pending) {
                pending = false;
                return true;
            }
        }
        return false;
    }
};
