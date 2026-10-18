// SPDX-License-Identifier: MIT
#pragma once
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
// Narrow reader for integer timestamps in the tool-generated JSONL format.
// OpenCV FileStorage stores JSON integer nodes as 32-bit values; do not use it
// for nanosecond timestamps or unwrapped sensor counters.
inline uint64_t oq_json_timestamp(const std::string &line,
                                  const std::string &key) {
  auto at = line.find('"' + key + '"');
  if (at == std::string::npos)
    throw std::runtime_error("Missing timestamp");
  at = line.find(':', at + key.size() + 2);
  if (at == std::string::npos)
    throw std::runtime_error("Malformed timestamp");
  ++at;
  while (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
    ++at;
  uint64_t value;
  auto result =
      std::from_chars(line.data() + at, line.data() + line.size(), value);
  if (result.ec != std::errc() || result.ptr == line.data() + line.size() ||
      (*result.ptr != ',' && *result.ptr != '}' && *result.ptr != ' '))
    throw std::runtime_error("Invalid timestamp integer");
  return value;
}
