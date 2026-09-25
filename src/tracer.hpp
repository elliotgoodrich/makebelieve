// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace makebelieve {

/// @class TraceProcess
/// A process in the trace: makebelieve itself, or one of the processes reading
/// the mount, which get a group each so a trace reads as who asked for what.
struct TraceProcess {
  /// The operating system's id for it, which keeps a client's group apart from
  /// makebelieve's own.
  std::uint32_t id = 0;

  /// What to call it, such as the name of its executable.
  std::string name;
};

/// @class TraceLane
/// A row in the trace: the process whose group it sits in, and which row of
/// that group it is. Rows are for work that is not tied to one thread.
/// @see Tracer::lane
struct TraceLane {
  std::uint32_t process = 0;
  std::uint32_t row = 0;

  [[nodiscard]] friend auto operator<=>(const TraceLane&,
                                        const TraceLane&) = default;
};

/// @class TraceText
/// @private
class TraceText {
  // The text @a values make, a space between each.
  template <typename... Values>
  [[nodiscard]] static std::string of(const Values&... values) {
    std::string out;
    std::string_view separator;
    ((out += separator, separator = " ", append(out, values)), ...);
    return out;
  }

  static void append(std::string& out, const std::filesystem::path& value);
  static void append(std::string& out, std::int64_t value);

  template <typename Value>
    requires std::convertible_to<const Value&, std::string_view>
  static void append(std::string& out, const Value& value) {
    out += std::string_view(value);
  }

  template <std::integral Value>
    requires(!std::same_as<Value, std::int64_t>)
  static void append(std::string& out, Value value) {
    if constexpr (std::same_as<Value, bool>) {
      out += value ? "true" : "false";
    } else {
      append(out, static_cast<std::int64_t>(value));
    }
  }

  friend class Tracer;
  friend class TraceArgs;
  friend class TraceScope;
};

/// @class TraceArgs
/// The `args` of a trace event: key/value pairs shown in the trace viewer's
/// details pane, rendered straight to JSON as they are added.
class TraceArgs {
  std::string m_json;

  // Adds the pair, taking @a value as it is spelled when @a literal says it
  // is already JSON, and as a string - escaped to survive JSON - otherwise.
  void add_value(std::string_view key, std::string_view value, bool literal);

 public:
  /// Adds @a key, whose value is @a values run together with a space between
  /// each. A value is a string, a path, a bool or an integer; a lone number or
  /// bool stays one in the JSON, and anything else becomes a string.
  template <typename... Values>
  void add(std::string_view key, const Values&... values) {
    static_assert(sizeof...(Values) > 0, "an argument needs a value");
    // A lone number or bool is spelled the way JSON spells it already.
    constexpr bool literal =
        sizeof...(Values) == 1 &&
        (... && std::integral<std::remove_cvref_t<Values>>);
    add_value(key, TraceText::of(values...), literal);
  }

  /// The pairs added so far as the inside of a JSON object - without its
  /// braces - or empty if there are none.
  [[nodiscard]] const std::string& json() const { return m_json; }
};

/// @class Tracer
/// Records what makebelieve is doing as events in the Trace Event Format.
///
/// Events go into memory, most recent last, up to a capacity: past that, the
/// oldest are dropped a slice at a time, so a long-running mount always holds
/// its latest history. @link snapshot renders what is held as a JSON array.
///
/// Rather than calling this directly, code records through the `MB_TRACE_*`
/// macros below, which go to the process's @link g_tracer and cost next to
/// nothing when there is none.
///
/// Categories and argument keys are appended unescaped, so they must not
/// contain anything JSON would need escaped; names and argument values may be
/// anything.
class Tracer {
 public:
  using Clock = std::chrono::steady_clock;

 private:
  const Clock::time_point m_origin = Clock::now();

  // Our process id.
  const std::uint32_t m_process;
  const std::size_t m_capacity;

  // Events are stored in slices of about this many bytes, so dropping the
  // oldest is one pop rather than a shuffle of everything held.
  const std::size_t m_slice_size;

  mutable std::mutex m_mutex;

  // Rendered events, each starting with the `,` that separates it from the one
  // before and never split across slices, so dropping a slice leaves whole
  // events behind.
  std::deque<std::string> m_slices;

  // Bytes held across m_slices.
  std::size_t m_size = 0;

  struct RowHash {
    [[nodiscard]] std::size_t operator()(TraceLane row) const noexcept {
      return std::hash<std::uint64_t>{}(
          (static_cast<std::uint64_t>(row.process) << 32U) | row.row);
    }
  };

  // The rendered metadata events naming each process and each row, without
  // the `,` that separates them, since they lead the trace. In no particular
  // order: a viewer reads a name off the event, not off where it sits.
  std::unordered_map<std::uint32_t, std::string> m_process_names;
  std::unordered_map<TraceLane, std::string, RowHash> m_row_names;

 public:
  /// Creates a tracer holding up to about @a capacity bytes of events.
  explicit Tracer(std::size_t capacity = std::size_t{16} * 1024 * 1024);

  /// Records a span of work on the calling thread from @a start to @a end.
  void complete(std::string_view category,
                std::string_view name,
                Clock::time_point start,
                Clock::time_point end,
                const TraceArgs& args = {}) noexcept;

  /// Whether the process @a id has been named already, so a caller can skip
  /// working out a name it would only throw away.
  [[nodiscard]] bool knows_process(std::uint32_t id) const noexcept;

  /// Calls the process @a id @a name in the trace, unless it already has one
  /// or @a name is empty. Kept apart from the other events, so the name
  /// outlives the oldest events being dropped.
  void name_process(std::uint32_t id, std::string_view name) noexcept;

  /// Returns a new row called @a name in @a process's group, for work that is
  /// not tied to one thread. A build runs on whichever thread the runner picks
  /// and may finish on another, so it belongs to a row rather than a thread.
  [[nodiscard]] TraceLane lane(std::uint32_t process,
                               std::string_view name) noexcept;

  /// Begins a span on @a lane, named after the file @a name, and ended by
  /// @link end. Spans on one lane must not overlap.
  void begin(TraceLane lane,
             std::string_view category,
             const std::filesystem::path& name,
             const TraceArgs& args = {}) noexcept;

  /// Ends the span @link begin started on @a lane.
  void end(TraceLane lane,
           std::string_view category,
           const std::filesystem::path& name,
           const TraceArgs& args = {}) noexcept;

  /// Records the start of a flow, drawn as an arrow out of the span the
  /// calling thread is inside, to wherever @link flow_in picks @a id up.
  void flow_out(std::string_view category,
                std::string_view name,
                std::uint64_t id) noexcept;

  /// Records the end of the flow @a id, drawn as an arrow into the span
  /// @a lane is inside.
  void flow_in(TraceLane lane,
               std::string_view category,
               std::string_view name,
               std::uint64_t id) noexcept;

  /// Names the calling thread in the trace. Kept apart from the other events,
  /// so the name outlives the oldest events being dropped.
  void name_thread(std::string_view name) noexcept;

  /// Renders everything held as a JSON array of events: the process and thread
  /// names first, then the events, oldest first.
  [[nodiscard]] std::string snapshot() const;

 private:
  // The row the calling thread records on: the one it has been lent, else its
  // own thread's row in makebelieve's group.
  [[nodiscard]] TraceLane here() const noexcept;

  // Appends the start of an event - the part every phase shares - to @a out,
  // against @a row.
  void begin_event(std::string& out,
                   char phase,
                   std::string_view category,
                   std::string_view name,
                   Clock::time_point timestamp,
                   TraceLane row) const;

  // Records one end of a flow against @a row.
  void flow(char phase,
            TraceLane row,
            std::string_view category,
            std::string_view name,
            std::uint64_t id) noexcept;

  // Names @a row, unless it already has a name.
  void name_row(TraceLane row, std::string_view name) noexcept;

  // Stores one rendered event, dropping the oldest to stay within capacity.
  void record(std::string_view event);
};

/// The tracer events are recorded to, or null when tracing is off.
/// @pre Only changed while no other thread can be recording, such as before
/// the threads that record are started.
inline Tracer* g_tracer = nullptr;  // NOLINT

/// @class TracerInstallation
/// RAII guard for installing a tracer to @link g_tracer.
/// @pre Constructed and destroyed only while no other thread can be recording.
class TracerInstallation {
 public:
  explicit TracerInstallation(Tracer& tracer) { g_tracer = &tracer; }
  ~TracerInstallation() { g_tracer = nullptr; }

  TracerInstallation(const TracerInstallation&) = delete;
  TracerInstallation& operator=(const TracerInstallation&) = delete;
  TracerInstallation(TracerInstallation&&) = delete;
  TracerInstallation& operator=(TracerInstallation&&) = delete;
};

/// @class TraceLanePool
/// Interchangeable rows for work that comes and goes: a request being served,
/// a build being run. A row is taken for as long as the work lasts and handed
/// back after, so a second one only appears when work really does overlap, and
/// a group holds as many rows as it ever had work running at once rather than
/// one per thing worked on.
///
/// Every row of a pool has the same name, since nothing distinguishes one from
/// another beyond what is on it; a viewer tells them apart by the row id it
/// shows beside the name anyway.
/// Thread-safe.
class TraceLanePool {
  const std::string m_name;

  mutable std::mutex m_mutex;

  // Rows are kept per process, so one process's work never lands on a row in
  // another's group. The free rows of one process stay ordered, since a lane
  // is taken from the lowest of them.
  std::unordered_map<std::uint32_t, std::set<TraceLane>> m_free;

 public:
  /// Creates a pool whose rows are all called @a name.
  explicit TraceLanePool(std::string name) : m_name(std::move(name)) {}

  /// Takes the lowest row in @a process's group that nothing is using, making
  /// one if they are all busy.
  [[nodiscard]] TraceLane take(Tracer& tracer, std::uint32_t process) noexcept;

  /// Hands @a lane back for the next piece of work to take.
  /// @pre Whatever @a lane was taken for has finished recording on it.
  void give_back(TraceLane lane) noexcept;
};

/// @class TraceScope
/// Records a complete event covering the rest of its life, from the moment it
/// is opened. A scope that is never opened records nothing, which is how
/// `MB_TRACE_SCOPE` - the way to make one - costs nothing when tracing is off:
/// it opens the scope only when there is a tracer, so nothing a name or an
/// argument is made of is worked out without one.
class TraceScope {
  Tracer* m_tracer = nullptr;
  std::string_view m_category;

  // Held here when the name was built rather than a literal, with m_name
  // viewing it.
  std::string m_owned_name;
  std::string_view m_name;

  Tracer::Clock::time_point m_start;
  TraceArgs m_args;

  // Keeps a literal as a view, and anything built - a string, or pieces to
  // run together, as `std::tie("open", path)` gives - as a copy.
  template <typename Name>
  void hold_name(Name&& name) {
    using Held = std::remove_cvref_t<Name>;
    if constexpr (requires { std::tuple_size<Held>::value; }) {
      m_owned_name = std::apply(
          [](const auto&... parts) { return TraceText::of(parts...); }, name);
      m_name = m_owned_name;
    } else if constexpr (std::same_as<Held, std::string>) {
      m_owned_name = std::forward<Name>(name);
      m_name = m_owned_name;
    } else {
      m_name = name;
    }
  }

 public:
  TraceScope() = default;

  /// Starts recording a span called @a name - a string literal, a string built
  /// for the occasion, or pieces to run together, as `std::tie("open", path)`
  /// gives - with @a values as its one argument, if any (see
  /// @link TraceArgs::add).
  /// @pre There is a tracer, and this scope has not been opened already.
  /// @pre @a category outlives this object, as does a name it views.
  template <typename Name, typename... Values>
  void open(std::string_view category, Name&& name, const Values&... values) {
    m_tracer = g_tracer;
    m_category = category;
    hold_name(std::forward<Name>(name));
    if constexpr (sizeof...(Values) > 0) {
      m_args.add(values...);
    }
    m_start = Tracer::Clock::now();
  }

  ~TraceScope() {
    if (m_tracer != nullptr) {
      m_tracer->complete(m_category, m_name, m_start, Tracer::Clock::now(),
                         m_args);
    }
  }

  TraceScope(const TraceScope&) = delete;
  TraceScope& operator=(const TraceScope&) = delete;
  TraceScope(TraceScope&&) = delete;
  TraceScope& operator=(TraceScope&&) = delete;
};

/// @class TracePoolScope
/// A @link TraceScope on a row borrowed from a pool for the length of the
/// scope, which anything the scope goes on to trace lands on too. Opened the
/// same way, by `MB_TRACE_POOL_SCOPE`, and likewise records nothing until it
/// is.
class TracePoolScope {
  // Records everything the calling thread traces on the row instead of on the
  // thread itself, for as long as it lives, so the work the scope goes on to
  // do carries along with it.
  class OnLane {
    TraceLane m_previous;

   public:
    explicit OnLane(TraceLane lane) noexcept;
    ~OnLane();

    OnLane(const OnLane&) = delete;
    OnLane& operator=(const OnLane&) = delete;
    OnLane(OnLane&&) = delete;
    OnLane& operator=(OnLane&&) = delete;
  };

  Tracer* m_tracer = nullptr;
  TraceLanePool* m_pool = nullptr;
  TraceLane m_lane{};

  // Only alive while there is a tracer, since without one there is no row.
  std::optional<OnLane> m_on_lane;
  std::optional<TraceScope> m_scope;

 public:
  TracePoolScope() = default;

  /// Takes a row from @a pool in @a process's group, then records as
  /// @link TraceScope::open does.
  /// @pre There is a tracer, and this scope has not been opened already.
  template <typename... Rest>
  void open(TraceLanePool& pool,
            const TraceProcess& process,
            std::string_view category,
            Rest&&... rest) {
    m_tracer = g_tracer;
    m_pool = &pool;
    m_tracer->name_process(process.id, process.name);
    m_lane = m_pool->take(*m_tracer, process.id);
    m_on_lane.emplace(m_lane);
    m_scope.emplace().open(category, std::forward<Rest>(rest)...);
  }

  ~TracePoolScope() {
    // The span is closed, and the thread put back on its own row, before the
    // row is handed on: nothing else may record on it in between.
    m_scope.reset();
    m_on_lane.reset();
    if (m_tracer != nullptr) {
      m_pool->give_back(m_lane);
    }
  }

  TracePoolScope(const TracePoolScope&) = delete;
  TracePoolScope& operator=(const TracePoolScope&) = delete;
  TracePoolScope(TracePoolScope&&) = delete;
  TracePoolScope& operator=(TracePoolScope&&) = delete;
};

}  // namespace makebelieve

// The macros below record to `makebelieve::g_tracer`. Each takes a category
// and name, and optionally an argument's key and value (see `TraceArgs::add`).
// A scope is opened only when there is a tracer, so nothing but the category
// is evaluated without one and a name or argument may be as costly to work out
// as it likes.
//
// The scope macros declare a variable and then open it, so they belong in a
// block of their own rather than as the unbraced body of an `if` or a loop.

#define MB_TRACE_CONCAT_IMPL(a, b) a##b
#define MB_TRACE_CONCAT(a, b) MB_TRACE_CONCAT_IMPL(a, b)

// Records the rest of the enclosing scope as a complete event. The category
// must outlive the scope.
#define MB_TRACE_SCOPE(category, name, ...)                             \
  ::makebelieve::TraceScope MB_TRACE_CONCAT(mb_trace_scope_, __LINE__); \
  if (::makebelieve::g_tracer != nullptr) {                             \
    MB_TRACE_CONCAT(mb_trace_scope_, __LINE__)                          \
        .open((category), (name)__VA_OPT__(, ) __VA_ARGS__);            \
  }

// As MB_TRACE_SCOPE, on a row borrowed from `pool` for the scope, in the group
// of the `process` (a `TraceProcess`) the work belongs to.
#define MB_TRACE_POOL_SCOPE(pool, process, category, name, ...)       \
  ::makebelieve::TracePoolScope MB_TRACE_CONCAT(mb_trace_pool_scope_, \
                                                __LINE__);            \
  if (::makebelieve::g_tracer != nullptr) {                           \
    MB_TRACE_CONCAT(mb_trace_pool_scope_, __LINE__)                   \
        .open((pool), (process), (category),                          \
              (name)__VA_OPT__(, ) __VA_ARGS__);                      \
  }

// Names the calling thread.
#define MB_TRACE_THREAD_NAME(name)                     \
  do {                                                 \
    if (::makebelieve::Tracer* const mb_trace_tracer = \
            ::makebelieve::g_tracer) {                 \
      mb_trace_tracer->name_thread(name);              \
    }                                                  \
  } while (false)
