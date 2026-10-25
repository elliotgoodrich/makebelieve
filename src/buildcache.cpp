// SPDX-License-Identifier: MIT
#include "buildcache.hpp"

#include "filesystemutil.hpp"
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
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace makebelieve {

namespace {

// Bumped whenever the tables change shape. A cache of any other version is
// emptied rather than migrated: it only saves rebuilding.
constexpr int k_schema_version = 1;

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
  }
  return "unknown";
}

std::optional<Manifest::Action> action_from_text(std::string_view text) {
  for (const Manifest::Action action :
       {Manifest::Action::Run, Manifest::Action::Capture,
        Manifest::Action::Copy, Manifest::Action::Tracing}) {
    if (to_text(action) == text) {
      return action;
    }
  }
  return std::nullopt;
}

// Whether @a hash could have come from BuildCache::hash, so it is safe to
// name a file by.
bool is_hash(std::string_view hash) {
  return hash.size() == 32 && std::ranges::all_of(hash, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

}  // namespace

BuildCache::BuildCache(std::filesystem::path directory)
    : m_directory(std::move(directory)), m_objects(m_directory / "objects") {
  std::filesystem::create_directories(m_objects);

  const std::string database = to_text(m_directory / "cache.sqlite");
  if (const int code = sqlite3_open_v2(
          database.c_str(), &m_db,
          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
          nullptr);
      code != SQLITE_OK) {
    // A handle comes back even on failure, to say why; it must still close.
    const ScopeGuard close([db = m_db] { sqlite3_close(db); });
    fail(m_db, code, std::format("opening {}", database));
  }
  ScopeGuard close_on_failure([this] { sqlite3_close(m_db); });

  // Exclusive locking holds the database from the first transaction until the
  // connection closes, which is what keeps a second instance out. Without a
  // busy timeout, that instance fails at once rather than waiting: `makebelieve
  // unmount` returns only once the process it stops has closed its cache.
  execute(m_db, "PRAGMA locking_mode = EXCLUSIVE");
  const int locked =
      sqlite3_exec(m_db, "BEGIN EXCLUSIVE; COMMIT", nullptr, nullptr, nullptr);
  if (locked == SQLITE_BUSY || locked == SQLITE_LOCKED) {
    throw std::system_error(
        std::make_error_code(std::errc::device_or_resource_busy),
        std::format("build cache [{}] is in use by another makebelieve",
                    m_directory.string()));
  }
  if (locked != SQLITE_OK) {
    fail(m_db, locked, "locking the database");
  }
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
  close_on_failure.dismiss();
}

BuildCache::~BuildCache() {
  sqlite3_close(m_db);
}

const std::filesystem::path& BuildCache::directory() const {
  return m_directory;
}

std::vector<BuildCache::Record> BuildCache::records() const {
  const std::lock_guard lock(m_mutex);
  std::vector<Record> records;
  std::unordered_map<std::string, std::size_t> by_output;

  Statement outputs(m_db,
                    "SELECT path, action, command, hash, mtime FROM outputs");
  while (outputs.step()) {
    const std::optional<Manifest::Action> action =
        action_from_text(outputs.text(1));
    if (!action.has_value()) {
      continue;  // nothing this version wrote, so nothing it can rebuild from
    }
    const std::string path = outputs.text(0);
    by_output.emplace(path, records.size());
    records.push_back(
        Record{.command = Command{.output = from_text(path),
                                  .action = *action,
                                  .text = outputs.text(2)},
               .hash = outputs.text(3),
               .mtime = std::chrono::file_clock::time_point(
                   std::chrono::file_clock::duration(outputs.integer(4))),
               .inputs = {}});
  }

  Statement inputs(
      m_db,
      "SELECT output, path, mtime, size, hash FROM inputs ORDER BY rowid");
  while (inputs.step()) {
    const auto it = by_output.find(inputs.text(0));
    if (it == by_output.end()) {
      continue;
    }
    records[it->second].inputs.push_back(
        Input{.path = from_text(inputs.text(1)),
              .mtime = inputs.integer(2),
              .size = inputs.integer(3),
              .hash = inputs.text(4)});
  }
  return records;
}

std::expected<std::string, std::error_code> BuildCache::read(
    std::string_view hash) const {
  const auto missing =
      std::make_error_code(std::errc::no_such_file_or_directory);
  if (!is_hash(hash)) {
    return std::unexpected(missing);
  }
  std::expected<std::string, std::error_code> bytes =
      FileSystemUtil::read_file(m_objects / std::string(hash));
  if (!bytes.has_value()) {
    return std::unexpected(bytes.error());
  }
  // A content file a crash cut short, or that anything else has touched.
  if (BuildCache::hash(*bytes) != hash) {
    return std::unexpected(missing);
  }
  return bytes;
}

std::string BuildCache::store(const Command& command,
                              std::string_view bytes,
                              std::chrono::file_clock::time_point mtime,
                              std::span<const Input> inputs) {
  std::string hash = BuildCache::hash(bytes);
  const std::lock_guard lock(m_mutex);

  // Written in full under another name, then renamed into place, so the file
  // under the hash's name never holds anything else.
  const std::filesystem::path object = m_objects / hash;
  const std::filesystem::path scratch =
      m_objects / std::format("{}.{}.tmp", hash, ++m_next_scratch);
  if (const std::error_code error =
          FileSystemUtil::write_file(scratch, bytes)) {
    std::error_code ignored;
    std::filesystem::remove(scratch, ignored);
    throw std::system_error(error, "build cache: writing an output");
  }
  {
    std::error_code error;
    std::filesystem::rename(scratch, object, error);
    if (error) {
      std::filesystem::remove(scratch, error);
      throw std::system_error(error, "build cache: writing an output");
    }
  }

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
                     "INSERT OR IGNORE INTO inputs(output, path, mtime, size, "
                     "hash) VALUES (?, ?, ?, ?, ?)");
    for (const Input& input : inputs) {
      insert
          .bind(output, to_text(input.path), input.mtime, input.size,
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
  return hash;
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
  const XXH128_hash_t hash = XXH3_128bits(bytes.data(), bytes.size());
  return std::format("{:016x}{:016x}", hash.high64, hash.low64);
}

std::int64_t BuildCache::raw_mtime(std::chrono::file_clock::time_point time) {
  return static_cast<std::int64_t>(time.time_since_epoch().count());
}

}  // namespace makebelieve
