// SPDX-License-Identifier: MIT
#include "buildcache.hpp"

#include "directorytreeutil.hpp"
#include "filesystemutil.hpp"
#include "openedfileutil.hpp"
#include "scopeguard.hpp"

#include <sqlite3.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace makebelieve {

namespace {

// Bumped whenever the tables change shape. A cache of any other version is
// emptied rather than migrated: it only saves rebuilding.
constexpr int k_schema_version = 3;

constexpr std::string_view k_schema = R"sql(
CREATE TABLE outputs(
  path TEXT PRIMARY KEY,
  action TEXT NOT NULL,
  command TEXT NOT NULL,
  hash TEXT NOT NULL,
  mtime INTEGER NOT NULL
);
CREATE INDEX outputs_by_hash ON outputs(hash);
CREATE TABLE inputs(
  output TEXT NOT NULL,
  path TEXT NOT NULL,
  kind INTEGER NOT NULL,
  mtime INTEGER NOT NULL,
  size INTEGER NOT NULL,
  hash TEXT NOT NULL,
  PRIMARY KEY(output, path)
);
)sql";

class SqliteCategory final : public std::error_category {
 public:
  [[nodiscard]] const char* name() const noexcept override { return "sqlite"; }
  [[nodiscard]] std::string message(int code) const override {
    return sqlite3_errstr(code);
  }
};

const std::error_category& sqlite_category() {
  static const SqliteCategory category;
  return category;
}

// Throws the error @a code from @a db, saying what was being @a done.
[[noreturn]] void fail(sqlite3* db, int code, std::string_view done) {
  const char* const detail =
      db != nullptr ? sqlite3_errmsg(db) : sqlite3_errstr(code);
  throw std::system_error(code, sqlite_category(),
                          std::format("build cache: {}: {}", done, detail));
}

// Runs @a sql, which returns no rows, or throws.
void execute(sqlite3* db, const char* sql) {
  if (const int code = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
      code != SQLITE_OK) {
    fail(db, code, sql);
  }
}

// One prepared statement, finalized when it goes.
class Statement {
  sqlite3* m_db;
  sqlite3_stmt* m_statement = nullptr;

 public:
  Statement(sqlite3* db, std::string_view sql) : m_db(db) {
    if (const int code =
            sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()),
                               &m_statement, nullptr);
        code != SQLITE_OK) {
      fail(db, code, sql);
    }
  }

  ~Statement() { sqlite3_finalize(m_statement); }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  Statement(Statement&&) = delete;
  Statement& operator=(Statement&&) = delete;

  // Binds @a values to the parameters, in order, ready to step afresh.
  template <class... Values>
  Statement& bind(const Values&... values) {
    sqlite3_reset(m_statement);
    sqlite3_clear_bindings(m_statement);
    int index = 0;
    (bind_one(++index, values), ...);
    return *this;
  }

  // Moves to the next row, returning false once there are none.
  bool step() {
    const int code = sqlite3_step(m_statement);
    if (code == SQLITE_ROW) {
      return true;
    }
    if (code == SQLITE_DONE) {
      return false;
    }
    fail(m_db, code, sqlite3_sql(m_statement));
  }

  // Runs a statement that returns no rows.
  void run() {
    while (step()) {
    }
  }

  [[nodiscard]] std::string text(int column) const {
    const auto* const data = sqlite3_column_text(m_statement, column);
    const int size = sqlite3_column_bytes(m_statement, column);
    return data == nullptr ? std::string()
                           : std::string(reinterpret_cast<const char*>(data),
                                         static_cast<std::size_t>(size));
  }

  [[nodiscard]] std::int64_t integer(int column) const {
    return sqlite3_column_int64(m_statement, column);
  }

 private:
  void bind_one(int index, std::string_view value) {
    check(sqlite3_bind_text(m_statement, index, value.data(),
                            static_cast<int>(value.size()), SQLITE_TRANSIENT));
  }
  void bind_one(int index, const std::string& value) {
    bind_one(index, std::string_view(value));
  }
  void bind_one(int index, std::int64_t value) {
    check(sqlite3_bind_int64(m_statement, index, value));
  }

  void check(int code) {
    if (code != SQLITE_OK) {
      fail(m_db, code, sqlite3_sql(m_statement));
    }
  }
};

// A transaction that rolls back unless committed.
class Transaction {
  sqlite3* m_db;
  bool m_done = false;

 public:
  explicit Transaction(sqlite3* db) : m_db(db) { execute(db, "BEGIN"); }

  ~Transaction() {
    if (!m_done) {
      sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
  }

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  Transaction(Transaction&&) = delete;
  Transaction& operator=(Transaction&&) = delete;

  void commit() {
    execute(m_db, "COMMIT");
    m_done = true;
  }
};

// A path as stored: UTF-8 whatever the platform, with `/` between parts.
std::string to_text(const std::filesystem::path& path) {
  const std::u8string text = path.generic_u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path from_text(std::string_view text) {
  return {std::u8string(reinterpret_cast<const char8_t*>(text.data()),
                        text.size())};
}

std::string_view to_text(Manifest::Action action) {
  switch (action) {
    case Manifest::Action::Run:
      return "run";
    case Manifest::Action::Capture:
      return "capture";
    case Manifest::Action::Copy:
      return "copy";
    case Manifest::Action::Tracing:
      return "tracing";
    case Manifest::Action::Status:
      return "status";
  }
  return "unknown";
}

std::optional<Manifest::Action> action_from_text(std::string_view text) {
  for (const Manifest::Action action : {
           Manifest::Action::Run,
           Manifest::Action::Capture,
           Manifest::Action::Copy,
           Manifest::Action::Tracing,
           Manifest::Action::Status,
       }) {
    if (to_text(action) == text) {
      return action;
    }
  }
  return std::nullopt;
}

// How much of a file BuildCache::hash reads at a time.
constexpr std::size_t k_hash_piece_bytes = std::size_t{1} << 20;

// Formats @a hash as BuildCache names content by it.
std::string to_text(const XXH128_hash_t& hash) {
  return std::format("{:016x}{:016x}", hash.high64, hash.low64);
}

// Whether @a hash could have come from BuildCache::hash, so it is safe to
// name a file by.
bool is_hash(std::string_view hash) {
  return hash.size() == 32 && std::ranges::all_of(hash, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

// Marks @a directory as a cache: hidden, ignored by git, and tagged for backup
// tools that skip caches (https://bford.info/cachedir/). Best-effort, as the
// cache works the same without any of it.
void mark_as_cache(const std::filesystem::path& directory) {
  (void)FileSystemUtil::hide(directory);
  // Left as they are if already there.
  const auto write_once = [&directory](std::string_view name,
                                       std::string_view contents) {
    const std::filesystem::path path = directory / name;
    std::error_code error;
    if (!std::filesystem::exists(path, error) && !error) {
      (void)FileSystemUtil::write_file(path, contents);
    }
  };
  write_once(".gitignore", "# Created by makebelieve.\n*\n");
  write_once("CACHEDIR.TAG",
             "Signature: 8a477f597d28d172789f06886806bc55\n"
             "# This file is a cache directory tag created by makebelieve.\n"
             "# For information about cache directory tags, see:\n"
             "#\thttps://bford.info/cachedir/\n");
}

// How long a connection waits on another's brief hold of the records.
constexpr int k_busy_timeout_ms = 5000;

// Opens the database at @a path with @a flags, or throws.
sqlite3* open_database(const std::filesystem::path& path, int flags) {
  const std::string name = to_text(path);
  sqlite3* db = nullptr;
  if (const int code = sqlite3_open_v2(name.c_str(), &db,
                                       flags | SQLITE_OPEN_NOMUTEX, nullptr);
      code != SQLITE_OK) {
    // A handle comes back even on failure, to say why; it must still close.
    const ScopeGuard close([db] { sqlite3_close(db); });
    fail(db, code, std::format("opening {}", name));
  }
  return db;
}

// Every output recorded in @a db, with its inputs.
std::vector<BuildCache::Record> select_records(sqlite3* db) {
  std::vector<BuildCache::Record> records;
  std::unordered_map<std::string, std::size_t> by_output;

  Statement outputs(db,
                    "SELECT path, action, command, hash, mtime FROM outputs");
  while (outputs.step()) {
    const std::optional<Manifest::Action> action =
        action_from_text(outputs.text(1));
    if (!action.has_value()) {
      continue;  // nothing this version wrote, so nothing it can rebuild from
    }
    const std::string path = outputs.text(0);
    by_output.emplace(path, records.size());
    records.push_back(BuildCache::Record{
        .command = Command{.output = from_text(path),
                           .action = *action,
                           .text = outputs.text(2)},
        .hash = outputs.text(3),
        .mtime = std::chrono::file_clock::time_point(
            std::chrono::file_clock::duration(outputs.integer(4))),
        .inputs = {}});
  }

  Statement inputs(
      db,
      "SELECT output, path, kind, mtime, size, hash FROM inputs ORDER BY "
      "rowid");
  while (inputs.step()) {
    const auto it = by_output.find(inputs.text(0));
    if (it == by_output.end()) {
      continue;
    }
    records[it->second].inputs.push_back(BuildCache::Input{
        .path = from_text(inputs.text(1)),
        .kind = static_cast<BuildCache::Input::Kind>(inputs.integer(2)),
        .mtime = inputs.integer(3),
        .size = inputs.integer(4),
        .hash = inputs.text(5)});
  }
  return records;
}

}  // namespace

BuildCache::BuildCache(std::filesystem::path directory)
    : m_directory(std::move(directory)),
      m_objects(m_directory / "objects"),
      m_scratch(m_directory / "scratch") {
  std::filesystem::create_directories(m_objects);
  mark_as_cache(m_directory);

  ScopeGuard close_on_failure([this] {
    sqlite3_close(m_db);
    sqlite3_close(m_lock);
  });
  m_lock = open_database(m_directory / "lock.sqlite",
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);

  // Exclusive locking holds this database from the first transaction until the
  // connection closes, which is what keeps a second instance out. It is a
  // database of its own so that the records stay open to readers. Without a
  // busy timeout, that instance fails at once rather than waiting: `makebelieve
  // unmount` returns only once the process it stops has closed its cache.
  execute(m_lock, "PRAGMA locking_mode = EXCLUSIVE");
  const int locked = sqlite3_exec(
      m_lock, "BEGIN EXCLUSIVE; CREATE TABLE IF NOT EXISTS held(id); COMMIT",
      nullptr, nullptr, nullptr);
  if (locked == SQLITE_BUSY || locked == SQLITE_LOCKED) {
    // The error appends what the code says.
    throw std::system_error(
        std::make_error_code(std::errc::device_or_resource_busy),
        std::format(
            "build cache [{}] is in use by another makebelieve, which "
            "is serving from it or still stopping; stop that one with "
            "`makebelieve unmount <the directory holding it>` and try again",
            m_directory.string()));
  }
  if (locked != SQLITE_OK) {
    fail(m_lock, locked, "locking the database");
  }

  m_db = open_database(m_directory / "cache.sqlite",
                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
  // Only a reader starting or finishing can hold this up, and only briefly.
  sqlite3_busy_timeout(m_db, k_busy_timeout_ms);
  execute(m_db, "PRAGMA journal_mode = WAL");
  // A crash loses at most the last few records with WAL at NORMAL, but never
  // corrupts the database, which is all a cache needs.
  execute(m_db, "PRAGMA synchronous = NORMAL");

  {
    Transaction transaction(m_db);
    Statement version(m_db, "PRAGMA user_version");
    version.step();
    const std::int64_t found = version.integer(0);
    if (found != k_schema_version) {
      execute(m_db, "DROP TABLE IF EXISTS outputs");
      execute(m_db, "DROP TABLE IF EXISTS inputs");
      execute(m_db, std::string(k_schema).c_str());
      execute(
          m_db,
          std::format("PRAGMA user_version = {}", k_schema_version).c_str());
    }
    transaction.commit();
  }

  // Only now the cache is ours: whatever is in scratch was left by a run that
  // has gone, never by one still writing there.
  {
    std::error_code ignored;
    std::filesystem::remove_all(m_scratch, ignored);
  }
  std::filesystem::create_directories(m_scratch);
  close_on_failure.dismiss();
}

BuildCache::~BuildCache() {
  // In this order, so the next instance cannot get in before this one is done.
  sqlite3_close(m_db);
  sqlite3_close(m_lock);
}

const std::filesystem::path& BuildCache::directory() const {
  return m_directory;
}

const std::filesystem::path& BuildCache::scratch_directory() const {
  return m_scratch;
}

std::vector<BuildCache::Record> BuildCache::records() const {
  const std::lock_guard lock(m_mutex);
  return select_records(m_db);
}

std::vector<BuildCache::Record> BuildCache::read_records(
    const std::filesystem::path& directory) {
  const std::filesystem::path database = directory / "cache.sqlite";
  if (!std::filesystem::exists(database)) {
    return {};
  }
  sqlite3* const db = open_database(database, SQLITE_OPEN_READONLY);
  const ScopeGuard close([db] { sqlite3_close(db); });
  sqlite3_busy_timeout(db, k_busy_timeout_ms);

  // One transaction, so a build recorded meanwhile is seen whole or not at all.
  const Transaction transaction(db);
  Statement version(db, "PRAGMA user_version");
  version.step();
  if (version.integer(0) != k_schema_version) {
    return {};
  }
  return select_records(db);
}

std::expected<std::shared_ptr<const OpenedFile>, std::error_code>
BuildCache::open(std::string_view hash) const {
  const auto missing =
      std::make_error_code(std::errc::no_such_file_or_directory);
  if (!is_hash(hash)) {
    return std::unexpected(missing);
  }
  std::expected<std::shared_ptr<const OpenedFile>, std::error_code> file =
      OpenedFileUtil::open(m_objects / std::string(hash));
  if (!file.has_value()) {
    return std::unexpected(file.error());
  }
  const std::expected<std::string, std::error_code> found =
      BuildCache::hash(**file);
  if (!found.has_value()) {
    return std::unexpected(found.error());
  }
  // A content file that anything else has touched.
  if (*found != hash) {
    return std::unexpected(missing);
  }
  return file;
}

std::string BuildCache::store(const Command& command,
                              std::string_view bytes,
                              std::chrono::file_clock::time_point mtime,
                              std::span<const Input> inputs) {
  std::string hash = BuildCache::hash(bytes);
  store_content(command,
                *DirectoryTreeUtil::memory_file(
                    std::make_shared<const std::string>(bytes), {}),
                hash, mtime, inputs);
  return hash;
}

void BuildCache::store_file(const Command& command,
                            const std::filesystem::path& file,
                            std::string_view hash,
                            std::chrono::file_clock::time_point mtime,
                            std::span<const Input> inputs) {
  // Whatever becomes of it, the file is the cache's to get rid of.
  const ScopeGuard remove_file([&file] {
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
  });
  const std::lock_guard lock(m_mutex);
  place(std::string(hash), [&file](const std::filesystem::path& scratch) {
    std::error_code error;
    std::filesystem::rename(file, scratch, error);
    return error;
  });
  record(command, std::string(hash), mtime, inputs);
}

void BuildCache::store_content(const Command& command,
                               const OpenedFile& content,
                               std::string_view hash,
                               std::chrono::file_clock::time_point mtime,
                               std::span<const Input> inputs) {
  const std::lock_guard lock(m_mutex);
  place(std::string(hash), [&content](const std::filesystem::path& scratch) {
    return DirectoryTreeUtil::write_file(scratch, content);
  });
  record(command, std::string(hash), mtime, inputs);
}

void BuildCache::place(
    const std::string& hash,
    const std::function<std::error_code(const std::filesystem::path&)>& write) {
  if (!is_hash(hash)) {
    throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                            "build cache: storing an output");
  }
  // Content already there is left as it is, as something may be reading it.
  const std::filesystem::path object = m_objects / hash;
  std::error_code ignored;
  if (std::filesystem::exists(object, ignored)) {
    return;
  }
  const std::filesystem::path scratch =
      m_scratch / std::format("cache-{}.tmp", ++m_next_scratch);
  std::error_code error = write(scratch);
  if (!error) {
    std::filesystem::rename(scratch, object, error);
  }
  if (error) {
    std::filesystem::remove(scratch, ignored);
    throw std::system_error(error, "build cache: storing an output");
  }
}

void BuildCache::record(const Command& command,
                        const std::string& hash,
                        std::chrono::file_clock::time_point mtime,
                        std::span<const Input> inputs) {
  const std::string output = to_text(command.output);
  std::optional<std::string> previous;
  try {
    Transaction transaction(m_db);
    Statement find(m_db, "SELECT hash FROM outputs WHERE path = ?");
    if (find.bind(output).step()) {
      previous = find.text(0);
    }
    Statement(m_db, "DELETE FROM inputs WHERE output = ?").bind(output).run();
    Statement(m_db,
              "INSERT OR REPLACE INTO outputs(path, action, command, hash, "
              "mtime) VALUES (?, ?, ?, ?, ?)")
        .bind(output, to_text(command.action), command.text, hash,
              raw_mtime(mtime))
        .run();
    Statement insert(m_db,
                     "INSERT OR IGNORE INTO inputs(output, path, kind, mtime, "
                     "size, hash) VALUES (?, ?, ?, ?, ?, ?)");
    for (const Input& input : inputs) {
      insert
          .bind(output, to_text(input.path),
                static_cast<std::int64_t>(input.kind), input.mtime, input.size,
                input.hash)
          .run();
    }
    transaction.commit();
  } catch (...) {
    try {
      drop_if_unreferenced(hash);
    } catch (...) {  // NOLINT(bugprone-empty-catch): the first error matters
    }
    throw;
  }
  if (previous.has_value() && *previous != hash) {
    drop_if_unreferenced(*previous);
  }
}

void BuildCache::forget(std::span<const std::filesystem::path> outputs) {
  if (outputs.empty()) {
    return;
  }
  const std::lock_guard lock(m_mutex);
  std::unordered_set<std::string> dropped;
  {
    Transaction transaction(m_db);
    Statement find(m_db, "SELECT hash FROM outputs WHERE path = ?");
    Statement remove_inputs(m_db, "DELETE FROM inputs WHERE output = ?");
    Statement remove_output(m_db, "DELETE FROM outputs WHERE path = ?");
    for (const std::filesystem::path& output : outputs) {
      const std::string path = to_text(output);
      if (!find.bind(path).step()) {
        continue;
      }
      dropped.insert(find.text(0));
      remove_inputs.bind(path).run();
      remove_output.bind(path).run();
    }
    transaction.commit();
  }
  for (const std::string& hash : dropped) {
    drop_if_unreferenced(hash);
  }
}

void BuildCache::collect_garbage() {
  const std::lock_guard lock(m_mutex);
  std::unordered_set<std::string> referenced;
  Statement hashes(m_db, "SELECT DISTINCT hash FROM outputs");
  while (hashes.step()) {
    referenced.insert(hashes.text(0));
  }

  std::error_code error;
  std::vector<std::filesystem::path> unreferenced;
  for (std::filesystem::directory_iterator it(m_objects, error), end;
       !error && it != end; it.increment(error)) {
    if (!referenced.contains(to_text(it->path().filename()))) {
      unreferenced.push_back(it->path());
    }
  }
  for (const std::filesystem::path& path : unreferenced) {
    std::filesystem::remove(path, error);
  }
}

void BuildCache::drop_if_unreferenced(const std::string& hash) {
  Statement find(m_db, "SELECT 1 FROM outputs WHERE hash = ? LIMIT 1");
  if (!find.bind(hash).step()) {
    std::error_code ignored;
    std::filesystem::remove(m_objects / hash, ignored);
  }
}

std::string BuildCache::hash(std::string_view bytes) {
  return to_text(XXH3_128bits(bytes.data(), bytes.size()));
}

std::expected<std::string, std::error_code> BuildCache::hash(
    const OpenedFile& file) {
  XXH3_state_t state;
  XXH3_128bits_reset(&state);
  for (Offset offset = 0;;) {
    const std::expected<std::string, std::error_code> piece =
        DirectoryTreeUtil::wait_read(file.read(offset, k_hash_piece_bytes));
    if (!piece.has_value()) {
      return std::unexpected(piece.error());
    }
    if (piece->empty()) {
      return to_text(XXH3_128bits_digest(&state));
    }
    XXH3_128bits_update(&state, piece->data(), piece->size());
    offset += static_cast<Offset>(piece->size());
  }
}

std::int64_t BuildCache::raw_mtime(std::chrono::file_clock::time_point time) {
  return static_cast<std::int64_t>(time.time_since_epoch().count());
}

std::string BuildCache::hash_entries(std::span<const TreeEntry> entries) {
  // Each entry is hashed alone - its name's bytes as the platform holds them,
  // seeded by whether it is a directory - and the hashes summed, so the order
  // a listing comes in counts for nothing, with nothing sorted or copied. The
  // names in one directory are distinct, so no two of them cancel out.
  XXH128_hash_t sum{.low64 = entries.size(), .high64 = 0};
  for (const TreeEntry& entry : entries) {
    const std::filesystem::path::string_type& name = entry.name.native();
    const XXH128_hash_t one = XXH3_128bits_withSeed(
        name.data(), name.size() * sizeof(name[0]),
        std::holds_alternative<DirectoryInfo>(entry.info) ? 1 : 0);
    const std::uint64_t low = sum.low64 + one.low64;
    sum.high64 += one.high64 + (low < sum.low64 ? 1 : 0);
    sum.low64 = low;
  }
  return to_text(sum);
}

}  // namespace makebelieve
