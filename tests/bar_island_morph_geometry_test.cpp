#include "shell/bar/bar_island_morph_geometry.h"
#include "shell/bar/bar_section_geometry.h"
#include "shell/panel/attached_panel_layout.h"
#include "shell/panel/attached_panel_morph.h"
#include "tests/test_check.h"

#include <cassert>
#include <cmath>

int main() {
  {
    const attached_panel::BodyRect bar{0,10,1920,34};
    const AttachedPanelSource media{.section=AttachedPanelSourceSection::Start,.x=873,.y=10,.width=34,.height=34};
    const AttachedPanelSource clock{.section=AttachedPanelSourceSection::Center,.x=918,.y=10,.width=84,.height=34};
    const AttachedPanelSource quick{.section=AttachedPanelSourceSection::End,.x=1013,.y=10,.width=34,.height=34};
    const auto place = [&](const AttachedPanelSource& source,int w,int h) {
      return attached_panel::fitOutwardIsland(AttachedRevealDirection::Down,bar,source,1920,1080,w,h,8,7);
    };
    const auto m=place(media,411,215), c=place(clock,256,116), q=place(quick,514,444);
    TEST_CHECK(m.x==489 && m.y==17 && m.width==411 && m.height==215);
    TEST_CHECK(c.x==832 && c.y==17 && c.width==256 && c.height==116);
    TEST_CHECK(q.x==1020 && q.y==17 && q.width==514 && q.height==444);
    const std::array<bar_island_morph::Extent,3> rest{{{873,907,true},{918,1002,true},{1013,1047,true}}};
    const auto openedMedia=bar_island_morph::reflow(rest,0,{489,900,true},1,1920,17);
    TEST_CHECK(openedMedia.extents[1]==rest[1] && openedMedia.extents[2]==rest[2]);
    const auto openedQuick=bar_island_morph::reflow(rest,2,{1020,1534,true},1,1920,17);
    TEST_CHECK(openedQuick.extents[0]==rest[0] && openedQuick.extents[1]==rest[1]);
    for (auto direction : {AttachedRevealDirection::Down,AttachedRevealDirection::Up,AttachedRevealDirection::Right,AttachedRevealDirection::Left}) {
      const auto tiny=attached_panel::fitOutwardIsland(direction,{0,0,1,1},media,1,1,900,600,8,7);
      TEST_CHECK(tiny.x==0 && tiny.y==0 && tiny.width==1 && tiny.height==1);
    }
  }

  {
    // Constrained start and end lanes retain the exact painted compact baseline
    // on both horizontal and vertical bars; raw content extends beyond its slot.
    const auto start = bar_sections::clippedExtent(20,80,20,140,true);
    const auto end = bar_sections::clippedExtent(200,260,140,260,true);
    TEST_CHECK(start.start==20 && start.end==80 && end.start==200 && end.end==260);
    const auto padded=bar_sections::paddedExtents({start,{},end},300,5,10);
    for (const bool vertical : {false,true}) {
      const auto rect=bar_sections::rectangle(padded[0],32,vertical);
      TEST_CHECK((vertical?rect.y:rect.x)==15);
      TEST_CHECK((vertical?rect.height:rect.width)==70);
    }
    std::array<bar_island_morph::Extent,3> baseline{};
    for (std::size_t i=0;i<3;++i) baseline[i]={padded[i].start,padded[i].end,padded[i].visible};
    for (const std::size_t active : {0U,2U}) {
      const auto initial=bar_island_morph::reflow(baseline,active,{0,300,true},0,300,10);
      TEST_CHECK(initial.extents[active].start==baseline[active].start);
      TEST_CHECK(initial.extents[active].end==baseline[active].end);
    }
    TEST_CHECK(!bar_sections::clippedExtent(20,20,0,80,true).visible);
  }
  {
    const AttachedPanelSource source{.section=AttachedPanelSourceSection::Center,
        .x=100,.y=4,.width=80,.height=32,.radii={4,8,12,16}};
    const auto body=attached_panel::includeIslandSource({60,36,200,140},source,400,300);
    TEST_CHECK(body.x==60 && body.y==4 && body.width==200 && body.height==172);
    const attached_panel::MorphRect compact{100,4,80,32},expanded{60,4,200,172};
    const auto first=attached_panel::islandMorphGeometry(compact,source.radii,expanded,20,0);
    TEST_CHECK(first.body.x==100 && first.body.y==4 && first.body.width==80 && first.body.height==32);
    TEST_CHECK(first.radii==source.radii);
    const auto middle=attached_panel::islandMorphGeometry(compact,source.radii,expanded,20,.5F);
    TEST_CHECK(middle.body.x==80 && middle.body.width==140 && middle.body.height==102);
    TEST_CHECK((middle.radii==Radii{12,14,16,18}));
    TEST_CHECK(middle.contentX==20 && middle.contentY==0);
    const auto last=attached_panel::islandMorphGeometry(compact,source.radii,expanded,20,1);
    TEST_CHECK(last.body.x==60 && last.body.width==200 && last.body.height==172);
    TEST_CHECK(last.radii==Radii{20});
    TEST_CHECK(last.background.width==last.body.width && last.background.height==last.body.height);
    const auto nearFirst=attached_panel::islandMorphGeometry(compact,source.radii,expanded,20,.00001F);
    TEST_CHECK(std::abs(nearFirst.body.width-first.body.width)<.01F);
    TEST_CHECK(std::abs(nearFirst.body.height-first.body.height)<.01F);
    TEST_CHECK(middle.contentClip.x>=middle.body.x && middle.contentClip.y>=middle.body.y);
    TEST_CHECK(middle.contentClip.width<=middle.body.width && middle.contentClip.height<=middle.body.height);
    const auto clamped=attached_panel::includeIslandSource({60,36,200,140},source,160,120);
    TEST_CHECK(clamped.x==60 && clamped.y==4 && clamped.width==100 && clamped.height==116);
  }
  using bar_island_morph::Extent;
  constexpr std::array baseline{
      Extent{100.0F, 180.0F, true}, Extent{260.0F, 340.0F, true}, Extent{420.0F, 500.0F, true}};

  const auto closed = bar_island_morph::reflow(baseline, 1, {150.0F, 450.0F, true}, 0.0F, 600.0F, 20.0F);
  assert(closed.extents == baseline);

  const auto calendar = bar_island_morph::reflow(baseline, 1, {150.0F, 450.0F, true}, 1.0F, 600.0F, 20.0F);
  assert(calendar.extents[1].start == 150.0F && calendar.extents[1].end == 450.0F);
  assert(calendar.extents[0].start == 50.0F && calendar.extents[0].end == 130.0F);
  assert(calendar.extents[2].start == 470.0F && calendar.extents[2].end == 550.0F);
  assert(calendar.fitsSurface);

  const auto media = bar_island_morph::reflow(baseline, 0, {40.0F, 330.0F, true}, 1.0F, 600.0F, 20.0F);
  assert(media.extents[0].start == 40.0F && media.extents[0].end == 330.0F);
  assert(media.extents[1].start == 350.0F && media.extents[1].end == 430.0F);
  assert(media.extents[2].start == 450.0F && media.extents[2].end == 530.0F);

  const auto status = bar_island_morph::reflow(baseline, 2, {270.0F, 570.0F, true}, 1.0F, 600.0F, 20.0F);
  assert(status.extents[2].start == 270.0F && status.extents[2].end == 570.0F);
  assert(status.extents[1].start == 170.0F && status.extents[1].end == 250.0F);
  assert(status.extents[0].start == 70.0F && status.extents[0].end == 150.0F);

  const auto half = bar_island_morph::reflow(baseline, 1, {150.0F, 450.0F, true}, 0.5F, 600.0F, 20.0F);
  assert(half.extents[1].start == 205.0F && half.extents[1].end == 395.0F);
  assert(half.extents[0] == baseline[0] && half.extents[2] == baseline[2]);

  const auto overflow = bar_island_morph::reflow(baseline, 2, {270.0F, 670.0F, true}, 1.0F, 600.0F, 20.0F);
  assert(!overflow.fitsSurface && overflow.overflowBefore == 0.0F && overflow.overflowAfter == 70.0F);
  // The active island stays aligned with the panel instead of being shifted into bounds.
  assert(overflow.extents[2].start == 270.0F && overflow.extents[2].end == 670.0F);

  const std::array hiddenMiddle{
      Extent{100.0F, 180.0F, true}, Extent{0.0F, 0.0F, false}, Extent{420.0F, 500.0F, true}};
  const auto hidden = bar_island_morph::reflow(hiddenMiddle, 0, {100.0F, 430.0F, true}, 1.0F, 600.0F, 20.0F);
  assert(!hidden.extents[1].visible && hidden.extents[2].start == 450.0F);
  const auto hiddenGap =
      bar_island_morph::reflow(hiddenMiddle, 0, hiddenMiddle[0], 0.01F, 700.0F, 300.0F);
  assert(!hiddenGap.extents[1].visible);
  assert(std::abs(hiddenGap.extents[2].start - 420.6F) < 0.001F);

  const auto reversed = bar_island_morph::reflow(baseline, 1, {450.0F, 150.0F, true}, 1.0F, 600.0F, 20.0F);
  assert(reversed.extents == calendar.extents);

  const auto invalid = bar_island_morph::reflow(baseline, 3, {0.0F, 600.0F, true}, 1.0F, 600.0F, 20.0F);
  assert(invalid.extents == baseline);

  const auto before = bar_island_morph::reflow(baseline, 0, {-70.0F, 330.0F, true}, 1.0F, 600.0F, 20.0F);
  assert(!before.fitsSurface && before.overflowBefore == 70.0F && before.overflowAfter == 0.0F);

  const auto narrower = bar_island_morph::reflow(baseline, 1, {280.0F, 320.0F, true}, 1.0F, 600.0F, 20.0F);
  assert(narrower.extents[1] == baseline[1]);

  // Runtime passes the final panel interval with the existing eased reveal
  // progress. Opening and closing therefore meet the compact source without a
  // first- or last-frame jump.
  const auto firstFrame =
      bar_island_morph::reflow(baseline, 1, {150.0F, 450.0F, true}, 0.0F, 600.0F, 20.0F);
  const auto closingFrame =
      bar_island_morph::reflow(baseline, 1, {150.0F, 450.0F, true}, 0.01F, 600.0F, 20.0F);
  assert(firstFrame.extents == baseline);
  assert(std::abs(closingFrame.extents[1].start - 258.9F) < 0.001F);
  assert(std::abs(closingFrame.extents[1].end - 341.1F) < 0.001F);

  // A configured gap larger than the compact gap grows on the same timeline;
  // it must not move siblings by the full difference on the first frame.
  const auto gapFirstFrame =
      bar_island_morph::reflow(baseline, 1, baseline[1], 0.0F, 600.0F, 100.0F);
  const auto gapEpsilon =
      bar_island_morph::reflow(baseline, 1, baseline[1], 0.01F, 600.0F, 100.0F);
  assert(gapFirstFrame.extents == baseline);
  assert(std::abs(gapEpsilon.extents[0].start - 99.8F) < 0.001F);
  assert(std::abs(gapEpsilon.extents[2].start - 420.2F) < 0.001F);

  using bar_island_morph::Sides;
  constexpr Sides bleed{7.0F, 11.0F, 13.0F, 17.0F};
  constexpr Sides inset{2.0F, 3.0F, 5.0F, 6.0F};
  const auto top = bar_island_morph::stableSurfaceMainInsets("top", 100.0F, bleed, inset);
  const auto bottom = bar_island_morph::stableSurfaceMainInsets("bottom", 100.0F, bleed, inset);
  const auto left = bar_island_morph::stableSurfaceMainInsets("left", 100.0F, bleed, inset);
  const auto right = bar_island_morph::stableSurfaceMainInsets("right", 100.0F, bleed, inset);
  assert(top == bottom && top.start == 100.0F && top.end == 100.0F);
  assert(left == right && left.start == 100.0F && left.end == 100.0F);
  const auto smallHorizontal = bar_island_morph::stableSurfaceMainInsets("top", 8.0F, bleed, inset);
  const auto smallVertical = bar_island_morph::stableSurfaceMainInsets("left", 8.0F, bleed, inset);
  assert(smallHorizontal.start == 9.0F && smallHorizontal.end == 13.0F);
  assert(smallVertical.start == 11.0F && smallVertical.end == 14.0F);
}
