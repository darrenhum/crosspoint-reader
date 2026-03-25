#include "LockscreenCalendarActivity.h"

#include <CalendarSyncManager.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"

extern HalPowerManager powerManager;

void LockscreenCalendarActivity::onEnter() {
  Activity::onEnter();
  initToday();

  displayMonth = todayMonth;
  displayYear = todayYear;
  selectedDay = todayDay;

  // Load persisted calendar data
  calendar::CalendarStore::load(calendarData);

  requestUpdate();
}

void LockscreenCalendarActivity::onExit() {
  // Clean up sync task if still running
  if (syncTaskHandle) {
    vTaskDelete(syncTaskHandle);
    syncTaskHandle = nullptr;
  }
  syncInProgress = false;
  Activity::onExit();
}

void LockscreenCalendarActivity::initToday() {
  time_t now;
  time(&now);
  struct tm tmNow;
  localtime_r(&now, &tmNow);

  if (tmNow.tm_year > 100) {  // Valid time (after 2000)
    todayYear = tmNow.tm_year + 1900;
    todayMonth = tmNow.tm_mon + 1;
    todayDay = tmNow.tm_mday;
  } else {
    // Fallback if RTC not set
    todayYear = 2025;
    todayMonth = 1;
    todayDay = 1;
  }
  todayDays = calendar::dateToDays(todayYear, todayMonth, todayDay);
}

void LockscreenCalendarActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    goToPreviousMonth();
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    goToNextMonth();
    requestUpdate();
    return;
  }

  buttonNavigator.onPreviousRelease([this] {
    // Move selected day back by 7 (one week)
    selectedDay -= 7;
    if (selectedDay < 1) {
      goToPreviousMonth();
      int dim = calendar::daysInMonth(displayYear, displayMonth);
      selectedDay = dim + selectedDay;  // selectedDay is negative offset
      if (selectedDay < 1) selectedDay = 1;
    }
    requestUpdate();
  });

  buttonNavigator.onNextRelease([this] {
    // Move selected day forward by 7 (one week)
    int dim = calendar::daysInMonth(displayYear, displayMonth);
    selectedDay += 7;
    if (selectedDay > dim) {
      selectedDay = selectedDay - dim;
      goToNextMonth();
      dim = calendar::daysInMonth(displayYear, displayMonth);
      if (selectedDay > dim) selectedDay = dim;
    }
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && !syncInProgress) {
    // Manual sync trigger
    syncInProgress = true;
    requestUpdate();

    xTaskCreate(&syncTaskTrampoline, "CalSync", 4096, this, 1, &syncTaskHandle);
  }
}

void LockscreenCalendarActivity::syncTaskTrampoline(void* param) {
  auto* self = static_cast<LockscreenCalendarActivity*>(param);
  self->syncTask();
  self->syncTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

void LockscreenCalendarActivity::syncTask() {
  uint8_t batteryPct = powerManager.getBatteryPercentage();

  time_t now;
  time(&now);
  uint32_t currentEpoch = static_cast<uint32_t>(now);

  auto result = calendar::CalendarSyncManager::sync(calendarData, batteryPct, currentEpoch);

  switch (result) {
    case calendar::CalendarSyncManager::SyncResult::OK_UPDATED:
      LOG_INF("CAL", "Calendar sync: data updated");
      break;
    case calendar::CalendarSyncManager::SyncResult::OK_NOT_MODIFIED:
      LOG_INF("CAL", "Calendar sync: not modified");
      break;
    case calendar::CalendarSyncManager::SyncResult::FAILED:
      LOG_ERR("CAL", "Calendar sync failed");
      break;
    default:
      LOG_DBG("CAL", "Calendar sync skipped");
      break;
  }

  syncInProgress = false;
  requestUpdate();
}

void LockscreenCalendarActivity::goToPreviousMonth() {
  displayMonth--;
  if (displayMonth < 1) {
    displayMonth = 12;
    displayYear--;
  }
  int dim = calendar::daysInMonth(displayYear, displayMonth);
  if (selectedDay > dim) selectedDay = dim;
}

void LockscreenCalendarActivity::goToNextMonth() {
  displayMonth++;
  if (displayMonth > 12) {
    displayMonth = 1;
    displayYear++;
  }
  int dim = calendar::daysInMonth(displayYear, displayMonth);
  if (selectedDay > dim) selectedDay = dim;
}

bool LockscreenCalendarActivity::dayHasEvent(uint16_t daysSince2000) const {
  for (uint8_t i = 0; i < calendarData.eventCount; i++) {
    const auto& evt = calendarData.events[i];
    if (daysSince2000 >= evt.startDay && daysSince2000 < evt.endDay) {
      return true;
    }
  }
  return false;
}

int LockscreenCalendarActivity::getEventsForDay(uint16_t daysSince2000, const calendar::CalendarEvent** outEvents,
                                                 int maxOut) const {
  int count = 0;
  for (uint8_t i = 0; i < calendarData.eventCount && count < maxOut; i++) {
    const auto& evt = calendarData.events[i];
    if (daysSince2000 >= evt.startDay && daysSince2000 < evt.endDay) {
      outEvents[count++] = &evt;
    }
  }
  return count;
}

void LockscreenCalendarActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = GUI.getMetrics();

  // --- Header: Month Year ---
  char headerBuf[64];
  static constexpr const char* MONTH_NAMES[] = {"January",   "February", "March",    "April",
                                                 "May",       "June",     "July",     "August",
                                                 "September", "October",  "November", "December"};
  const char* monthName = (displayMonth >= 1 && displayMonth <= 12) ? MONTH_NAMES[displayMonth - 1] : "?";
  snprintf(headerBuf, sizeof(headerBuf), "%s %d", monthName, displayYear);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerBuf);

  // --- Calendar grid area ---
  int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  int footerHeight = renderer.getLineHeight(UI_10_FONT_ID) + 8;
  int buttonHintsY = pageHeight - metrics.buttonHintsHeight;
  int contentBottom = buttonHintsY - metrics.verticalSpacing - footerHeight;
  int contentHeight = contentBottom - contentTop;

  // Split into calendar grid (left) and event list (right)
  int sidePadding = metrics.contentSidePadding;
  int availWidth = pageWidth - 2 * sidePadding;

  // Calendar grid takes ~60% of width, event list takes ~40%
  int gridWidth = (availWidth * 60) / 100;
  int listWidth = availWidth - gridWidth - sidePadding;

  drawCalendarGrid(sidePadding, contentTop, gridWidth, contentHeight);
  drawEventList(sidePadding + gridWidth + sidePadding, contentTop, listWidth, contentHeight);

  // --- Footer: Last Refreshed ---
  drawLastRefreshedFooter(sidePadding, contentBottom + 2, availWidth);

  // --- Button hints ---
  const char* syncLabel = syncInProgress ? tr(STR_CALENDAR_SYNCING) : tr(STR_CALENDAR_SYNC);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), syncLabel, "< >", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void LockscreenCalendarActivity::drawCalendarGrid(int contentX, int contentY, int contentWidth,
                                                   int contentHeight) const {
  const int cols = 7;
  int cellWidth = contentWidth / cols;
  int rowHeight = renderer.getLineHeight(UI_10_FONT_ID) + 6;

  // Day of week headers
  uint8_t weekStart = SETTINGS.calendarWeekStart;
  static constexpr const char* DAY_HEADERS_SUN[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
  static constexpr const char* DAY_HEADERS_MON[] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
  const char* const* dayHeaders = (weekStart == CrossPointSettings::WEEK_MONDAY) ? DAY_HEADERS_MON : DAY_HEADERS_SUN;

  int headerY = contentY;
  for (int col = 0; col < cols; col++) {
    int x = contentX + col * cellWidth + cellWidth / 2;
    int textW = renderer.getTextWidth(UI_10_FONT_ID, dayHeaders[col]);
    renderer.drawText(UI_10_FONT_ID, x - textW / 2, headerY, dayHeaders[col], true, EpdFontFamily::BOLD);
  }

  // Separator line below day headers
  int sepY = headerY + rowHeight - 2;
  renderer.drawLine(contentX, sepY, contentX + contentWidth, sepY, true);

  // Calculate first day position
  int dow = calendar::dayOfWeek(displayYear, displayMonth, 1);  // 0=Sunday
  int startCol = (weekStart == CrossPointSettings::WEEK_MONDAY) ? ((dow + 6) % 7) : dow;

  int dim = calendar::daysInMonth(displayYear, displayMonth);
  int gridY = sepY + 4;

  for (int day = 1; day <= dim; day++) {
    int dayIndex = startCol + day - 1;
    int row = dayIndex / cols;
    int col = dayIndex % cols;

    int cellX = contentX + col * cellWidth;
    int cellY = gridY + row * rowHeight;

    // Check if this row fits
    if (cellY + rowHeight > contentY + contentHeight) break;

    char dayStr[4];
    snprintf(dayStr, sizeof(dayStr), "%d", day);
    int textW = renderer.getTextWidth(UI_10_FONT_ID, dayStr);
    int textX = cellX + cellWidth / 2 - textW / 2;

    // Highlight today
    bool isToday = (displayYear == todayYear && displayMonth == todayMonth && day == todayDay);
    bool isSelected = (day == selectedDay);

    if (isToday) {
      // Draw filled circle/rect behind today's date
      int boxSize = rowHeight - 2;
      int boxX = cellX + cellWidth / 2 - boxSize / 2;
      renderer.fillRect(boxX, cellY, boxSize, boxSize, true);
      renderer.drawText(UI_10_FONT_ID, textX, cellY + 1, dayStr, false);  // White text on black
    } else if (isSelected) {
      // Draw outline for selected day
      int boxSize = rowHeight - 2;
      int boxX = cellX + cellWidth / 2 - boxSize / 2;
      renderer.drawRect(boxX, cellY, boxSize, boxSize, true);
      renderer.drawText(UI_10_FONT_ID, textX, cellY + 1, dayStr, true);
    } else {
      renderer.drawText(UI_10_FONT_ID, textX, cellY + 1, dayStr, true);
    }

    // Event dot indicator below the day number
    uint16_t dayDays = calendar::dateToDays(displayYear, displayMonth, day);
    if (dayHasEvent(dayDays)) {
      int dotX = cellX + cellWidth / 2;
      int dotY = cellY + rowHeight - 4;
      // Small filled circle as event indicator
      renderer.fillRect(dotX - 1, dotY - 1, 3, 3, !isToday);
    }
  }
}

void LockscreenCalendarActivity::drawEventList(int listX, int listY, int listWidth, int listHeight) const {
  // Show events for the selected day
  uint16_t selectedDays = calendar::dateToDays(displayYear, displayMonth, selectedDay);

  const calendar::CalendarEvent* dayEvents[8];
  int eventCount = getEventsForDay(selectedDays, dayEvents, 8);

  int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int y = listY;

  // Draw selected date as sub-header
  char dateBuf[32];
  snprintf(dateBuf, sizeof(dateBuf), "%d/%d/%d", displayMonth, selectedDay, displayYear);
  renderer.drawText(UI_10_FONT_ID, listX, y, dateBuf, true, EpdFontFamily::BOLD);
  y += lineHeight + 4;

  // Separator
  renderer.drawLine(listX, y, listX + listWidth, y, true);
  y += 4;

  if (eventCount == 0) {
    renderer.drawText(UI_10_FONT_ID, listX, y, tr(STR_CALENDAR_NO_EVENTS), true);
    return;
  }

  for (int i = 0; i < eventCount && y + lineHeight < listY + listHeight; i++) {
    const auto* evt = dayEvents[i];
    // Bullet point + truncated summary
    char eventLine[64];
    snprintf(eventLine, sizeof(eventLine), "\xE2\x80\xA2 %s", evt->summary);  // UTF-8 bullet

    // Truncate to fit width
    std::string truncated = renderer.truncatedText(UI_10_FONT_ID, eventLine, listWidth);
    renderer.drawText(UI_10_FONT_ID, listX, y, truncated.c_str(), true);
    y += lineHeight + 2;
  }
}

void LockscreenCalendarActivity::drawLastRefreshedFooter(int footerX, int footerY, int footerWidth) const {
  char footerBuf[64];

  if (calendarData.lastSyncEpoch == 0) {
    snprintf(footerBuf, sizeof(footerBuf), tr(STR_LAST_REFRESHED), tr(STR_NEVER_SYNCED));
  } else {
    time_t syncTime = static_cast<time_t>(calendarData.lastSyncEpoch);
    struct tm syncTm;
    localtime_r(&syncTime, &syncTm);

    char timeBuf[20];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", &syncTm);
    snprintf(footerBuf, sizeof(footerBuf), tr(STR_LAST_REFRESHED), timeBuf);
  }

  // Draw centered footer
  int textW = renderer.getTextWidth(SMALL_FONT_ID, footerBuf);
  int textX = footerX + (footerWidth - textW) / 2;
  renderer.drawText(SMALL_FONT_ID, textX, footerY, footerBuf, true);
}
