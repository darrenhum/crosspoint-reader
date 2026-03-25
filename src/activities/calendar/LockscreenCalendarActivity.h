#pragma once

#include <CalendarStore.h>
#include <CalendarTypes.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * LockscreenCalendarActivity
 *
 * Displays a monthly calendar grid with event markers from synced ICS feeds.
 * Shows a 7-column grid for the current month, highlights today, and marks
 * days that have events with a dot indicator. Includes a "Last Refreshed"
 * timestamp footer.
 *
 * Navigation:
 *   Left/Right: Previous/Next month
 *   Up/Down:    Previous/Next week (move selected day)
 *   Confirm:    Trigger manual sync
 *   Back:       Return to previous activity
 */
class LockscreenCalendarActivity final : public Activity {
 public:
  explicit LockscreenCalendarActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LockscreenCalendar", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  // Current displayed month/year
  int displayMonth = 1;  // 1-12
  int displayYear = 2025;

  // Selected day within the month (1-based)
  int selectedDay = 1;

  // Today's date
  int todayYear = 0;
  int todayMonth = 0;
  int todayDay = 0;
  uint16_t todayDays = 0;  // Days since 2000-01-01

  // Calendar data from persistent store (read by render task under RenderLock,
  // written by main task only while holding RenderLock)
  calendar::CalendarData calendarData;

  // Sync state - runs on a separate FreeRTOS task for non-blocking UI.
  // syncResultData is heap-allocated only during sync to avoid wasting ~3.5KB permanently.
  bool syncInProgress = false;           ///< Protected by RenderLock (read by render, written by loop)
  volatile bool syncComplete = false;    ///< Set by sync task, polled by main task (must be volatile)
  volatile bool syncAbortRequested = false;  ///< Set by main task to request clean shutdown
  TaskHandle_t syncTaskHandle = nullptr;
  calendar::CalendarData* syncResultData = nullptr;  ///< Heap-allocated, owned during sync only
  static void syncTaskTrampoline(void* param);
  void syncTask();

  // Rendering helpers
  void drawCalendarGrid(int contentX, int contentY, int contentWidth, int contentHeight) const;
  void drawEventList(int listX, int listY, int listWidth, int listHeight) const;
  void drawLastRefreshedFooter(int footerX, int footerY, int footerWidth) const;

  /// Check if a given day (days since 2000-01-01) has any events
  bool dayHasEvent(uint16_t daysSince2000) const;

  /// Get number of events on a given day
  int getEventsForDay(uint16_t daysSince2000, const calendar::CalendarEvent** outEvents, int maxOut) const;

  /// Initialize today's date from system time
  void initToday();

  /// Navigate to previous/next month
  void goToPreviousMonth();
  void goToNextMonth();
};
