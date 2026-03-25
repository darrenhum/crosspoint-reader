#pragma once

#include <cstddef>
#include <cstdint>

#include "CalendarTypes.h"

namespace calendar {

/**
 * Zero-allocation streaming ICS (iCalendar) parser.
 *
 * Designed to be fed small chunks (e.g., 512 bytes) from an HTTP response
 * without buffering the entire file in memory. Extracts DTSTART, DTEND, and
 * SUMMARY from VEVENT blocks and discards events outside the display window.
 *
 * Usage:
 *   IcsParser parser;
 *   parser.begin(windowStartDays, windowEndDays, events, maxEvents);
 *   while (chunk = readFromSocket(...)) {
 *     parser.feed(chunk, chunkLen);
 *   }
 *   int count = parser.end();
 */
class IcsParser {
 public:
  IcsParser() = default;

  /**
   * Initialize a new parse session.
   * @param windowStart  Earliest day to keep (days since 2000-01-01)
   * @param windowEnd    Latest day to keep (exclusive, days since 2000-01-01)
   * @param outEvents    Pre-allocated event array to fill
   * @param maxEvents    Size of outEvents array
   */
  void begin(uint16_t windowStart, uint16_t windowEnd, CalendarEvent* outEvents, uint8_t maxEvents);

  /**
   * Feed a chunk of ICS data to the parser.
   * @param data  Pointer to chunk buffer
   * @param len   Length of data in bytes
   */
  void feed(const uint8_t* data, size_t len);

  /**
   * Finalize parsing and return the number of events collected.
   */
  uint8_t end();

 private:
  enum class State {
    SCANNING,     ///< Looking for BEGIN:VEVENT
    IN_VEVENT,    ///< Inside a VEVENT block, scanning properties
    IN_VALUE,     ///< Reading a property value
    SKIP_LINE,    ///< Skip remaining chars until newline
  };

  enum class Property {
    NONE,
    DTSTART,
    DTEND,
    SUMMARY,
  };

  // Parse state
  State state = State::SCANNING;
  Property currentProperty = Property::NONE;

  // Line buffer for accumulating key:value data
  static constexpr size_t LINE_BUF_SIZE = 256;
  char lineBuf[LINE_BUF_SIZE];
  size_t lineLen = 0;

  // Current event being built
  uint16_t currentStart = 0;
  uint16_t currentEnd = 0;
  char currentSummary[MAX_SUMMARY_LEN];
  bool hasStart = false;
  bool hasEnd = false;

  // Output
  CalendarEvent* events = nullptr;
  uint8_t eventCount = 0;
  uint8_t maxEventCount = 0;

  // Window filter
  uint16_t windowStart = 0;
  uint16_t windowEnd = 0;

  void processLine();
  void commitEvent();
  static uint16_t parseDtValue(const char* value, size_t len);
  static bool startsWith(const char* str, size_t len, const char* prefix);
};

}  // namespace calendar
