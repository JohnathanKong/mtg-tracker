#include "mtg_ui_geometry.h"
#include <cassert>
int main() {
    using namespace mtg_ui;
    assert(safelyInside(kOptionalLeft));
    assert(safelyInside(kOptionalRight));
    assert(safelyInside(kEditButton));
    assert(!safelyInside({10,275,120,42}));
    assert(kOptionalLeft.x+kOptionalLeft.w < kOptionalRight.x);
    assert(kOptionalRight.x+kOptionalRight.w < kEditButton.x);
}
