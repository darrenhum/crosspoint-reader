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
  bool gotData;
};

/// HTTP event handler that streams data directly to the ICS parser
esp_err_t icsHttpEventHandler(esp_http_client_event_t* event) {
  auto* ctx = static_cast<StreamContext*>(event->user_data);
  if (!ctx) return ESP_OK;

  switch (event->event_id) {
    case HTTP_EVENT_ON_HEADER:
      // Capture ETag header
      if (strcasecmp(event->header_key, "ETag") == 0 && event->header_value) {
        strncpy(ctx->etagBuf, event->header_value, MAX_ETAG_LEN - 1);
        ctx->etagBuf[MAX_ETAG_LEN - 1] = '\0';
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
bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;

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
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
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
    const char* url = nullptr;
    switch (i) {
      case 0:
        url = SETTINGS.calendarIcsUrl1;
        break;
      case 1:
        url = SETTINGS.calendarIcsUrl2;
        break;
      case 2:
        url = SETTINGS.calendarIcsUrl3;
        break;
    }
    if (url && url[0] != '\0') {
      hasUrls = true;
      break;
    }
  }
  if (!hasUrls) {
    return SyncResult::SKIPPED_NO_URLS;
  }

  // Check schedule (adaptive interval + exponential backoff)
  if (data.lastSyncEpoch > 0 && currentEpoch > 0) {
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
  if (!connectWifi()) {
    data.consecutiveFailures++;
    CalendarStore::save(data);
    disconnectWifi();
    return SyncResult::FAILED;
  }

  // Calculate display window
  // Convert currentEpoch to days since 2000-01-01
  // Unix epoch (1970) to 2000 epoch offset: 946684800 seconds
  constexpr uint32_t EPOCH_2000_OFFSET = 946684800;
  uint16_t todayDays = 0;
  if (currentEpoch > EPOCH_2000_OFFSET) {
    todayDays = static_cast<uint16_t>((currentEpoch - EPOCH_2000_OFFSET) / 86400);
  }
  uint16_t windowStart = (todayDays > PAST_DAYS) ? todayDays - PAST_DAYS : 0;
  uint16_t windowEnd = todayDays + FUTURE_DAYS;

  // Temporary buffer for merged events from all feeds
  CalendarEvent tempEvents[MAX_EVENTS];
  uint8_t tempCount = 0;
  bool anyModified = false;
  bool anyFailed = false;

  // Fetch each configured feed sequentially
  for (uint8_t i = 0; i < MAX_ICS_FEEDS; i++) {
    const char* url = nullptr;
    switch (i) {
      case 0:
        url = SETTINGS.calendarIcsUrl1;
        break;
      case 1:
        url = SETTINGS.calendarIcsUrl2;
        break;
      case 2:
        url = SETTINGS.calendarIcsUrl3;
        break;
    }
    if (!url || url[0] == '\0') continue;

    uint8_t remaining = MAX_EVENTS - tempCount;
    if (remaining == 0) break;

    bool modified = fetchAndParseFeed(url, data.feedMeta[i], tempEvents + tempCount, remaining, remaining, windowStart,
                                      windowEnd);
    if (modified) {
      anyModified = true;
      tempCount += remaining;
    } else if (remaining == 0) {
      // 304 Not Modified - keep existing events from this feed
      // (they're already in data.events from previous sync)
    } else {
      anyFailed = true;
    }
  }

  // Power down WiFi as soon as possible
  disconnectWifi();

  if (anyModified) {
    // Replace events with newly fetched data
    data.eventCount = tempCount > MAX_EVENTS ? MAX_EVENTS : tempCount;
    memcpy(data.events, tempEvents, data.eventCount * sizeof(CalendarEvent));
  }

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

bool CalendarSyncManager::fetchAndParseFeed(const char* url, FeedSyncMeta& meta, CalendarEvent* events,
                                            uint8_t& eventCount, uint8_t maxEvents, uint16_t windowStart,
                                            uint16_t windowEnd) {
  IcsParser parser;
  parser.begin(windowStart, windowEnd, events, maxEvents);

  StreamContext ctx = {};
  ctx.parser = &parser;
  ctx.gotData = false;

  esp_http_client_config_t config = {};
  config.url = url;
  config.event_handler = icsHttpEventHandler;
  config.user_data = &ctx;
  config.timeout_ms = RECV_TIMEOUT_MS;
  config.buffer_size = STREAM_BUF_SIZE;
  config.buffer_size_tx = 256;
  config.skip_cert_common_name_check = true;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = false;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("CAL", "Failed to init HTTP client for %s", url);
    eventCount = 0;
    return false;
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
    return false;
  }

  if (statusCode == 304) {
    // Not Modified - data unchanged
    LOG_DBG("CAL", "Feed not modified (304): %s", url);
    eventCount = 0;
    return false;
  }

  if (statusCode != 200) {
    LOG_ERR("CAL", "HTTP %d for feed: %s", statusCode, url);
    eventCount = 0;
    return false;
  }

  // Update sync metadata with new ETag
  if (ctx.etagBuf[0] != '\0') {
    strncpy(meta.etag, ctx.etagBuf, MAX_ETAG_LEN - 1);
    meta.etag[MAX_ETAG_LEN - 1] = '\0';
  }

  eventCount = parser.end();
  LOG_DBG("CAL", "Parsed %u events from feed: %s", eventCount, url);
  return true;
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
