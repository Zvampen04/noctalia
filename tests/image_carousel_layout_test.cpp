#include <cassert>
#include <cmath>

#include "ui/controls/image_carousel_layout.h"

int main() {
    const float viewportWidth = 1920.0f;
    const float expandedWidth = 768.0f;
    const float expandedHeight = 475.0f;
    const float sliceWidth = 108.0f;
    const float sliceHeight = 432.0f;
    const float spacing = -30.0f;

    auto selected = ui::carouselSlice(5, 5, viewportWidth, expandedWidth, expandedHeight, sliceWidth, sliceHeight, spacing);
    assert(std::fabs(selected.x - 576.0f) < 0.0001f);
    assert(std::fabs(selected.y - 0.0f) < 0.0001f);
    assert(std::fabs(selected.width - expandedWidth) < 0.0001f);
    assert(std::fabs(selected.height - expandedHeight) < 0.0001f);
    assert(selected.z == 100);

    auto previous = ui::carouselSlice(4, 5, viewportWidth, expandedWidth, expandedHeight, sliceWidth, sliceHeight, spacing);
    auto next = ui::carouselSlice(6, 5, viewportWidth, expandedWidth, expandedHeight, sliceWidth, sliceHeight, spacing);
    assert(std::fabs(previous.x - 498.0f) < 0.0001f);
    assert(std::fabs(next.x - 1314.0f) < 0.0001f);
    assert(std::fabs(previous.y - 21.5f) < 0.0001f);
    assert(std::fabs(previous.height - sliceHeight) < 0.0001f);
    assert(previous.z == 49);

    auto narrow = ui::carouselSlice(5, 5, 600.0f, 768.0f, 475.0f, 108.0f, 432.0f, spacing);
    assert(narrow.width == 384.0f);
    assert(std::fabs(narrow.x - 108.0f) < 0.0001f);

    assert(ui::carouselContains(0.0f, 0.0f, 100.0f, 100.0f, 28.0f) == false);
    assert(ui::carouselContains(28.0f, 0.0f, 100.0f, 100.0f, 28.0f) == true);
    assert(ui::carouselContains(0.0f, 100.0f, 100.0f, 100.0f, 28.0f) == true);
    assert(ui::carouselContains(90.0f, 100.0f, 100.0f, 100.0f, 28.0f) == false);
    assert(ui::carouselContains(0.0f, 100.0f, 100.0f, 100.0f, -28.0f) == false);
    assert(ui::carouselContains(100.0f, 0.0f, 100.0f, 100.0f, -28.0f) == false);

    assert(ui::carouselContains(28.0f, 50.0f, 100.0f, 100.0f, -28.0f) == true);
    assert(ui::carouselContains(72.0f, 0.0f, 100.0f, 100.0f, -28.0f) == true);

    assert(!ui::carouselContains(5.0f, 5.0f, 0.0f, 0.0f, 2.0f));
    assert(!ui::carouselContains(50.0f, 50.0f, 0.5f, 100.0f, 2.0f));

    auto extremeStep = ui::carouselSlice(1, 0, 1920.0f, 768.0f, expandedHeight, sliceWidth, sliceHeight, -1000.0f);
    auto extremeSelected = ui::carouselSlice(0, 0, 1920.0f, 768.0f, expandedHeight, sliceWidth, sliceHeight, -1000.0f);
    assert(extremeStep.x == extremeSelected.x + 768.0f - 108.0f + 1.0f);

    int selectedIndex = 8;
    auto shiftMinus = ui::carouselSlice(selectedIndex - 1, selectedIndex, viewportWidth, 768.0f, 475.0f, 108.0f, 432.0f, -30.0f);
    auto shiftPlus = ui::carouselSlice(selectedIndex + 1, selectedIndex, viewportWidth, 768.0f, 475.0f, 108.0f, 432.0f, -30.0f);
    assert(std::fabs(shiftPlus.x - next.x) < 0.0001f);
    assert(std::fabs(shiftMinus.x - previous.x) < 0.0001f);

    return 0;
}
