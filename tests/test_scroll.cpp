#include "mtg_scroll.h"
#include <cassert>

int main() {
    using namespace mtg_scroll;
    assert(maxScroll(0) == 0);
    assert(maxScroll(4) == 0);
    assert(maxScroll(5) == 46);
    assert(maxScroll(17) == 598);
    assert(clampScroll(-50, 8) == 0);
    assert(clampScroll(100000, 8) == maxScroll(8));
    assert(firstRow(77) == 1);
    assert(rowOffset(77) == 31);
    assert(tappedIndex(110, 0, 17) == 0);
    assert(tappedIndex(156, 0, 17) == 1);
    assert(tappedIndex(110, 31, 17) == 0);
    assert(tappedIndex(130, 31, 17) == 1);
    assert(tappedIndex(293, 31, 17) == 4);
    assert(tappedIndex(109, 31, 17) == -1);
    assert(tappedIndex(294, 31, 17) == -1);
    assert(tappedIndex(120, 0, 0) == -1);
}
