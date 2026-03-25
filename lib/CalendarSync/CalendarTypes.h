#pragma once

#include <cstdint>
#include <cstring>

namespace calendar {

/// Maximum number of .ics feed URLs supported
inline constexpr uint8_t MAX_ICS_FEEDS = 3;

/// Maximum number of events stored across all feeds for the current display window
inline constexpr uint8_t MAX_EVENTS = 64;

/// Maximum length for an event summary (null-terminated)
inline constexpr uint8_t MAX_SUMMARY_LEN = 48;

/// Maximum length for an ICS URL (null-terminated)
inline constexpr uint16_t MAX_ICS_URL_LEN = 256;

/// Maximum length for an ETag header value (null-terminated)
inline constexpr uint8_t MAX_ETAG_LEN = 64;

/// Display window: events from today minus this many days
inline constexpr uint8_t PAST_DAYS = 0;

/// Display window: events up to this many days in the future
inline constexpr uint8_t FUTURE_DAYS = 35;

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

/// Unix epoch offset for 2000-01-01 (946684800 seconds)
inline constexpr uint32_t EPOCH_2000_OFFSET = 946684800;

/// Count leap years from 2000 up to (but not including) the given year.
inline int leapYearsBefore(int year) {
  // Leap years between 2000 and year-1 inclusive
  int y0 = 1999;  // one before 2000
  int y1 = year - 1;
  return (y1 / 4 - y0 / 4) - (y1 / 100 - y0 / 100) + (y1 / 400 - y0 / 400);
}

/// Convert a broken-down date (year, month 1-12, day 1-31) to days since 2000-01-01.
/// O(1) arithmetic formula. Supports years 2000-2099.
inline uint16_t dateToDays(int year, int month, int day) {
  static constexpr int daysBeforeMonth[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1) return 0;

  int32_t days = 365 * (year - 2000) + leapYearsBefore(year);
  days += daysBeforeMonth[month - 1];
  // Add leap day if past February in a leap year
  bool leapYear = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
  if (month > 2 && leapYear) days++;
  days += day - 1;

  return static_cast<uint16_t>(days > 0 ? days : 0);
}

/// Convert days since 2000-01-01 back to year, month (1-12), day (1-31).
/// Capped at year 2099 to prevent infinite loops on extreme input values.
inline void daysToDate(uint16_t totalDays, int& year, int& month, int& day) {
  year = 2000;
  int32_t remaining = totalDays;

  while (year < 2100) {
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
