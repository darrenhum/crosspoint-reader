#pragma once

#include "CalendarTypes.h"

namespace calendar {

/**
 * Persists CalendarData to/from SD card as a binary file.
 * File path: /.crosspoint/calendar.bin
 */
class CalendarStore {
 public:
  /// Load calendar data from SD card. Returns true on success.
  static bool load(CalendarData& data);

  /// Save calendar data to SD card. Returns true on success.
  static bool save(const CalendarData& data);

 private:
  static constexpr char FILE_PATH[] = "/.crosspoint/calendar.bin";
  static constexpr uint8_t FILE_VERSION = 1;
  static constexpr uint32_t FILE_MAGIC = 0x43414C31;  // "CAL1"
};

}  // namespace calendar
