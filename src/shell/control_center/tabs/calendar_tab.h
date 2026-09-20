#pragma once

#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "shell/control_center/calendar_month_open_intent.h"
#include "shell/control_center/tab.h"
#include "ui/controls/calendar_view.h"

#include <cstdint>
#include <limits>

class Button;
class CalendarService;
class ConfigService;
class InputArea;
class Label;
class ScrollView;

class CalendarTab : public Tab {
public:
  explicit CalendarTab(ConfigService* config = nullptr, CalendarService* calendar = nullptr);
  ~CalendarTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setForceMonthView(bool force) noexcept { m_forceMonthView = force; }
  void setForceWeekStrip(bool force) noexcept { m_forceWeekStrip = force; }
  void setActive(bool active) override;
  void onClose() override;

private:
  std::unique_ptr<Flex> createWeekStrip();
  void rebuildWeekStrip();
  bool m_weekStrip = false;
  bool m_forceMonthView = false;
  bool m_forceWeekStrip = false;
  CalendarMonthOpenIntent m_monthOpenIntent;
  int m_weekOffsetDays = 0;
  void focusToday();
  void changeMonthBy(int delta);
  void cancelMonthSlide();
  void beginSlideOut(int delta);
  void beginSlideIn();
  void applyMonthSlide(float progress, bool slidingIn);
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void rebuild();
  void rebuildEventList(float scale);
  void toggleEventsCard();

  ConfigService* m_config = nullptr;
  CalendarService* m_calendar = nullptr;
  std::uint64_t m_calendarCallbackId = 0;
  bool m_eventsDirty = false;
  Flex* m_rootLayout = nullptr;
  InputArea* m_weekStripArea = nullptr;
  Button* m_toggleEventsCardButton = nullptr;
  InputArea* m_calendarArea = nullptr;
  Flex* m_card = nullptr;
  Flex* m_header = nullptr;
  Flex* m_previousSlot = nullptr;
  Flex* m_nextSlot = nullptr;
  Flex* m_monthWrap = nullptr;
  Label* m_todayLabel = nullptr;
  Label* m_monthLabel = nullptr;
  Button* m_previousButton = nullptr;
  Button* m_nextButton = nullptr;
  Flex* m_gridViewport = nullptr;
  Flex* m_grid = nullptr;
  Flex* m_eventsCard = nullptr;
  Label* m_eventsTitle = nullptr;
  ScrollView* m_eventsScroll = nullptr;
  calendar_view::EventListState m_eventListState;
  int m_selectedYear = std::numeric_limits<int>::min();
  int m_selectedMonth = -1;
  int m_selectedDay = -1;
  int m_monthOffset = 0;
  float m_lastInnerWidth = -1.0F;
  float m_lastInnerHeight = -1.0F;
  int m_lastDisplayYear = std::numeric_limits<int>::min();
  int m_lastDisplayMonth = -1;
  int m_lastCurrentYear = std::numeric_limits<int>::min();
  int m_lastCurrentMonth = -1;
  int m_lastToday = -1;
  int m_monthSlideDirection = 0;
  int m_pendingMonthDelta = 0;
  bool m_startMonthSlideIn = false;
  AnimationManager::Id m_monthSlideAnimId = 0;
  bool m_showEventsCard = true;
  bool m_showWeekNumbers = false;
  Timer m_eventFadeTimer;
};
