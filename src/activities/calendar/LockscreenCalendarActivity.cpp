#include "LockscreenCalendarActivity.h"

#include <CalendarSyncManager.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"
#include "fontIds.h"

extern HalPowerManager powerManager;

/// Stack size for the CalSync FreeRTOS task (bytes).
/// IcsParser ~360B + StreamContext ~80B + HTTP config ~200B + call frames.
/// tempEvents are heap-allocated in sync() to avoid stack pressure.
static constexpr uint32_t SYNC_TASK_STACK_SIZE = 8192;

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
  // Free heap-allocated sync buffer if sync was in progress
  if (syncResultData) {
    free(syncResultData);
    syncResultData = nullptr;
  }
  syncInProgress = false;
  syncComplete = false;
  Activity::onExit();
}

void LockscreenCalendarActivity::initToday() {
  time_t now;
  time(&now);
  struct tm tmNow;
  localtime_r(&now, &tmNow);

  // tm_year is years since 1900; 100 corresponds to year 2000
  if (tmNow.tm_year > 100) {
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
  // Check if sync task completed - consume result on main task (thread-safe handoff).
  // Acquire RenderLock so the render task cannot read calendarData/syncInProgress during the swap.
  if (syncComplete) {
    {
      RenderLock lock(*this);
      if (syncResultData) {
        calendarData = *syncResultData;
        free(syncResultData);
        syncResultData = nullptr;
      }
      syncInProgress = false;
    }
    // Delete the suspended sync task (it suspended itself after setting syncComplete)
    if (syncTaskHandle) {
      vTaskDelete(syncTaskHandle);
      syncTaskHandle = nullptr;
    }
    syncComplete = false;
    requestUpdate();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    {
      RenderLock lock(*this);
      goToPreviousMonth();
    }
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    {
      RenderLock lock(*this);
      goToNextMonth();
    }
    requestUpdate();
    return;
  }

  buttonNavigator.onPreviousRelease([this] {
    {
      RenderLock lock(*this);
      // Move selected day back by 7 (one week)
      selectedDay -= 7;
      if (selectedDay < 1) {
        goToPreviousMonth();
        int dim = calendar::daysInMonth(displayYear, displayMonth);
        selectedDay = dim + selectedDay;  // selectedDay is negative offset
        if (selectedDay < 1) selectedDay = 1;
      }
    }
    requestUpdate();
  });

  buttonNavigator.onNextRelease([this] {
    {
      RenderLock lock(*this);
      // Move selected day forward by 7 (one week)
      int dim = calendar::daysInMonth(displayYear, displayMonth);
      selectedDay += 7;
      if (selectedDay > dim) {
        selectedDay = selectedDay - dim;
        goToNextMonth();
        dim = calendar::daysInMonth(displayYear, displayMonth);
        if (selectedDay > dim) selectedDay = dim;
      }
    }
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && !syncInProgress) {
    // Heap-allocate sync buffer (~3.5KB) only for the duration of the sync
    syncResultData = static_cast<calendar::CalendarData*>(malloc(sizeof(calendar::CalendarData)));
    if (!syncResultData) {
      LOG_ERR("CAL", "malloc failed for sync buffer: %zu bytes", sizeof(calendar::CalendarData));
      return;
    }
    {
      RenderLock lock(*this);
      // Copy current data so sync task has feed metadata for conditional requests
      *syncResultData = calendarData;
      syncInProgress = true;
    }
    syncComplete = false;
    requestUpdate();

    xTaskCreate(&syncTaskTrampoline, "CalSync", SYNC_TASK_STACK_SIZE, this, 1, &syncTaskHandle);
  }
}

void LockscreenCalendarActivity::syncTaskTrampoline(void* param) {
  auto* self = static_cast<LockscreenCalendarActivity*>(param);
  self->syncTask();
  // Signal main task that sync is complete.
  // Do NOT self-delete here: the main task owns syncTaskHandle and will clean up
  // in loop() or onExit(). Self-deleting would leave syncTaskHandle dangling,
  // causing a double-free if onExit() runs before loop() processes the flag.
  self->syncComplete = true;
  vTaskSuspend(nullptr);  // Suspend instead of delete; main task will vTaskDelete
}

void LockscreenCalendarActivity::syncTask() {
  uint8_t batteryPct = powerManager.getBatteryPercentage();

  time_t now;
  time(&now);
  uint32_t currentEpoch = static_cast<uint32_t>(now);

  auto result = calendar::CalendarSyncManager::sync(*syncResultData, batteryPct, currentEpoch);

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
  static constexpr StrId MONTH_STR_IDS[] = {
      StrId::STR_MONTH_JAN, StrId::STR_MONTH_FEB, StrId::STR_MONTH_MAR, StrId::STR_MONTH_APR,
      StrId::STR_MONTH_MAY, StrId::STR_MONTH_JUN, StrId::STR_MONTH_JUL, StrId::STR_MONTH_AUG,
      StrId::STR_MONTH_SEP, StrId::STR_MONTH_OCT, StrId::STR_MONTH_NOV, StrId::STR_MONTH_DEC,
  };
  const char* monthName =
      (displayMonth >= 1 && displayMonth <= 12) ? I18N.get(MONTH_STR_IDS[displayMonth - 1]) : "?";
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

  // Day of week headers (internationalized)
  uint8_t weekStart = SETTINGS.calendarWeekStart;
  // Sunday-start order: Su Mo Tu We Th Fr Sa
  static constexpr StrId DAY_STR_IDS_SUN[] = {StrId::STR_DAY_SU, StrId::STR_DAY_MO, StrId::STR_DAY_TU,
                                               StrId::STR_DAY_WE, StrId::STR_DAY_TH, StrId::STR_DAY_FR,
                                               StrId::STR_DAY_SA};
  // Monday-start order: Mo Tu We Th Fr Sa Su
  static constexpr StrId DAY_STR_IDS_MON[] = {StrId::STR_DAY_MO, StrId::STR_DAY_TU, StrId::STR_DAY_WE,
                                               StrId::STR_DAY_TH, StrId::STR_DAY_FR, StrId::STR_DAY_SA,
                                               StrId::STR_DAY_SU};
  const StrId* dayStrIds =
      (weekStart == CrossPointSettings::WEEK_MONDAY) ? DAY_STR_IDS_MON : DAY_STR_IDS_SUN;

  int headerY = contentY;
  for (int col = 0; col < cols; col++) {
    int x = contentX + col * cellWidth + cellWidth / 2;
    const char* dayLabel = I18N.get(dayStrIds[col]);
    int textW = renderer.getTextWidth(UI_10_FONT_ID, dayLabel);
    renderer.drawText(UI_10_FONT_ID, x - textW / 2, headerY, dayLabel, true, EpdFontFamily::BOLD);
  }

  // Separator line below day headers
  int sepY = headerY + rowHeight - 2;
  renderer.drawLine(contentX, sepY, contentX + contentWidth, sepY, true);

  // Calculate first day position
  int dow = calendar::dayOfWeek(displayYear, displayMonth, 1);  // 0=Sunday
  int startCol = (weekStart == CrossPointSettings::WEEK_MONDAY) ? ((dow + 6) % 7) : dow;

  int dim = calendar::daysInMonth(displayYear, displayMonth);
  int gridY = sepY + 4;

  // Precompute event-day flags for this month to avoid O(events) per cell.
  // Max bit index is (dim-1) ≤ 30 (max 31 days), well within uint32_t range.
  // uint32_t is preferred over uint64_t: ESP32-C3 is 32-bit RISC-V, so 32-bit
  // shifts are single instructions vs multi-instruction 64-bit shifts.
  uint16_t monthStartDays = calendar::dateToDays(displayYear, displayMonth, 1);
  uint32_t eventDayBits = 0;  // Bit i set => day (i+1) has an event
  for (uint8_t ei = 0; ei < calendarData.eventCount; ei++) {
    const auto& evt = calendarData.events[ei];
    // Event spans [startDay, endDay). Compute overlap with [monthStartDays, monthStartDays+dim).
    int startOffset = (evt.startDay > monthStartDays) ? static_cast<int>(evt.startDay - monthStartDays) : 0;
    int endOffset = (evt.endDay > monthStartDays) ? static_cast<int>(evt.endDay - monthStartDays) : 0;
    if (endOffset > dim) endOffset = dim;
    for (int d = startOffset; d < endOffset; d++) {
      eventDayBits |= (1U << d);
    }
  }

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

    // Event dot indicator below the day number (use precomputed bitset)
    if (eventDayBits & (1U << (day - 1))) {
      int dotX = cellX + cellWidth / 2;
      int dotY = cellY + rowHeight - 4;
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
    // Bullet point + summary (stack buffer, no heap allocation)
    char eventLine[64];
    snprintf(eventLine, sizeof(eventLine), "\xE2\x80\xA2 %s", evt->summary);

    renderer.drawText(UI_10_FONT_ID, listX, y, eventLine, true);
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
