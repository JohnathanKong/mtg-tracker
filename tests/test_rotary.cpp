#include "mtg_rotary.h"

#include <cassert>
#include <cstdio>

int main() {
    MtgRotaryPulse p;

    // No change on initial LOW: must await the end of the detent pulse.
    assert(!p.sample(true));
    assert(!p.sample(true));
    assert(!p.sample(true));
    assert(!p.sample(false));
    assert(p.sample(false));
    assert(!p.sample(false));

    // Bounce: one-sample spikes do not register.
    assert(!p.sample(true));
    assert(!p.sample(false));
    assert(!p.sample(false));

    // A full second detent causes one, and only one, update.
    assert(!p.sample(true));
    assert(!p.sample(true));
    assert(!p.sample(false));
    assert(p.sample(false));
    assert(!p.sample(false));

    // A brief release bounce while pressed cannot double count.
    assert(!p.sample(true));
    assert(!p.sample(true));
    assert(!p.sample(false));
    assert(!p.sample(true));
    assert(!p.sample(true));
    assert(!p.sample(false));
    assert(p.sample(false));
    assert(!p.sample(false));

    std::puts("Rotary click synchronization tests passed");
}
