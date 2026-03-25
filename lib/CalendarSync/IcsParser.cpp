#include "IcsParser.h"

#include <cstring>

namespace calendar {

void IcsParser::begin(uint16_t windowStart, uint16_t windowEnd, CalendarEvent* outEvents, uint8_t maxEvents) {
  this->windowStart = windowStart;
  this->windowEnd = windowEnd;
  this->events = outEvents;
  this->maxEventCount = maxEvents;
  this->eventCount = 0;

  state = State::SCANNING;
  currentProperty = Property::NONE;
  lineLen = 0;
  pendingNewline = false;
  hasStart = false;
  hasEnd = false;
  currentSummary[0] = '\0';
  currentSummaryLen = 0;
}

void IcsParser::feed(const uint8_t* data, size_t len) {
  if (!events || eventCount >= maxEventCount) return;

  for (size_t i = 0; i < len; i++) {
    char c = static_cast<char>(data[i]);

    // Handle line endings: \r\n or \n
    if (c == '\r') continue;

    if (c == '\n') {
      // ICS continuation lines start with a space or tab on the next line.
      // We set a flag so that when we process the next character, we can
      // determine if it's a continuation or a new line.
      lineBuf[lineLen] = '\0';
      pendingNewline = true;
      continue;
    }

    // Handle ICS line folding: if we just saw a newline and c is space/tab,
    // it's a continuation of the previous logical line.
    if (pendingNewline) {
      pendingNewline = false;
      if (c == ' ' || c == '\t') {
        // Continuation line - append to current buffer (skip the folding whitespace)
        continue;
      }
      // Not a continuation - process the completed line
      processLine();
      lineLen = 0;
    }

    if (lineLen < LINE_BUF_SIZE - 1) {
      lineBuf[lineLen++] = c;
    }
    // If line exceeds buffer, silently truncate (max lineLen stays at LINE_BUF_SIZE-1)
  }
}

uint8_t IcsParser::end() {
  // Process any remaining data in the line buffer
  if (lineLen > 0 || pendingNewline) {
    lineBuf[lineLen] = '\0';
    processLine();
    lineLen = 0;
    pendingNewline = false;
  }
  return eventCount;
}

void IcsParser::processLine() {
  if (lineLen == 0) return;

  switch (state) {
    case State::SCANNING:
      if (startsWith(lineBuf, lineLen, "BEGIN:VEVENT")) {
        state = State::IN_VEVENT;
        hasStart = false;
        hasEnd = false;
        currentSummary[0] = '\0';
        currentSummaryLen = 0;
        currentStart = 0;
        currentEnd = 0;
      }
      break;

    case State::IN_VEVENT:
      if (startsWith(lineBuf, lineLen, "END:VEVENT")) {
        commitEvent();
        state = State::SCANNING;
      } else if (startsWith(lineBuf, lineLen, "DTSTART")) {
        // Find the colon after DTSTART (may have params like ;VALUE=DATE:)
        const char* colon = strchr(lineBuf, ':');
        if (colon && colon + 1 < lineBuf + lineLen) {
          size_t valLen = lineLen - static_cast<size_t>(colon + 1 - lineBuf);
          currentStart = parseDtValue(colon + 1, valLen);
          hasStart = true;
        }
      } else if (startsWith(lineBuf, lineLen, "DTEND")) {
        const char* colon = strchr(lineBuf, ':');
        if (colon && colon + 1 < lineBuf + lineLen) {
          size_t valLen = lineLen - static_cast<size_t>(colon + 1 - lineBuf);
          currentEnd = parseDtValue(colon + 1, valLen);
          hasEnd = true;
        }
      } else if (startsWith(lineBuf, lineLen, "SUMMARY")) {
        const char* colon = strchr(lineBuf, ':');
        if (colon && colon + 1 < lineBuf + lineLen) {
          const char* val = colon + 1;
          size_t valLen = lineLen - static_cast<size_t>(val - lineBuf);
          if (valLen >= MAX_SUMMARY_LEN) valLen = MAX_SUMMARY_LEN - 1;
          memcpy(currentSummary, val, valLen);
          currentSummary[valLen] = '\0';
          currentSummaryLen = static_cast<uint8_t>(valLen);
        }
      }
      break;

    default:
      break;
  }
}

void IcsParser::commitEvent() {
  if (!hasStart || eventCount >= maxEventCount) return;

  // Default end to start+1 for all-day events without DTEND
  if (!hasEnd) {
    currentEnd = currentStart + 1;
  }

  // Filter: discard events entirely outside the display window
  if (currentEnd <= windowStart || currentStart >= windowEnd) return;

  CalendarEvent& evt = events[eventCount];
  evt.startDay = currentStart;
  evt.endDay = currentEnd;
  // Copy using tracked length (avoids strlen scan; max content length is MAX_SUMMARY_LEN-1)
  memcpy(evt.summary, currentSummary, currentSummaryLen);
  evt.summary[currentSummaryLen] = '\0';
  eventCount++;
}

uint16_t IcsParser::parseDtValue(const char* value, size_t len) {
  // ICS date formats:
  // DATE: 20250315
  // DATETIME: 20250315T120000 or 20250315T120000Z
  // We only need the date portion (first 8 chars)
  if (len < 8) return 0;

  // Validate that first 8 chars are all digits
  for (int i = 0; i < 8; i++) {
    if (value[i] < '0' || value[i] > '9') return 0;
  }

  // Parse YYYYMMDD directly from digits (avoids atoi + temp buffers)
  int year = (value[0] - '0') * 1000 + (value[1] - '0') * 100 + (value[2] - '0') * 10 + (value[3] - '0');
  int month = (value[4] - '0') * 10 + (value[5] - '0');
  int day = (value[6] - '0') * 10 + (value[7] - '0');

  if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31) {
    return 0;
  }

  return dateToDays(year, month, day);
}

bool IcsParser::startsWith(const char* str, size_t len, const char* prefix, size_t prefixLen) {
  if (len < prefixLen) return false;
  return memcmp(str, prefix, prefixLen) == 0;
}

}  // namespace calendar
