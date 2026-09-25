// SPDX-License-Identifier: MIT
#include "tracer.hpp"

#include "processutil.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace makebelieve;

// A strict JSON checker, just enough to say whether a trace would load.
class JsonChecker {
  std::string_view m_text;
  std::size_t m_pos = 0;

  void skip_space() {
    while (m_pos < m_text.size() &&
           std::string_view(" \t\r\n").contains(m_text[m_pos])) {
      ++m_pos;
    }
  }

  bool consume(char c) {
    skip_space();
    if (m_pos < m_text.size() && m_text[m_pos] == c) {
      ++m_pos;
      return true;
    }
    return false;
  }

  bool literal(std::string_view word) {
    if (m_text.substr(m_pos).starts_with(word)) {
      m_pos += word.size();
      return true;
    }
    return false;
  }

  bool string() {
    if (!consume('"')) {
      return false;
    }
    while (m_pos < m_text.size()) {
      const auto c = static_cast<unsigned char>(m_text[m_pos++]);
      if (c == '"') {
        return true;
      }
      if (c < 0x20) {
        return false;
      }
      if (c == '\\') {
        if (m_pos >= m_text.size()) {
          return false;
        }
        const char escaped = m_text[m_pos++];
        if (escaped == 'u') {
          if (m_pos + 4 > m_text.size()) {
            return false;
          }
          for (int i = 0; i < 4; ++i) {
            if (!std::isxdigit(static_cast<unsigned char>(m_text[m_pos++]))) {
              return false;
            }
          }
        } else if (!std::string_view("\"\\/bfnrt").contains(escaped)) {
          return false;
        }
      }
    }
    return false;
  }

  bool number() {
    const std::size_t start = m_pos;
    static_cast<void>(literal("-"));
    while (m_pos < m_text.size() &&
           std::string_view("0123456789.eE+-").contains(m_text[m_pos])) {
      ++m_pos;
    }
    return m_pos > start;
  }

  bool value() {
    skip_space();
    if (m_pos >= m_text.size()) {
      return false;
    }
    switch (m_text[m_pos]) {
      case '{':
        return object();
      case '[':
        return array();
      case '"':
        return string();
      case 't':
        return literal("true");
      case 'f':
        return literal("false");
      case 'n':
        return literal("null");
      default:
        return number();
    }
  }

  bool object() {
    consume('{');
    if (consume('}')) {
      return true;
    }
    do {
      if (!string() || !consume(':') || !value()) {
        return false;
      }
    } while (consume(','));
    return consume('}');
  }

  bool array() {
    consume('[');
    if (consume(']')) {
      return true;
    }
    do {
      if (!value()) {
        return false;
      }
    } while (consume(','));
    return consume(']');
  }

 public:
  static bool is_valid(std::string_view text) {
    JsonChecker checker;
    checker.m_text = text;
    if (!checker.value()) {
      return false;
    }
    checker.skip_space();
    return checker.m_pos == text.size();
  }
};

std::size_t count(std::string_view text, std::string_view needle) {
  std::size_t found = 0;
  for (std::size_t pos = text.find(needle); pos != std::string_view::npos;
       pos = text.find(needle, pos + needle.size())) {
    ++found;
  }
  return found;
}

// The JSON an argument's value comes out as, for comparing escapes.
std::string rendered(std::string_view value) {
  TraceArgs args;
  args.add("k", value);
  const std::string_view json = args.json();
  return std::string(json.substr(json.find(':') + 1));
}

TEST(TracerTest, AnEmptyTraceIsValidJson) {
  const Tracer tracer;
  const std::string trace = tracer.snapshot();
  EXPECT_TRUE(JsonChecker::is_valid(trace)) << trace;
  EXPECT_TRUE(trace.contains(R"("args":{"name":"makebelieve"})"));
}

TEST(TracerTest, RecordsACompleteEventWithItsArguments) {
  Tracer tracer;
  const Tracer::Clock::time_point start = Tracer::Clock::now();
  TraceArgs args;
  args.add("path", "a.txt");
  args.add("size", 42);
  args.add("ok", true);
  tracer.complete("vfs", "open", start, start + std::chrono::microseconds(5),
                  args);
  tracer.name_thread("worker");

  const std::string trace = tracer.snapshot();
  EXPECT_TRUE(JsonChecker::is_valid(trace)) << trace;
  EXPECT_TRUE(trace.contains(R"({"ph":"X","cat":"vfs","name":"open","pid":)" +
                             std::to_string(ProcessUtil::self())));
  EXPECT_TRUE(trace.contains(
      R"("dur":5.000,"args":{"path":"a.txt","size":42,"ok":true}})"));
  EXPECT_TRUE(trace.contains(R"("name":"thread_name")"));
  EXPECT_TRUE(trace.contains(R"("args":{"name":"worker"})"));
}

// An argument's value can be made of several pieces, which run together with a
// space between each, as a name can.
TEST(TracerTest, RunsSeveralValuesTogetherIntoOne) {
  TraceArgs args;
  args.add("what", "open", std::filesystem::path("a") / "b.txt");
  args.add("how many", 3);
  EXPECT_EQ(args.json(), R"("what":"open a/b.txt","how many":3)");

  // The root of a tree is a path with nothing in it, which still has to read
  // as something.
  TraceArgs root;
  root.add("path", std::filesystem::path());
  EXPECT_EQ(root.json(), R"("path":"/")");
}

// A lane is a row of its own, and a flow is the pair of events an arrow is
// drawn between: out of the span the calling thread is in, into the span on
// the lane.
TEST(TracerTest, RecordsALaneWithAFlowIntoIt) {
  Tracer tracer;
  const TraceLane lane = tracer.lane(ProcessUtil::self(), "out.txt");
  const Tracer::Clock::time_point start = Tracer::Clock::now();

  tracer.complete("vfs", "open out.txt", start, Tracer::Clock::now());
  tracer.flow_out("build", "build", 7);
  tracer.begin(lane, "build", "out.txt");
  tracer.flow_in(lane, "build", "build", 7);
  tracer.end(lane, "build", "out.txt");

  const std::string trace = tracer.snapshot();
  EXPECT_TRUE(JsonChecker::is_valid(trace)) << trace;
  EXPECT_TRUE(trace.contains(R"("args":{"name":"out.txt"})")) << trace;
  EXPECT_TRUE(trace.contains(R"("ph":"B","cat":"build","name":"out.txt")"));
  EXPECT_TRUE(trace.contains(R"("ph":"E","cat":"build","name":"out.txt")"));
  EXPECT_TRUE(trace.contains(R"("ph":"s","cat":"build","name":"build")"));
  EXPECT_TRUE(trace.contains(R"("ph":"f","cat":"build","name":"build")"));
  EXPECT_EQ(count(trace, R"("bp":"e","id":7)"), 2U);

  // The lane is a row of its own, so its spans are on neither the calling
  // thread nor each other's.
  EXPECT_NE(lane.row, 0U);
  EXPECT_EQ(count(trace, R"("tid":)" + std::to_string(lane.row)),
            4U);  // the lane's name, its begin and end, and the flow in
}

// Rows belong to the process whose work they carry, so a trace reads as who
// asked for what: makebelieve's own group, and one per process reading it.
TEST(TracerTest, RowsBelongToTheProcessTheyCarryWorkFor) {
  Tracer tracer;
  constexpr std::uint32_t k_client = 4321;
  tracer.name_process(k_client, "reader.exe");
  const TraceLane lane = tracer.lane(k_client, "reader");
  tracer.begin(lane, "vfs", "open a.txt");
  tracer.end(lane, "vfs", "open a.txt");
  const Tracer::Clock::time_point now = Tracer::Clock::now();
  tracer.complete("build", "ours", now, now);

  const std::string trace = tracer.snapshot();
  EXPECT_TRUE(JsonChecker::is_valid(trace)) << trace;
  EXPECT_NE(ProcessUtil::self(), k_client);
  EXPECT_TRUE(trace.contains(R"("name":"process_name","pid":)" +
                             std::to_string(k_client) +
                             R"(,"args":{"name":"reader.exe"})"))
      << trace;
  EXPECT_TRUE(trace.contains(R"("name":"process_name","pid":)" +
                             std::to_string(ProcessUtil::self()) +
                             R"(,"args":{"name":"makebelieve"})"))
      << trace;
  // The client's span sits under the client, and ours under us.
  EXPECT_TRUE(trace.contains(R"("name":"open a.txt","pid":)" +
                             std::to_string(k_client)))
      << trace;
  EXPECT_TRUE(trace.contains(R"("name":"ours","pid":)" +
                             std::to_string(ProcessUtil::self())))
      << trace;

  // A process keeps the first name it was given, and says so, which is how a
  // caller knows not to go looking one up.
  EXPECT_TRUE(tracer.knows_process(k_client));
  EXPECT_FALSE(tracer.knows_process(k_client + 1));
  tracer.name_process(k_client, "something else");
  EXPECT_FALSE(tracer.snapshot().contains("something else"));

  // A name nobody managed to work out leaves the process unnamed rather than
  // naming it nothing.
  tracer.name_process(k_client + 1, "");
  EXPECT_FALSE(tracer.knows_process(k_client + 1));
}

// A pool hands out the lowest row nothing is using, so rows only multiply
// when work overlaps.
TEST(TracerTest, ALanePoolReusesItsRows) {
  Tracer tracer;
  TraceLanePool pool("client");

  const TraceLane first = pool.take(tracer, ProcessUtil::self());
  const TraceLane second = pool.take(tracer, ProcessUtil::self());
  EXPECT_NE(first, second);

  pool.give_back(first);
  EXPECT_EQ(pool.take(tracer, ProcessUtil::self()), first);

  pool.give_back(second);
  pool.give_back(first);
  EXPECT_EQ(pool.take(tracer, ProcessUtil::self()), std::min(first, second));

  // Another process's work gets rows of its own rather than sharing these.
  constexpr std::uint32_t k_other = 99;
  const TraceLane elsewhere = pool.take(tracer, k_other);
  EXPECT_EQ(elsewhere.process, k_other);
  EXPECT_NE(elsewhere.row, first.row);
  EXPECT_NE(elsewhere.row, second.row);

  // Every row of a pool goes by the same name, and only the two that were
  // ever needed at once were made.
  const std::string trace = tracer.snapshot();
  EXPECT_EQ(count(trace, R"("args":{"name":"client"})"), 3U)
      << trace;  // two here, and one in the other process's group
}

// Work handed to a pooled row takes whatever it traces along with it, so a
// request and the build it waits for read as one worker.
TEST(TracerTest, WhatAScopeOnALaneTracesLandsOnThatLane) {
  Tracer tracer;
  const TracerInstallation installed(tracer);
  TraceLanePool pool("client");

  const std::filesystem::path opened("a.txt");
  {
    MB_TRACE_POOL_SCOPE(pool, (TraceProcess{.id = 4321, .name = "reader.exe"}),
                        "vfs", std::tie("open", opened));
    MB_TRACE_SCOPE("vfs", "inside");
  }
  MB_TRACE_SCOPE("vfs", "outside");

  const std::string trace = tracer.snapshot();
  EXPECT_TRUE(JsonChecker::is_valid(trace)) << trace;
  EXPECT_TRUE(trace.contains(R"("args":{"name":"reader.exe"})")) << trace;
  EXPECT_TRUE(trace.contains(R"("args":{"name":"client"})")) << trace;
  const std::string lane =
      R"("tid":)" + std::to_string(pool.take(tracer, 4321).row);
  EXPECT_TRUE(trace.contains(R"("name":"open a.txt","pid":4321,)" + lane))
      << trace;
  EXPECT_TRUE(trace.contains(R"("name":"inside","pid":4321,)" + lane)) << trace;
  EXPECT_FALSE(trace.contains(R"("name":"outside","pid":4321,)" + lane))
      << trace;
}

TEST(TracerTest, EscapesStringsIntoValidJson) {
  EXPECT_EQ(rendered("plain"), R"("plain")");
  EXPECT_EQ(rendered(R"(say "hi" \ bye)"), R"("say \"hi\" \\ bye")");
  EXPECT_EQ(rendered("line\nnext\ttab\r"), R"("line\nnext\ttab\r")");
  EXPECT_EQ(rendered(std::string_view("\x01\x1f", 2)), R"("\u0001\u001f")");
  // Valid UTF-8 passes through as it is.
  EXPECT_EQ(rendered("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80"),
            "\"caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80\"");
  // Anything else is replaced, a byte at a time: a stray continuation byte, a
  // truncated sequence, an overlong encoding and a UTF-16 surrogate.
  EXPECT_EQ(rendered("a\x80z"), R"("a\ufffdz")");
  EXPECT_EQ(rendered("a\xe2\x82"), R"("a\ufffd\ufffd")");
  EXPECT_EQ(rendered("\xc0\xaf"), R"("\ufffd\ufffd")");
  EXPECT_EQ(rendered("\xed\xa0\x80"), R"("\ufffd\ufffd\ufffd")");

  Tracer tracer;
  TraceArgs args;
  args.add("bytes", std::string_view("\"\\\n\xff\0", 5));
  const Tracer::Clock::time_point now = Tracer::Clock::now();
  tracer.complete("test", "name with \"quotes\"", now, now, args);
  const std::string trace = tracer.snapshot();
  EXPECT_TRUE(JsonChecker::is_valid(trace)) << trace;
}

TEST(TracerTest, PathsUseForwardSlashes) {
  TraceArgs args;
  args.add("path", std::filesystem::path("a") / "b" / "c.txt");
  EXPECT_EQ(args.json(), R"("path":"a/b/c.txt")");
}

TEST(TracerTest, TimestampsAreMicrosecondsToThreeDecimalPlaces) {
  Tracer tracer;
  const Tracer::Clock::time_point start = Tracer::Clock::now();
  tracer.complete("test", "span", start,
                  start + std::chrono::nanoseconds(1'234'567));
  EXPECT_TRUE(tracer.snapshot().contains(R"("dur":1234.567})"));
}

TEST(TracerTest, DropsTheOldestEventsPastCapacity) {
  constexpr std::size_t k_capacity = 4096;
  Tracer tracer(k_capacity);
  tracer.name_thread("survivor");
  const Tracer::Clock::time_point now = Tracer::Clock::now();
  for (int i = 0; i < 1000; ++i) {
    TraceArgs args;
    args.add("index", i);
    tracer.complete("test", "event", now, now, args);
  }

  const std::string trace = tracer.snapshot();
  EXPECT_TRUE(JsonChecker::is_valid(trace)) << trace;
  EXPECT_LE(trace.size(), 2 * k_capacity);
  EXPECT_FALSE(trace.contains(R"("index":0})"));
  EXPECT_TRUE(trace.contains(R"("index":999})"));
  // Thread names are held apart, so dropping events does not lose them.
  EXPECT_TRUE(trace.contains(R"("args":{"name":"survivor"})"));
}

TEST(TracerTest, KeepsAnEventLargerThanTheCapacity) {
  Tracer tracer(16);
  TraceArgs args;
  args.add("big", std::string(100, 'x'));
  const Tracer::Clock::time_point now = Tracer::Clock::now();
  tracer.complete("test", "first", now, now, args);
  tracer.complete("test", "second", now, now, args);

  const std::string trace = tracer.snapshot();
  EXPECT_TRUE(JsonChecker::is_valid(trace)) << trace;
  EXPECT_FALSE(trace.contains(R"("name":"first")"));
  EXPECT_TRUE(trace.contains(R"("name":"second")"));
}

TEST(TracerTest, RecordsFromManyThreadsAtOnce) {
  constexpr int k_threads = 4;
  constexpr int k_events = 1000;
  Tracer tracer;
  std::vector<std::thread> threads;
  for (int t = 0; t < k_threads; ++t) {
    threads.emplace_back([&tracer] {
      tracer.name_thread("recorder");
      const Tracer::Clock::time_point now = Tracer::Clock::now();
      for (int i = 0; i < k_events; ++i) {
        tracer.complete("test", "event", now, now);
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  const std::string trace = tracer.snapshot();
  EXPECT_TRUE(JsonChecker::is_valid(trace));
  EXPECT_EQ(count(trace, R"("name":"event")"),
            std::size_t{k_threads * k_events});
  EXPECT_EQ(count(trace, R"("name":"recorder")"), std::size_t{k_threads});
}

TEST(TracerTest, TheMacrosRecordToTheInstalledTracer) {
  Tracer tracer;
  {
    const TracerInstallation installed(tracer);
    EXPECT_EQ(g_tracer, &tracer);
    {
      MB_TRACE_SCOPE("test", "scope", "path", std::filesystem::path("x"));
      {
        // A name may be made of pieces, held by reference until there is
        // something to record them to.
        const std::filesystem::path opened("x.txt");
        MB_TRACE_SCOPE("test", std::tie("open", opened));
      }
      MB_TRACE_THREAD_NAME("macro thread");
    }
  }
  EXPECT_EQ(g_tracer, nullptr);

  const std::string trace = tracer.snapshot();
  EXPECT_TRUE(JsonChecker::is_valid(trace)) << trace;
  EXPECT_TRUE(trace.contains(R"("ph":"X","cat":"test","name":"scope")"));
  EXPECT_TRUE(trace.contains(R"("args":{"path":"x"}})"));
  EXPECT_TRUE(trace.contains(R"("ph":"X","cat":"test","name":"open x.txt")"));
  EXPECT_TRUE(trace.contains(R"("args":{"name":"macro thread"})"));
}

// Without a tracer the macros evaluate nothing beyond a scope's category, so a
// name or an argument can be as costly as it likes.
TEST(TracerTest, TheMacrosEvaluateNoArgumentsWithoutATracer) {
  ASSERT_EQ(g_tracer, nullptr);
  int evaluated = 0;
  const auto costly = [&evaluated] {
    ++evaluated;
    return 1;
  };
  {
    MB_TRACE_SCOPE("test", "scope " + std::to_string(costly()), "value",
                   costly());
    TraceLanePool pool("client");
    MB_TRACE_POOL_SCOPE(
        pool, (TraceProcess{.id = static_cast<std::uint32_t>(costly())}),
        "test", "pooled " + std::to_string(costly()));
  }
  EXPECT_EQ(evaluated, 0);
}

}  // namespace
