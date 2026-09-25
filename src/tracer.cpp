// SPDX-License-Identifier: MIT
#include "tracer.hpp"

#include "processutil.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>

#include <utility>

namespace makebelieve {

namespace {

// Thread ids and lane ids are handed out from one counter, in the order they
// are first used, so they stay small and a lane can never land on a thread.
std::uint32_t next_tid() {
  static std::atomic<std::uint32_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

// The row this thread has been lent, if any (see TracePoolScope). A row of 0
// is none, since rows are handed out from 1.
TraceLane& lent_row() {
  thread_local TraceLane row;
  return row;
}

// This thread's own row, which it records on when it has not been lent one.
std::uint32_t own_row() {
  thread_local const std::uint32_t row = next_tid();
  return row;
}

// The buffer each thread renders its events into, reused so recording does not
// allocate once it has grown to fit.
std::string& scratch() {
  thread_local std::string buffer;
  buffer.clear();
  return buffer;
}

template <typename Integer>
void append_number(std::string& out, Integer value) {
  std::array<char, 24> digits{};
  const std::to_chars_result result =
      std::to_chars(digits.data(), digits.data() + digits.size(), value);
  out.append(digits.data(), result.ptr);
}

// Appends @a duration as microseconds - the unit the format wants - to three
// decimal places, so sub-microsecond spans still register.
void append_microseconds(std::string& out, std::chrono::nanoseconds duration) {
  const std::int64_t nanoseconds = std::max<std::int64_t>(duration.count(), 0);
  append_number(out, nanoseconds / 1000);
  const auto fraction = static_cast<int>(nanoseconds % 1000);
  out += '.';
  out += static_cast<char>('0' + (fraction / 100));
  out += static_cast<char>('0' + ((fraction / 10) % 10));
  out += static_cast<char>('0' + (fraction % 10));
}

// The length of the valid UTF-8 sequence starting at @a text[i], or 0 when
// there is none there.
std::size_t utf8_sequence_length(std::string_view text, std::size_t i) {
  const auto byte = [&](std::size_t offset) {
    return static_cast<unsigned char>(text[i + offset]);
  };
  const auto continuation = [&](std::size_t offset) {
    return i + offset < text.size() && (byte(offset) & 0xC0U) == 0x80U;
  };

  const unsigned char lead = byte(0);
  if (lead >= 0xC2 && lead <= 0xDF) {
    return continuation(1) ? 2 : 0;
  }
  if (lead >= 0xE0 && lead <= 0xEF) {
    if (!continuation(1) || !continuation(2)) {
      return 0;
    }
    // Neither an overlong encoding nor a UTF-16 surrogate.
    if ((lead == 0xE0 && byte(1) < 0xA0) || (lead == 0xED && byte(1) > 0x9F)) {
      return 0;
    }
    return 3;
  }
  if (lead >= 0xF0 && lead <= 0xF4) {
    if (!continuation(1) || !continuation(2) || !continuation(3)) {
      return 0;
    }
    // Neither an overlong encoding nor past U+10FFFF.
    if ((lead == 0xF0 && byte(1) < 0x90) || (lead == 0xF4 && byte(1) > 0x8F)) {
      return 0;
    }
    return 4;
  }
  return 0;
}

// Appends one character that needs escaping.
void append_escaped(std::string& out, unsigned char c) {
  switch (c) {
    case '"':
      out += "\\\"";
      return;
    case '\\':
      out += "\\\\";
      return;
    case '\n':
      out += "\\n";
      return;
    case '\r':
      out += "\\r";
      return;
    case '\t':
      out += "\\t";
      return;
    default:
      break;
  }
  constexpr std::string_view k_hex = "0123456789abcdef";
  out += "\\u00";
  out += k_hex[c >> 4U];
  out += k_hex[c & 0xFU];
}

}  // namespace

namespace {

// Appends @a text to @a out as a quoted JSON string, escaping what JSON
// requires and replacing any byte that is not valid UTF-8 with U+FFFD, so
// arbitrary bytes still make a valid trace.
void append_json_string(std::string& out, std::string_view text) {
  out += '"';
  // Runs of characters that need nothing are appended whole.
  std::size_t run = 0;
  std::size_t i = 0;
  while (i < text.size()) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c >= 0x20 && c < 0x80 && c != '"' && c != '\\') {
      ++i;
      continue;
    }
    out.append(text.substr(run, i - run));
    if (c < 0x80) {
      append_escaped(out, c);
      ++i;
    } else if (const std::size_t length = utf8_sequence_length(text, i);
               length != 0) {
      out.append(text.substr(i, length));
      i += length;
    } else {
      out += "\\ufffd";
      ++i;
    }
    run = i;
  }
  out.append(text.substr(run));
  out += '"';
}

}  // namespace

void TraceArgs::add_value(std::string_view key,
                          std::string_view value,
                          bool literal) {
  m_json += m_json.empty() ? "\"" : ",\"";
  m_json += key;
  m_json += "\":";
  if (literal) {
    m_json += value;
  } else {
    append_json_string(m_json, value);
  }
}

void TraceText::append(std::string& out, std::int64_t value) {
  append_number(out, value);
}

void TraceText::append(std::string& out, const std::filesystem::path& value) {
  if (value.empty()) {
    out += '/';
    return;
  }

  try {
    const std::u8string utf8 = value.generic_u8string();
    out.append(utf8.begin(), utf8.end());
  } catch (const std::system_error&) {
    // A Windows name that is not valid UTF-16 has no UTF-8 spelling.
    out += '?';
  }
}

Tracer::Tracer(std::size_t capacity)
    : m_process(ProcessUtil::self()),
      m_capacity(capacity),
      m_slice_size(std::max<std::size_t>(capacity / 16, 1)) {
  name_process(m_process, "makebelieve");
}

TraceLane Tracer::here() const noexcept {
  const TraceLane lent = lent_row();
  return lent.row != 0 ? lent
                       : TraceLane{.process = m_process, .row = own_row()};
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void Tracer::begin_event(std::string& out,
                         char phase,
                         std::string_view category,
                         std::string_view name,
                         Clock::time_point timestamp,
                         TraceLane row) const {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  out += R"(,{"ph":")";
  out += phase;
  out += R"(","cat":")";
  out += category;
  out += R"(","name":)";
  append_json_string(out, name);
  out += R"(,"pid":)";
  append_number(out, row.process);
  out += R"(,"tid":)";
  append_number(out, row.row);
  out += R"(,"ts":)";
  append_microseconds(out, timestamp - m_origin);
}

namespace {

// Appends the event's arguments, if it has any, then closes it.
void end_event(std::string& out, const TraceArgs& args) {
  if (!args.json().empty()) {
    out += R"(,"args":{)";
    out += args.json();
    out += '}';
  }
  out += "}\n";
}

}  // namespace

void Tracer::complete(std::string_view category,
                      std::string_view name,
                      Clock::time_point start,
                      Clock::time_point end,
                      const TraceArgs& args) noexcept {
  try {
    std::string& event = scratch();
    begin_event(event, 'X', category, name, start, here());
    event += R"(,"dur":)";
    append_microseconds(event, end - start);
    end_event(event, args);
    record(event);
  } catch (...) {  // NOLINT(bugprone-empty-catch): best-effort, see class docs
  }
}

TracePoolScope::OnLane::OnLane(TraceLane lane) noexcept
    : m_previous(std::exchange(lent_row(), lane)) {}

TracePoolScope::OnLane::~OnLane() {
  lent_row() = m_previous;
}

TraceLane TraceLanePool::take(Tracer& tracer, std::uint32_t process) noexcept {
  try {
    const std::lock_guard lock(m_mutex);
    std::set<TraceLane>& free = m_free[process];
    if (const auto lowest = free.begin(); lowest != free.end()) {
      const TraceLane lane = *lowest;
      free.erase(lowest);
      return lane;
    }
  } catch (...) {  // NOLINT(bugprone-empty-catch): fall back to a fresh row
  }
  // Outside the lock, so naming the new row does not hold up the pool. Two
  // threads finding the pool empty at once simply make a row each, which is
  // what overlapping work needs anyway.
  return tracer.lane(process, m_name);
}

void TraceLanePool::give_back(TraceLane lane) noexcept {
  try {
    const std::lock_guard lock(m_mutex);
    m_free[lane.process].insert(lane);
  } catch (...) {  // NOLINT(bugprone-empty-catch): a lost row is only a row
  }
}

TraceLane Tracer::lane(std::uint32_t process, std::string_view name) noexcept {
  const TraceLane lane{.process = process, .row = next_tid()};
  name_row(lane, name);
  return lane;
}

void Tracer::begin(TraceLane lane,
                   std::string_view category,
                   const std::filesystem::path& name,
                   const TraceArgs& args) noexcept {
  try {
    std::string& event = scratch();
    begin_event(event, 'B', category, TraceText::of(name), Clock::now(), lane);
    end_event(event, args);
    record(event);
  } catch (...) {  // NOLINT(bugprone-empty-catch): best-effort, see class docs
  }
}

void Tracer::end(TraceLane lane,
                 std::string_view category,
                 const std::filesystem::path& name,
                 const TraceArgs& args) noexcept {
  try {
    std::string& event = scratch();
    begin_event(event, 'E', category, TraceText::of(name), Clock::now(), lane);
    end_event(event, args);
    record(event);
  } catch (...) {  // NOLINT(bugprone-empty-catch): best-effort, see class docs
  }
}

void Tracer::flow_out(std::string_view category,
                      std::string_view name,
                      std::uint64_t id) noexcept {
  flow('s', here(), category, name, id);
}

void Tracer::flow_in(TraceLane lane,
                     std::string_view category,
                     std::string_view name,
                     std::uint64_t id) noexcept {
  flow('f', lane, category, name, id);
}

// `bp` binds each end of the flow to the span its thread or lane is inside,
// which is what the arrow is drawn between.
void Tracer::flow(char phase,
                  TraceLane row,
                  std::string_view category,
                  std::string_view name,
                  std::uint64_t id) noexcept {
  try {
    std::string& event = scratch();
    begin_event(event, phase, category, name, Clock::now(), row);
    event += R"(,"bp":"e","id":)";
    append_number(event, id);
    event += "}\n";
    record(event);
  } catch (...) {  // NOLINT(bugprone-empty-catch): best-effort, see class docs
  }
}

void Tracer::name_thread(std::string_view name) noexcept {
  name_row(here(), name);
}

bool Tracer::knows_process(std::uint32_t id) const noexcept {
  try {
    const std::lock_guard lock(m_mutex);
    return m_process_names.contains(id);
  } catch (...) {  // NOLINT(bugprone-empty-catch): best-effort, see class docs
    return false;
  }
}

void Tracer::name_process(std::uint32_t id, std::string_view name) noexcept {
  if (name.empty()) {
    return;
  }
  try {
    {
      // The first name a process is given sticks, so a later lookup that only
      // manages a worse one cannot overwrite it.
      const std::lock_guard lock(m_mutex);
      if (m_process_names.contains(id)) {
        return;
      }
    }

    std::string event = R"({"ph":"M","name":"process_name","pid":)";
    append_number(event, id);
    event += R"(,"args":{"name":)";
    append_json_string(event, name);
    event += "}}\n";

    const std::lock_guard lock(m_mutex);
    m_process_names.emplace(id, std::move(event));
  } catch (...) {  // NOLINT(bugprone-empty-catch): best-effort, see class docs
  }
}

void Tracer::name_row(TraceLane row, std::string_view name) noexcept {
  try {
    std::string event = R"({"ph":"M","name":"thread_name","pid":)";
    append_number(event, row.process);
    event += R"(,"tid":)";
    append_number(event, row.row);
    event += R"(,"args":{"name":)";
    append_json_string(event, name);
    event += "}}\n";

    const std::lock_guard lock(m_mutex);
    m_row_names.insert_or_assign(row, std::move(event));
  } catch (...) {  // NOLINT(bugprone-empty-catch): best-effort, see class docs
  }
}

void Tracer::record(std::string_view event) {
  const std::lock_guard lock(m_mutex);
  if (m_slices.empty() ||
      m_slices.back().size() + event.size() > m_slice_size) {
    m_slices.emplace_back().reserve(std::max(m_slice_size, event.size()));
  }
  m_slices.back() += event;
  m_size += event.size();
  // The newest slice always stays, so the latest event survives even if it
  // alone is over capacity.
  while (m_size > m_capacity && m_slices.size() > 1) {
    m_size -= m_slices.front().size();
    m_slices.pop_front();
  }
}

std::string Tracer::snapshot() const {
  constexpr std::string_view k_tail = "]\n";

  const std::lock_guard lock(m_mutex);
  std::size_t size = 1 + m_size + k_tail.size();
  for (const auto& [id, event] : m_process_names) {
    size += event.size() + 1;
  }
  for (const auto& [row, event] : m_row_names) {
    size += event.size() + 1;
  }

  // The names lead, so they survive the oldest events being dropped, and the
  // events that follow each bring the `,` that separates them. There is always
  // at least makebelieve's own name to lead with.
  std::string trace;
  trace.reserve(size);
  trace += '[';
  const auto append_name = [&trace](const std::string& event) {
    if (trace.size() > 1) {
      trace += ',';
    }
    trace += event;
  };
  for (const auto& [id, event] : m_process_names) {
    append_name(event);
  }
  for (const auto& [row, event] : m_row_names) {
    append_name(event);
  }
  for (const std::string& slice : m_slices) {
    trace += slice;
  }
  trace += k_tail;
  return trace;
}

}  // namespace makebelieve
