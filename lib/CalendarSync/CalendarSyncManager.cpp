#include "CalendarSyncManager.h"

#include <Logging.h>
#include <WiFi.h>

#include <cstring>
#include <ctime>

#include "CalendarStore.h"
#include "IcsParser.h"
#include "esp_http_client.h"

// Include from src/ using relative path (same pattern as KOReaderSync)
#include "../../src/CrossPointSettings.h"
#include "../../src/WifiCredentialStore.h"

extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}

namespace calendar {

namespace {

/// Context for streaming ICS data through the HTTP event handler
struct StreamContext {
  IcsParser* parser;
  char etagBuf[MAX_ETAG_LEN];
  uint32_t lastModifiedEpoch;
  bool gotData;
};

/// Parse an HTTP Date header (e.g. "Thu, 01 Jan 2025 00:00:00 GMT") to epoch seconds.
/// Returns 0 on failure. Manual parse to avoid strptime() portability issues on ESP-IDF.
uint32_t parseHttpDate(const char* dateStr) {
  if (!dateStr) return 0;
  // RFC 7231 format: "Day, DD Mon YYYY HH:MM:SS GMT"
  // Find the date portion after "Day, "
  const char* p = strchr(dateStr, ',');
  if (!p) return 0;
  p++;  // skip comma
  while (*p == ' ') p++;  // skip spaces

  int day = 0, year = 0, hour = 0, min = 0, sec = 0;
  char monStr[4] = {};
  if (sscanf(p, "%d %3s %d %d:%d:%d", &day, monStr, &year, &hour, &min, &sec) != 6) return 0;
  if (day < 1 || day > 31 || hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 59) return 0;

  static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int month = -1;
  for (int i = 0; i < 12; i++) {
    if (strcmp(monStr, months[i]) == 0) {
      month = i;
      break;
    }
  }
  if (month < 0 || year < 2000 || year > 2099) return 0;

  // Convert to days since 2000-01-01 then to epoch
  uint16_t days = dateToDays(year, month + 1, day);
  uint32_t epochSec = static_cast<uint32_t>(days) * 86400 + EPOCH_2000_OFFSET;
  epochSec += static_cast<uint32_t>(hour) * 3600 + static_cast<uint32_t>(min) * 60 + static_cast<uint32_t>(sec);
  return epochSec;
}

/// HTTP event handler that streams data directly to the ICS parser
esp_err_t icsHttpEventHandler(esp_http_client_event_t* event) {
  auto* ctx = static_cast<StreamContext*>(event->user_data);
  if (!ctx) return ESP_OK;

  switch (event->event_id) {
    case HTTP_EVENT_ON_HEADER:
      if (!event->header_key || !event->header_value) break;
      // Capture ETag header
      if (strcasecmp(event->header_key, "ETag") == 0) {
        strncpy(ctx->etagBuf, event->header_value, MAX_ETAG_LEN - 1);
        ctx->etagBuf[MAX_ETAG_LEN - 1] = '\0';
      }
      // Capture Last-Modified header
      if (strcasecmp(event->header_key, "Last-Modified") == 0) {
        ctx->lastModifiedEpoch = parseHttpDate(event->header_value);
      }
      break;

    case HTTP_EVENT_ON_DATA:
      // Stream directly to ICS parser - zero-copy from socket buffer
      if (ctx->parser && event->data_len > 0) {
        ctx->parser->feed(static_cast<const uint8_t*>(event->data), event->data_len);
        ctx->gotData = true;
      }
      break;

    default:
      break;
  }
  return ESP_OK;
}

/// Attempt WiFi connection using stored credentials
bool connectWifi(unsigned long timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;

  WIFI_STORE.loadFromFile();
  WiFi.mode(WIFI_STA);
  const auto& lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (lastSsid.empty()) {
    LOG_ERR("CAL", "No last connected SSID for calendar sync");
    return false;
  }

  const auto* cred = WIFI_STORE.findCredential(lastSsid);
  if (cred) {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  } else {
    WiFi.begin(lastSsid.c_str());
  }

  // Wait for connection with timeout
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(100);
  }

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CAL", "WiFi connection failed for calendar sync");
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    return false;
  }

  LOG_DBG("CAL", "WiFi connected for calendar sync");
  return true;
}

/// Disconnect WiFi and power down the radio
void disconnectWifi() {
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
  LOG_DBG("CAL", "WiFi radio powered down");
}

}  // namespace

const char* CalendarSyncManager::getIcsUrl(uint8_t feedIndex) {
  switch (feedIndex) {
    case 0:
      return SETTINGS.calendarIcsUrl1;
    case 1:
      return SETTINGS.calendarIcsUrl2;
    case 2:
      return SETTINGS.calendarIcsUrl3;
    default:
      return nullptr;
  }
}

CalendarSyncManager::SyncResult CalendarSyncManager::sync(CalendarData& data, uint8_t batteryPct,
                                                          uint32_t currentEpoch) {
  // Check battery threshold
  if (batteryPct < BATTERY_SUSPEND_THRESHOLD) {
    LOG_DBG("CAL", "Sync skipped: battery %u%% below suspend threshold", batteryPct);
    return SyncResult::SKIPPED_BATTERY;
  }

  // Check if any ICS URLs are configured
  bool hasUrls = false;
  for (uint8_t i = 0; i < MAX_ICS_FEEDS; i++) {
    const char* url = getIcsUrl(i);
    if (url && url[0] != '\0') {
      hasUrls = true;
      break;
    }
  }
  if (!hasUrls) {
    return SyncResult::SKIPPED_NO_URLS;
  }

  // Check schedule (adaptive interval + exponential backoff)
  if (data.lastSyncEpoch > 0 && currentEpoch > 0 && currentEpoch >= data.lastSyncEpoch) {
    uint32_t interval = getNextSyncInterval(batteryPct, data.consecutiveFailures);
    if (currentEpoch - data.lastSyncEpoch < interval) {
      return SyncResult::SKIPPED_SCHEDULE;
    }
  }

  // Check quiet hours
  if (currentEpoch > 0 && isQuietHours(currentEpoch)) {
    LOG_DBG("CAL", "Sync deferred: quiet hours");
    return SyncResult::SKIPPED_SCHEDULE;
  }

  // Connect WiFi
  if (!connectWifi(WIFI_CONNECT_TIMEOUT_MS)) {
    data.consecutiveFailures++;
    CalendarStore::save(data);
    disconnectWifi();
    return SyncResult::FAILED;
  }

  // Calculate display window
  uint16_t todayDays = 0;
  if (currentEpoch > EPOCH_2000_OFFSET) {
    todayDays = static_cast<uint16_t>((currentEpoch - EPOCH_2000_OFFSET) / 86400);
  }
  uint16_t windowStart = (todayDays > PAST_DAYS) ? todayDays - PAST_DAYS : 0;
  uint16_t windowEnd = todayDays + FUTURE_DAYS;

  // Heap-allocate temp buffer: 64 events × 52 bytes = 3328 bytes, too large for task stack.
  constexpr size_t tempEventsSize = MAX_EVENTS * sizeof(CalendarEvent);
  auto* tempEvents = static_cast<CalendarEvent*>(malloc(tempEventsSize));
  if (!tempEvents) {
    LOG_ERR("CAL", "malloc failed for tempEvents: %u bytes", tempEventsSize);
    disconnectWifi();
    return SyncResult::FAILED;
  }

  uint8_t tempCount = 0;
  bool anyModified = false;
  bool anyFailed = false;

  // Track which feeds returned 304 (NOT_MODIFIED) for possible re-fetch
  bool feedNeedsRefetch[MAX_ICS_FEEDS] = {};

  // First pass: fetch each configured feed with conditional headers
  for (uint8_t i = 0; i < MAX_ICS_FEEDS; i++) {
    const char* url = getIcsUrl(i);
    if (!url || url[0] == '\0') continue;

    uint8_t maxForFeed = MAX_EVENTS - tempCount;
    if (maxForFeed == 0) break;

    uint8_t addedCount = 0;
    FeedResult result = fetchAndParseFeed(url, data.feedMeta[i], tempEvents + tempCount, addedCount, maxForFeed,
                                          windowStart, windowEnd);
    switch (result) {
      case FeedResult::UPDATED:
        anyModified = true;
        tempCount += addedCount;
        break;
      case FeedResult::NOT_MODIFIED:
        feedNeedsRefetch[i] = true;
        break;
      case FeedResult::FAILED:
        anyFailed = true;
        break;
    }
  }

  // Second pass: if any feed was modified and others returned 304, re-fetch
  // the 304 feeds without conditional headers to get a consistent snapshot.
  // Without this, events from 304 feeds would be silently lost when data.events
  // is replaced with tempEvents.
  if (anyModified) {
    for (uint8_t i = 0; i < MAX_ICS_FEEDS; i++) {
      if (!feedNeedsRefetch[i]) continue;
      const char* url = getIcsUrl(i);
      if (!url || url[0] == '\0') continue;

      uint8_t maxForFeed = MAX_EVENTS - tempCount;
      if (maxForFeed == 0) break;

      // Clear conditional headers to force a full re-fetch
      FeedSyncMeta tempMeta = {};
      uint8_t addedCount = 0;
      FeedResult result = fetchAndParseFeed(url, tempMeta, tempEvents + tempCount, addedCount, maxForFeed,
                                            windowStart, windowEnd);
      if (result == FeedResult::UPDATED) {
        tempCount += addedCount;
        // Update the meta with new values from re-fetch
        data.feedMeta[i] = tempMeta;
      } else if (result == FeedResult::FAILED) {
        anyFailed = true;
      }
    }
  }

  // Power down WiFi as soon as possible
  disconnectWifi();

  if (anyModified) {
    // Replace events with newly fetched data (now includes all feeds)
    data.eventCount = tempCount > MAX_EVENTS ? MAX_EVENTS : tempCount;
    memcpy(data.events, tempEvents, data.eventCount * sizeof(CalendarEvent));
  }

  free(tempEvents);

  // Update sync metadata
  data.lastSyncEpoch = currentEpoch;
  if (anyFailed && !anyModified) {
    data.consecutiveFailures++;
  } else {
    data.consecutiveFailures = 0;
  }

  CalendarStore::save(data);

  if (anyFailed && !anyModified) {
    return SyncResult::FAILED;
  }
  return anyModified ? SyncResult::OK_UPDATED : SyncResult::OK_NOT_MODIFIED;
}

CalendarSyncManager::FeedResult CalendarSyncManager::fetchAndParseFeed(const char* url, FeedSyncMeta& meta,
                                                                       CalendarEvent* events, uint8_t& eventCount,
                                                                       uint8_t maxEvents, uint16_t windowStart,
                                                                       uint16_t windowEnd) {
  // Validate URL: only allow http:// and https:// schemes
  if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
    LOG_ERR("CAL", "Invalid URL scheme (must be http or https): %s", url);
    eventCount = 0;
    return FeedResult::FAILED;
  }

  IcsParser parser;
  parser.begin(windowStart, windowEnd, events, maxEvents);

  StreamContext ctx = {};
  ctx.parser = &parser;
  ctx.gotData = false;
  ctx.lastModifiedEpoch = 0;

  esp_http_client_config_t config = {};
  config.url = url;
  config.event_handler = icsHttpEventHandler;
  config.user_data = &ctx;
  config.timeout_ms = RECV_TIMEOUT_MS;
  config.buffer_size = STREAM_BUF_SIZE;
  config.buffer_size_tx = 256;
  // CN check is skipped because ICS feeds may redirect through CDNs with different hostnames.
  // Root CA validation is still performed via crt_bundle_attach.
  config.skip_cert_common_name_check = true;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = false;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("CAL", "Failed to init HTTP client for %s", url);
    eventCount = 0;
    return FeedResult::FAILED;
  }

  // Set conditional request headers
  if (meta.etag[0] != '\0') {
    esp_http_client_set_header(client, "If-None-Match", meta.etag);
  }

  if (meta.lastModifiedEpoch > 0) {
    // Format as HTTP date: e.g. "Thu, 01 Jan 2025 00:00:00 GMT"
    time_t t = static_cast<time_t>(meta.lastModifiedEpoch);
    struct tm tm;
    gmtime_r(&t, &tm);
    char dateBuf[32];
    strftime(dateBuf, sizeof(dateBuf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    esp_http_client_set_header(client, "If-Modified-Since", dateBuf);
  }

  esp_http_client_set_header(client, "User-Agent", "CrossPoint-Calendar/1.0");

  esp_err_t err = esp_http_client_perform(client);
  int statusCode = esp_http_client_get_status_code(client);

  esp_http_client_cleanup(client);

  if (err != ESP_OK) {
    LOG_ERR("CAL", "HTTP request failed: %s", esp_err_to_name(err));
    eventCount = 0;
    return FeedResult::FAILED;
  }

  if (statusCode == 304) {
    LOG_DBG("CAL", "Feed not modified (304): %s", url);
    eventCount = 0;
    return FeedResult::NOT_MODIFIED;
  }

  if (statusCode != 200) {
    LOG_ERR("CAL", "HTTP %d for feed: %s", statusCode, url);
    eventCount = 0;
    return FeedResult::FAILED;
  }

  // Update sync metadata with new ETag
  if (ctx.etagBuf[0] != '\0') {
    strncpy(meta.etag, ctx.etagBuf, MAX_ETAG_LEN - 1);
    meta.etag[MAX_ETAG_LEN - 1] = '\0';
  }

  // Update Last-Modified epoch
  if (ctx.lastModifiedEpoch > 0) {
    meta.lastModifiedEpoch = ctx.lastModifiedEpoch;
  }

  eventCount = parser.end();
  LOG_DBG("CAL", "Parsed %u events from feed: %s", eventCount, url);
  return FeedResult::UPDATED;
}

uint32_t CalendarSyncManager::getNextSyncInterval(uint8_t batteryPct, uint8_t consecutiveFailures) {
  // Base interval based on battery level
  uint32_t baseInterval;
  if (batteryPct > BATTERY_MED_THRESHOLD) {
    baseInterval = INTERVAL_HIGH_BATTERY;
  } else if (batteryPct > BATTERY_LOW_THRESHOLD) {
    baseInterval = INTERVAL_MED_BATTERY;
  } else {
    baseInterval = INTERVAL_LOW_BATTERY;
  }

  // Apply exponential backoff for failures
  if (consecutiveFailures > 0) {
    uint8_t multiplier = consecutiveFailures;
    if (multiplier > BACKOFF_MAX_MULTIPLIER) multiplier = BACKOFF_MAX_MULTIPLIER;
    uint32_t backoffInterval = BACKOFF_BASE * multiplier;
    if (backoffInterval > baseInterval) {
      return backoffInterval;
    }
  }

  return baseInterval;
}

bool CalendarSyncManager::isQuietHours(uint32_t epochTime) {
  // Extract hour from epoch (UTC)
  uint32_t secondsInDay = epochTime % 86400;
  uint8_t hour = static_cast<uint8_t>(secondsInDay / 3600);

  if (QUIET_START_HOUR > QUIET_END_HOUR) {
    // Wraps around midnight (e.g., 23:00 - 06:00)
    return hour >= QUIET_START_HOUR || hour < QUIET_END_HOUR;
  }
  return hour >= QUIET_START_HOUR && hour < QUIET_END_HOUR;
}

}  // namespace calendar
