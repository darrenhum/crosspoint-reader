#include "CalendarStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace calendar {

bool CalendarStore::load(CalendarData& data) {
  FsFile file;
  if (!Storage.openFileForRead("CAL", FILE_PATH, file)) {
    return false;
  }

  // Read and validate magic + version
  uint32_t magic = 0;
  if (file.read(&magic, sizeof(magic)) != sizeof(magic) || magic != FILE_MAGIC) {
    LOG_ERR("CAL", "Invalid calendar file magic");
    file.close();
    return false;
  }

  uint8_t version = 0;
  if (file.read(&version, sizeof(version)) != sizeof(version) || version != FILE_VERSION) {
    LOG_ERR("CAL", "Unsupported calendar file version: %u", version);
    file.close();
    return false;
  }

  // Read event count
  if (file.read(&data.eventCount, sizeof(data.eventCount)) != sizeof(data.eventCount)) {
    file.close();
    return false;
  }
  if (data.eventCount > MAX_EVENTS) data.eventCount = MAX_EVENTS;

  // Read events
  for (uint8_t i = 0; i < data.eventCount; i++) {
    if (file.read(&data.events[i], sizeof(CalendarEvent)) != sizeof(CalendarEvent)) {
      data.eventCount = i;
      break;
    }
    // Ensure null termination
    data.events[i].summary[MAX_SUMMARY_LEN - 1] = '\0';
  }

  // Read feed sync metadata
  if (file.read(data.feedMeta, sizeof(data.feedMeta)) != sizeof(data.feedMeta)) {
    LOG_ERR("CAL", "Failed to read feed sync metadata");
    file.close();
    return false;
  }

  // Read sync metadata
  if (file.read(&data.lastSyncEpoch, sizeof(data.lastSyncEpoch)) != sizeof(data.lastSyncEpoch)) {
    LOG_ERR("CAL", "Failed to read last sync epoch");
    file.close();
    return false;
  }
  if (file.read(&data.consecutiveFailures, sizeof(data.consecutiveFailures)) != sizeof(data.consecutiveFailures)) {
    data.consecutiveFailures = 0;  // Non-critical, default to 0
  }

  file.close();
  LOG_DBG("CAL", "Loaded %u calendar events from file", data.eventCount);
  return true;
}

bool CalendarStore::save(const CalendarData& data) {
  Storage.mkdir("/.crosspoint");

  FsFile file;
  if (!Storage.openFileForWrite("CAL", FILE_PATH, file)) {
    return false;
  }

  bool ok = true;

  // Write magic + version
  uint32_t magic = FILE_MAGIC;
  ok = ok && (file.write(reinterpret_cast<const uint8_t*>(&magic), sizeof(magic)) == sizeof(magic));

  uint8_t version = FILE_VERSION;
  ok = ok && (file.write(&version, sizeof(version)) == sizeof(version));

  // Write event count
  uint8_t count = data.eventCount > MAX_EVENTS ? MAX_EVENTS : data.eventCount;
  ok = ok && (file.write(&count, sizeof(count)) == sizeof(count));

  // Write events
  for (uint8_t i = 0; i < count && ok; i++) {
    ok = ok && (file.write(reinterpret_cast<const uint8_t*>(&data.events[i]), sizeof(CalendarEvent)) ==
                sizeof(CalendarEvent));
  }

  // Write feed sync metadata
  ok = ok && (file.write(reinterpret_cast<const uint8_t*>(data.feedMeta), sizeof(data.feedMeta)) ==
              sizeof(data.feedMeta));

  // Write sync metadata
  ok = ok && (file.write(reinterpret_cast<const uint8_t*>(&data.lastSyncEpoch), sizeof(data.lastSyncEpoch)) ==
              sizeof(data.lastSyncEpoch));
  ok = ok && (file.write(&data.consecutiveFailures, sizeof(data.consecutiveFailures)) ==
              sizeof(data.consecutiveFailures));

  file.close();

  if (!ok) {
    LOG_ERR("CAL", "Failed to write calendar data to file");
    return false;
  }

  LOG_DBG("CAL", "Saved %u calendar events to file", count);
  return true;
}

}  // namespace calendar
