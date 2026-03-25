#pragma once

#include <cstdint>

#include "CalendarTypes.h"

namespace calendar {

/**
 * Manages background ICS feed synchronization.
 *
 * Features:
 * - Sequential fetching of up to 3 ICS URLs
 * - HTTP If-Modified-Since / ETag conditional requests (304 handling)
 * - Aggressive timeouts to prevent hangs
 * - Adaptive scheduling based on battery level
 * - Exponential backoff on failures
 * - Quiet hours avoidance (23:00-06:00)
 */
class CalendarSyncManager {
 public:
  enum class SyncResult {
    OK_UPDATED,        ///< Data changed, full refresh needed
    OK_NOT_MODIFIED,   ///< All feeds returned 304, partial refresh only
    FAILED,            ///< One or more feeds failed
    SKIPPED_BATTERY,   ///< Sync skipped due to low battery
    SKIPPED_SCHEDULE,  ///< Not yet time for next sync
    SKIPPED_NO_URLS,   ///< No ICS URLs configured
  };

  /**
   * Perform a synchronization cycle.
   * Connects WiFi, fetches configured ICS URLs, parses events, stores results.
   *
   * @param data          Calendar data to update (read/write)
   * @param batteryPct    Current battery percentage (0-100)
   * @param currentEpoch  Current time as epoch seconds
   * @param abortFlag     Optional pointer to volatile bool; checked between feeds for early exit
   * @return SyncResult indicating what happened
   */
  static SyncResult sync(CalendarData& data, uint8_t batteryPct, uint32_t currentEpoch,
                          const volatile bool* abortFlag = nullptr);

  /**
   * Calculate the next sync interval in seconds based on battery level and failure count.
   *
   * @param batteryPct         Current battery percentage
   * @param consecutiveFailures Number of consecutive sync failures
   * @return Interval in seconds until next sync should be attempted
   */
  static uint32_t getNextSyncInterval(uint8_t batteryPct, uint8_t consecutiveFailures);

  /**
   * Check if current time is within quiet hours (avoid syncing during sleep hours).
   * @param epochTime Current epoch time
   * @return true if sync should be deferred
   */
  static bool isQuietHours(uint32_t epochTime);

 private:
  /// Result of fetching a single ICS feed
  enum class FeedResult {
    UPDATED,       ///< Feed returned 200, events parsed
    NOT_MODIFIED,  ///< Feed returned 304, no changes
    FAILED,        ///< HTTP error or invalid URL
  };

  /// Fetch a single ICS feed URL and parse events into the provided buffer.
  static FeedResult fetchAndParseFeed(const char* url, FeedSyncMeta& meta, CalendarEvent* events, uint8_t& eventCount,
                                      uint8_t maxEvents, uint16_t windowStart, uint16_t windowEnd);

  /// Get the ICS URL for a given feed index (0-based). Returns nullptr if not configured.
  static const char* getIcsUrl(uint8_t feedIndex);

  // Timeout constants (milliseconds)
  static constexpr int CONNECT_TIMEOUT_MS = 5000;
  static constexpr int TLS_TIMEOUT_MS = 5000;
  static constexpr int RECV_TIMEOUT_MS = 10000;
  static constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;

  // Adaptive scheduling intervals (seconds)
  static constexpr uint32_t INTERVAL_HIGH_BATTERY = 6 * 3600;    ///< >50%: every 6 hours
  static constexpr uint32_t INTERVAL_MED_BATTERY = 12 * 3600;    ///< 20-50%: every 12 hours
  static constexpr uint32_t INTERVAL_LOW_BATTERY = 24 * 3600;    ///< 10-20%: once daily
  static constexpr uint8_t BATTERY_SUSPEND_THRESHOLD = 10;       ///< <10%: suspend sync entirely
  static constexpr uint8_t BATTERY_LOW_THRESHOLD = 20;           ///< <20%: low battery
  static constexpr uint8_t BATTERY_MED_THRESHOLD = 50;           ///< <50%: medium battery

  // Exponential backoff
  static constexpr uint32_t BACKOFF_BASE = 6 * 3600;   ///< 6 hours base
  static constexpr uint8_t BACKOFF_MAX_MULTIPLIER = 4;  ///< Max 4x (24 hours)

  // Quiet hours (UTC hour range)
  static constexpr uint8_t QUIET_START_HOUR = 23;
  static constexpr uint8_t QUIET_END_HOUR = 6;

  // HTTP stream buffer
  static constexpr size_t STREAM_BUF_SIZE = 512;
};

}  // namespace calendar
