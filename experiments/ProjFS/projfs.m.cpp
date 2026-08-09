/**
 * @file
 * Minimal ProjFS example: projects a single read-only file, "time.txt", whose
 * contents change every second - a random greeting of varying length plus
 * the current UTC time.
 *
 * Usage:
 * @code
 *   projfs_experiment C:\projfs-mnt   # creates C:\projfs-mnt itself
 *   :: in another window
 *   filewatch C:\projfs-mnt\time.txt 30
 *   type C:\projfs-mnt\time.txt   # first read kicks off the heartbeat
 *   :: back in the first window, Ctrl+C stops the provider
 * @endcode
 */

// Without this, windows.h defines max/min as function-like macros, which
// breaks any later std::max/std::min call whose invocation happens to look
// like "max(" or "min(" - see schedule_next_change()'s call site below.
#define NOMINMAX
#include <objbase.h>
#include <projectedfslib.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace {

/// Candidate greetings make_content() picks from at random. These are
/// different lengths to make sure we can't guess the content size, which
/// will be the case if we generated the contents from an arbitrary
/// command.
constexpr std::array<std::string_view, 2> kGreetings = {"hello", "hi"};

/// Name of the only file this provider projects, relative to the
/// virtualization root - ProjFS paths never carry a leading separator.
constexpr std::wstring_view k_time_name = L"time.txt";

/**
 * Opaque handle for the running virtualization instance, set once in
 * main() after PrjStartVirtualizing() succeeds. Callbacks are handed their
 * own copy via PRJ_CALLBACK_DATA::NamespaceVirtualizationContext and should
 * prefer that; this global exists only so the detached background thread
 * started by schedule_next_change() - which runs outside of any callback -
 * has a context to call PrjUpdateFileIfNeeded() with.
 */
PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT g_ctx = nullptr;

/**
 * Debounces schedule_next_change(): at most one update is ever pending, no
 * matter how many reads land while it's outstanding.
 */
std::atomic<bool> g_update_scheduled{false};

/**
 * Set by get_file_data() on every real hydration, cleared by fire_update()
 * after each push. Lets a completed update tell whether anyone re-read the
 * file since it ran - see fire_update() for why that matters.
 */
std::atomic<bool> g_get_file_data_called{false};

/**
 * Builds time.txt's content for a given moment.
 *
 * @param now The time to render.
 * @return The greeting-plus-timestamp content for that moment.
 */
std::string make_content(std::chrono::system_clock::time_point now) {
  const auto seed =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
  const std::size_t index =
      std::uniform_int_distribution<std::size_t>(0, kGreetings.size() - 1)(rng);

  const std::time_t raw_time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_s(&tm, &raw_time);
  char time_buf[32];
  std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm);

  std::string content(kGreetings[index]);
  content += ", it is ";
  content += time_buf;
  content += " UTC\n";
  return content;
}

/**
 * Converts a system_clock time point to a Windows FILETIME value.
 */
std::int64_t to_filetime(std::chrono::system_clock::time_point tp) {
  constexpr std::int64_t kUnixEpochInFiletime = 116444736000000000LL;
  using Ticks100ns =
      std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
  return std::chrono::duration_cast<Ticks100ns>(tp.time_since_epoch()).count() +
         kUnixEpochInFiletime;
}

/**
 * Encodes an epoch-seconds value into a PRJ_PLACEHOLDER_VERSION_INFO's
 * ContentID.
 */
PRJ_PLACEHOLDER_VERSION_INFO encode_epoch_seconds(std::int64_t seconds) {
  PRJ_PLACEHOLDER_VERSION_INFO version_info{};
  std::memcpy(version_info.ContentID, &seconds, sizeof(seconds));
  return version_info;
}

/**
 * Decodes an epoch-seconds value from a PRJ_PLACEHOLDER_VERSION_INFO's
 * ContentID.
 */
std::int64_t decode_epoch_seconds(
    const PRJ_PLACEHOLDER_VERSION_INFO& version_info) {
  std::int64_t seconds = 0;
  std::memcpy(&seconds, version_info.ContentID, sizeof(seconds));
  return seconds;
}

/**
 * Builds the PRJ_PLACEHOLDER_INFO describing time.txt's metadata as of
 * `now`, rounded down to the whole second.
 */
PRJ_PLACEHOLDER_INFO make_placeholder_info(
    std::chrono::system_clock::time_point now,
    std::size_t content_size) {
  const std::int64_t filetime = to_filetime(now);
  const auto epoch_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();

  return {
      .FileBasicInfo =
          {
              .IsDirectory = FALSE,
              .FileSize = static_cast<INT64>(content_size),
              .CreationTime = {.QuadPart = filetime},
              .LastAccessTime = {.QuadPart = filetime},
              .LastWriteTime = {.QuadPart = filetime},
              .ChangeTime = {.QuadPart = filetime},
              .FileAttributes = FILE_ATTRIBUTE_READONLY,
          },
      .VersionInfo = encode_epoch_seconds(epoch_seconds),
  };
}

/**
 * Performs the actual metadata update against the projected file, for
 * `target`, once the delay armed by schedule_next_change() has elapsed.
 * This is the ProjFS counterpart to fire_dummy_write() in fuse.m.cpp, but
 * it never calls WriteFile(): PrjUpdateFileIfNeeded() only pushes new
 * placeholder metadata, discarding whatever hydrated content was cached
 * on disk. That metadata push is what the filesystem's change
 * notifications key off of.
 *
 * @param target The moment this update's content/metadata represents.
 *   Taken as a parameter (schedule_next_change() passes next_second)
 *   rather than calling system_clock::now() again in here, so the pushed
 *   content is deterministically next_second's content regardless of how
 *   much OS scheduling jitter elapsed between sleep_until(next_second)
 *   returning and this line actually running.
 * @param retry_count How many stale-content follow-ups to try (see below):
 *   schedule_next_change() passes 1, and each follow-up decrements it, so an
 *   idle file settles instead of looping forever.
 */
void fire_update(std::chrono::system_clock::time_point target,
                 int retry_count) {
  // We run on the background thread schedule_next_change() spawned, so the
  // follow-ups below just sleep and loop here rather than each spawning a
  // fresh thread.
  for (;;) {
    const std::string content = make_content(target);
    const PRJ_PLACEHOLDER_INFO info =
        make_placeholder_info(target, content.size());

    // PRJ_UPDATE_ALLOW_READ_ONLY: time.txt always carries
    // FILE_ATTRIBUTE_READONLY (see make_placeholder_info), so without this
    // flag ProjFS would refuse to update it even coming from us.
    // PRJ_UPDATE_ALLOW_DIRTY_METADATA: covers a placeholder whose metadata
    // was touched locally (e.g. by a virus scanner or indexer) without going
    // through us. Deliberately NOT PRJ_UPDATE_ALLOW_DIRTY_DATA or
    // PRJ_UPDATE_ALLOW_TOMBSTONE: unlike fuse.m.cpp's dummy write, this call
    // carries no bytes, so there's never legitimate local file content worth
    // preserving, nor a delete worth silently reverting.
    //
    // A read that lands within a few milliseconds of the whole-second
    // boundary leaves this call almost no lead time before it runs - short
    // enough that Windows can still be holding the cached section it just
    // created to serve that read. When that happens PrjUpdateFileIfNeeded
    // fails with ERROR_SHARING_VIOLATION, a condition that's transient (the
    // section is released shortly after the read completes), so a short
    // bounded retry is the correct response here, not a workaround: Windows
    // Server's own file-sharing service retries the same error the same
    // way, and defaults to the same 5 attempts -
    // https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2003/cc778145(v=ws.10).
    // VFS for Git hits this same failure from PrjUpdateFileIfNeeded and
    // instead defers to a background retry queue rather than looping
    // in place -
    // https://github.com/microsoft/VFSForGit/blob/3b7ac38808bdab9079e7dd94dcac16d6c84a1ea3/GVFS/GVFS.Virtualization/Projection/GitIndexProjection.cs#L1855-L1859
    // - a better fit for updating hundreds of thousands of placeholders at
    // once, but wrong for us: we're correcting one file and want it fixed
    // within about a second, so a short inline retry is what we actually
    // want here.
    PRJ_UPDATE_FAILURE_CAUSES failure_reason{};
    for (int attempt = 0; attempt < 5; ++attempt) {
      const HRESULT update_hr = PrjUpdateFileIfNeeded(
          g_ctx, k_time_name.data(), &info, sizeof(info),
          PRJ_UPDATE_ALLOW_READ_ONLY | PRJ_UPDATE_ALLOW_DIRTY_METADATA,
          &failure_reason);
      if (update_hr != HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    g_update_scheduled.store(false, std::memory_order_relaxed);

    if (retry_count <= 0) {
      return;
    }

    // The update above notified watchers, but Windows can keep serving the
    // previously hydrated bytes for a short window - a read landing there is
    // served from cache without reaching get_file_data(), so the reader sees
    // the notification yet still reads stale content. EdenFS documents the
    // same "PrjFS keeps providing the old contents unless invalidated"
    // hazard -
    // https://github.com/facebook/sapling/blob/main/eden/fs/docs/Windows.md
    // (Invalidations). So wait a second and, if nothing has hydrated since,
    // loop with fresh content (new ContentID, so it isn't a no-op) to drop
    // the stale cache and re-notify.
    g_get_file_data_called.store(false, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (g_get_file_data_called.load(std::memory_order_relaxed)) {
      return;
    }
    // Respect the debounce: a read may have raced in and scheduled its own
    // update while we were waiting.
    bool expected = false;
    if (!g_update_scheduled.compare_exchange_strong(expected, true)) {
      return;
    }

    target = std::chrono::system_clock::now();
    --retry_count;
  }
}

/**
 * Schedules fire_update() to run at `next_time`, on a new thread, unless
 * one is already pending (see g_update_scheduled). Mirrors
 * schedule_next_change() in fuse.m.cpp.
 *
 * @param next_time The absolute moment to run fire_update() at, and the
 *   moment its pushed content/metadata will represent (see fire_update()).
 *   Computing this - including deciding what to do about a stale moment -
 *   is entirely the caller's job; see get_file_data()'s call site.
 */
void schedule_next_change(std::chrono::system_clock::time_point next_time) {
  bool expected = false;
  if (!g_update_scheduled.compare_exchange_strong(expected, true)) {
    return;
  }
  // sleep_until, not a precomputed sleep_for(delay): next_time is an
  // absolute point in time, so waiting for it directly avoids drift from
  // however long it takes the OS to actually schedule this new thread
  // after it's created.
  std::thread([next_time] {
    std::this_thread::sleep_until(next_time);
    fire_update(next_time, 1);
  }).detach();
}

/// Hashes a GUID by its raw bytes for use as an unordered_map key.
struct GuidHash {
  std::size_t operator()(const GUID& guid) const {
    return std::hash<std::string_view>{}(
        std::string_view(reinterpret_cast<const char*>(&guid), sizeof(guid)));
  }
};

/**
 * Active directory enumeration sessions, keyed by the GUID ProjFS assigns
 * each one in start_dir_enum(). The value is true once time.txt has
 * already been handed back for that session (there is only ever the one
 * entry, so "have we returned it yet" is all the state an enumeration
 * needs). Guarded by g_enum_mutex since ProjFS may run enumerations for
 * different sessions concurrently on different threads.
 */
std::mutex g_enum_mutex;
std::unordered_map<GUID, bool, GuidHash> g_enum_returned;

/// Groups the PRJ_CALLBACKS callbacks as static methods, named after the
/// ProjFS operation each one implements, mirroring the FUSE struct in
/// fuse.m.cpp.
struct ProjFS {
  /// Begins a directory enumeration session. Our root only ever contains
  /// one entry, so all there is to track is "not yet returned".
  static HRESULT CALLBACK start_dir_enum(const PRJ_CALLBACK_DATA*,
                                         const GUID* enumeration_id) {
    std::lock_guard<std::mutex> lock(g_enum_mutex);
    g_enum_returned[*enumeration_id] = false;
    return S_OK;
  }

  /// Ends a directory enumeration session, releasing its bookkeeping.
  static HRESULT CALLBACK end_dir_enum(const PRJ_CALLBACK_DATA*,
                                       const GUID* enumeration_id) {
    std::lock_guard<std::mutex> lock(g_enum_mutex);
    g_enum_returned.erase(*enumeration_id);
    return S_OK;
  }

  /// Hands back time.txt for the root directory's enumeration, once per
  /// session (or again from the top if PRJ_CB_DATA_FLAG_ENUM_RESTART_SCAN
  /// is set).
  static HRESULT CALLBACK get_dir_enum(const PRJ_CALLBACK_DATA* callback_data,
                                       const GUID* enumeration_id,
                                       PCWSTR search_expression,
                                       PRJ_DIR_ENTRY_BUFFER_HANDLE buffer) {
    std::lock_guard<std::mutex> lock(g_enum_mutex);
    const auto it = g_enum_returned.find(*enumeration_id);
    if (it == g_enum_returned.end()) {
      return E_INVALIDARG;
    }

    if ((callback_data->Flags & PRJ_CB_DATA_FLAG_ENUM_RESTART_SCAN) != 0) {
      it->second = false;
    }

    if (!it->second &&
        (search_expression == nullptr ||
         PrjFileNameMatch(k_time_name.data(), search_expression))) {
      const auto now = std::chrono::system_clock::now();
      const std::string content = make_content(now);
      PRJ_PLACEHOLDER_INFO info = make_placeholder_info(now, content.size());
      if (PrjFillDirEntryBuffer(k_time_name.data(), &info.FileBasicInfo,
                                buffer) == S_OK) {
        it->second = true;
      }
    }
    return S_OK;
  }

  /**
   * Reports time.txt's current metadata (size/timestamps) so ProjFS can
   * create an on-disk placeholder for it.
   */
  static HRESULT CALLBACK
  get_placeholder_info(const PRJ_CALLBACK_DATA* callback_data) {
    if (PrjFileNameCompare(callback_data->FilePathName, k_time_name.data()) !=
        0) {
      return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    const auto now = std::chrono::system_clock::now();
    const std::string content = make_content(now);
    const PRJ_PLACEHOLDER_INFO info =
        make_placeholder_info(now, content.size());
    return PrjWritePlaceholderInfo(
        callback_data->NamespaceVirtualizationContext, k_time_name.data(),
        &info, sizeof(info));
  }

  /**
   * Supplies time.txt's content so ProjFS can hydrate the placeholder,
   * then triggers the next change notification.
   */
  static HRESULT CALLBACK get_file_data(const PRJ_CALLBACK_DATA* callback_data,
                                        UINT64 byte_offset,
                                        UINT32 length) {
    if (PrjFileNameCompare(callback_data->FilePathName, k_time_name.data()) !=
        0) {
      return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    // Reconstruct exactly the content get_placeholder_info() promised for
    // this placeholder, from the epoch-second stashed in
    // VersionInfo.ContentID.
    assert(callback_data->VersionInfo != nullptr);
    const std::int64_t epoch_seconds =
        decode_epoch_seconds(*callback_data->VersionInfo);
    const auto moment = std::chrono::system_clock::time_point(
        std::chrono::seconds(epoch_seconds));
    const std::string content = make_content(moment);

    void* buffer = PrjAllocateAlignedBuffer(
        callback_data->NamespaceVirtualizationContext, length);
    if (buffer == nullptr) {
      return E_OUTOFMEMORY;
    }

    const std::size_t offset = static_cast<std::size_t>(byte_offset);
    const std::size_t n =
        offset < content.size()
            ? std::min<std::size_t>(length, content.size() - offset)
            : 0;
    std::memset(buffer, 0, length);
    if (n > 0) {
      std::memcpy(buffer, content.data() + offset, n);
    }

    const HRESULT hr = PrjWriteFileData(
        callback_data->NamespaceVirtualizationContext,
        &callback_data->DataStreamId, buffer, byte_offset, length);
    PrjFreeAlignedBuffer(buffer);

    if (SUCCEEDED(hr)) {
      // A read reached us, so a just-fired update's follow-up isn't needed
      // (see fire_update()).
      g_get_file_data_called.store(true, std::memory_order_relaxed);

      // Schedule for the later of "one second after the content we just
      // served" and "right now". If nobody read the file for a while
      // before this, `moment` can already be several seconds stale, and
      // moment + 1s would already be in the past - there's no reason to
      // wait out an extra second on top of staleness that's already
      // there, so this fires as soon as possible (now) instead. When
      // `moment` is fresh (the normal case, matching the current
      // second), moment + 1s is later than now, and that's what's
      // actually used - the update still lands on the natural
      // next-second boundary rather than firing early.
      const std::chrono::system_clock::time_point next_from_moment =
          std::chrono::time_point_cast<std::chrono::seconds>(moment) +
          std::chrono::seconds(1);
      const std::chrono::system_clock::time_point now =
          std::chrono::system_clock::now();
      schedule_next_change(std::max(next_from_moment, now));
    }
    return hr;
  }
};

/**
 * Creates `root` fresh and marks it as a ProjFS virtualization root. Fails
 * outright if `root` already exists, rather than silently reusing
 * whatever's there - e.g. a leftover from a previous crashed run, or an
 * unrelated directory the caller didn't mean to virtualize over. This
 * provider owns the root's whole lifetime, pairing with
 * remove_virtualization_root() at shutdown.
 */
HRESULT ensure_virtualization_root(const std::wstring& root) {
  if (!::CreateDirectoryW(root.c_str(), nullptr)) {
    return HRESULT_FROM_WIN32(::GetLastError());
  }
  GUID instance_id{};
  ::CoCreateGuid(&instance_id);
  return PrjMarkDirectoryAsPlaceholder(root.c_str(), nullptr, nullptr,
                                       &instance_id);
}

/**
 * Removes `root`, once PrjStopVirtualizing() has torn down the
 * virtualization instance, so the mount point leaves no trace behind.
 * Two things stand between here and a plain RemoveDirectoryW: if
 * time.txt was ever read, it's a real on-disk placeholder file at this
 * point (not just a virtual entry), so the root isn't empty from
 * RemoveDirectoryW's point of view; and it still carries
 * FILE_ATTRIBUTE_READONLY (see make_placeholder_info), which blocks
 * DeleteFileW the same way it blocks WriteFile. Both need clearing first.
 * Best effort throughout - a failure here doesn't change whether the
 * provider itself did its job.
 */
void remove_virtualization_root(const std::wstring& root) {
  std::wstring time_txt_path = root;
  time_txt_path += L'\\';
  time_txt_path += k_time_name;
  ::SetFileAttributesW(time_txt_path.c_str(), FILE_ATTRIBUTE_NORMAL);
  ::DeleteFileW(time_txt_path.c_str());

  if (!::RemoveDirectoryW(root.c_str())) {
    std::fwprintf(stderr, L"failed to remove virtualization root: error %lu\n",
                  ::GetLastError());
  }
}

/**
 * Signaled by stop_handler() so main() can block on a single wait instead
 * of std::getchar(). getchar() returns immediately (EOF) rather than
 * blocking whenever stdin isn't a real interactive console - e.g. when
 * this process is launched by a test harness or another process with
 * redirected/closed stdin - which would otherwise tear the provider down
 * right after startup.
 */
HANDLE g_stop_event = nullptr;

BOOL WINAPI stop_handler(DWORD) {
  ::SetEvent(g_stop_event);
  return TRUE;
}

}  // namespace

/**
 * Parses argv for the virtualization root, marks it as such if needed, and
 * runs the provider until Ctrl+C (or another console-close signal) is
 * received.
 */
int wmain(int argc, wchar_t* argv[]) {
  if (argc != 2) {
    std::fwprintf(stderr, L"usage: %s <virtualization-root>\n", argv[0]);
    return 1;
  }
  const std::wstring root = argv[1];

  g_stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ::SetConsoleCtrlHandler(stop_handler, TRUE);

  if (const HRESULT hr = ensure_virtualization_root(root); FAILED(hr)) {
    std::fwprintf(stderr, L"failed to set up virtualization root: 0x%08lx\n",
                  static_cast<unsigned long>(hr));
    return 1;
  }

  PRJ_CALLBACKS callbacks = {
      .StartDirectoryEnumerationCallback = ProjFS::start_dir_enum,
      .EndDirectoryEnumerationCallback = ProjFS::end_dir_enum,
      .GetDirectoryEnumerationCallback = ProjFS::get_dir_enum,
      .GetPlaceholderInfoCallback = ProjFS::get_placeholder_info,
      .GetFileDataCallback = ProjFS::get_file_data,
  };

  if (const HRESULT hr = PrjStartVirtualizing(root.c_str(), &callbacks, nullptr,
                                              nullptr, &g_ctx);
      FAILED(hr)) {
    std::fwprintf(stderr, L"PrjStartVirtualizing failed: 0x%08lx\n",
                  static_cast<unsigned long>(hr));
    return 1;
  }

  std::wprintf(L"projfs_experiment running at [%s]\n", root.c_str());
  std::wprintf(L"Press Ctrl+C to stop the provider...\n");
  std::fflush(stdout);
  ::WaitForSingleObject(g_stop_event, INFINITE);

  PrjStopVirtualizing(g_ctx);
  remove_virtualization_root(root);
  return 0;
}
