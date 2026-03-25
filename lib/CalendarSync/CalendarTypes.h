#pragma once

#include <cstdint>
#include <cstring>

namespace calendar {

/// Maximum number of .ics feed URLs supported
static constexpr uint8_t MAX_ICS_FEEDS = 3;

/// Maximum number of events stored across all feeds for the current display window
static constexpr uint8_t MAX_EVENTS = 64;

/// Maximum length for an event summary (null-terminated)
static constexpr uint8_t MAX_SUMMARY_LEN = 48;

/// Maximum length for an ICS URL (null-terminated)
static constexpr uint16_t MAX_ICS_URL_LEN = 256;

/// Maximum length for an ETag header value (null-terminated)
static constexpr uint8_t MAX_ETAG_LEN = 64;

/// Display window: events from today minus this many days
static constexpr uint8_t PAST_DAYS = 0;

/// Display window: events up to this many days in the future
static constexpr uint8_t FUTURE_DAYS = 35;

/// Compact event representation for display
struct CalendarEvent {
  uint16_t startDay;  ///< Days since 2000-01-01 (compact date)
  uint16_t endDay;    ///< Days since 2000-01-01 (exclusive end)
  char summary[MAX_SUMMARY_LEN];

  CalendarEvent() : startDay(0), endDay(0) { summary[0] = '\0'; }
};

/// Per-feed sync metadata for conditional HTTP requests
struct FeedSyncMeta {
  char etag[MAX_ETAG_LEN];
  uint32_t lastModifiedEpoch;  ///< Epoch timestamp of Last-Modified header

  FeedSyncMeta() : lastModifiedEpoch(0) { etag[0] = '\0'; }
};

/// All calendar data persisted to SD card
struct CalendarData {
  CalendarEvent events[MAX_EVENTS];
  uint8_t eventCount;
  FeedSyncMeta feedMeta[MAX_ICS_FEEDS];
  uint32_t lastSyncEpoch;       ///< Epoch time of last successful sync
  uint8_t consecutiveFailures;  ///< For exponential backoff

  CalendarData() : eventCount(0), lastSyncEpoch(0), consecutiveFailures(0) { memset(feedMeta, 0, sizeof(feedMeta)); }
};

/// Convert a broken-down date (year, month 1-12, day 1-31) to days since 2000-01-01
inline uint16_t dateToDays(int year, int month, int day) {
  // Adjust for months Jan/Feb in the previous year for the leap year formula
  int y = year;
  int m = month;
  if (m <= 2) {
    y--;
    m += 12;
  }
  // Days from 2000-01-01 using a simplified Julian day calculation
  int32_t days = 365L * (y - 2000) + (y - 2000 + 3) / 4 - (y - 2000 + 99) / 100 + (y - 2000 + 399) / 400;
  days += (153 * (m - 3) + 2) / 5 + day - 1;
  // Adjust for the offset: 2000-01-01 is day 0
  // March 1, 2000 offset
  days += 59;  // Jan(31) + Feb(29 in 2000) = 60, minus 1 for zero-based = 59
  return static_cast<uint16_t>(days > 0 ? days : 0);
}

/// Convert days since 2000-01-01 back to year, month (1-12), day (1-31)
inline void daysToDate(uint16_t totalDays, int& year, int& month, int& day) {
  // Inverse of dateToDays, iterative approach for simplicity on embedded
  year = 2000;
  int32_t remaining = totalDays;

  while (true) {
    bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    int daysInYear = leap ? 366 : 365;
    if (remaining < daysInYear) break;
    remaining -= daysInYear;
    year++;
  }

  static constexpr int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));

  month = 1;
  for (int i = 0; i < 12; i++) {
    int dim = daysInMonth[i];
    if (i == 1 && leap) dim = 29;
    if (remaining < dim) break;
    remaining -= dim;
    month++;
  }
  day = static_cast<int>(remaining) + 1;
}

/// Get the day of the week for a given date (0=Sunday, 6=Saturday)
inline int dayOfWeek(int year, int month, int day) {
  // Tomohiko Sakamoto's algorithm
  static constexpr int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int y = year;
  if (month < 3) y--;
  return (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;
}

/// Get number of days in a given month (1-12) of a given year
inline int daysInMonth(int year, int month) {
  static constexpr int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 30;
  int days = dim[month - 1];
  if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
    days = 29;
  }
  return days;
}

}  // namespace calendar
