// SPDX-License-Identifier: MIT
#include "virtualfilesystem.hpp"

#include "directorytree.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <objbase.h>
#include <projectedfslib.h>
#include <windows.h>

namespace makebelieve {

namespace {

// Upper bound on one PrjAllocateAlignedBuffer allocation.
constexpr UINT32 k_write_chunk_bytes = 1U << 20;

// PrjUpdateFileIfNeeded fails with ERROR_SHARING_VIOLATION when Windows is
// still holding the cached section for a read that just completed.  So we
// retry for a few times afterwards.
constexpr int k_update_attempts = 5;
constexpr std::chrono::milliseconds k_update_retry_delay{50};

// Flags shared by every update and delete issued here. Placeholders are
// created read-only.
constexpr PRJ_UPDATE_TYPES k_update_flags = static_cast<PRJ_UPDATE_TYPES>(
    PRJ_UPDATE_ALLOW_DIRTY_METADATA | PRJ_UPDATE_ALLOW_READ_ONLY);

struct GuidHash {
  std::size_t operator()(const GUID& guid) const {
    return std::hash<std::string_view>{}(
        std::string_view(reinterpret_cast<const char*>(&guid), sizeof(guid)));
  }
};

[[noreturn]] void throw_hresult(HRESULT hr, const char* what) {
  throw std::system_error(static_cast<int>(hr), std::system_category(), what);
}

// ProjFS names paths relative to the virtualization root with backslashes and
// no leading separator, which is how the root itself becomes the empty
// string.
std::wstring to_projfs_path(std::filesystem::path path) {
  path.make_preferred();
  return path.wstring();
}

// MSVC's file_clock already counts 100ns ticks from the Windows epoch, which
// is precisely what FILETIME and LARGE_INTEGER timestamps want.
std::int64_t to_filetime(std::chrono::file_clock::time_point time) {
  return time.time_since_epoch().count();
}

// Packs size and mtime into the placeholder's ContentID.
PRJ_PLACEHOLDER_VERSION_INFO make_version_info(std::int64_t size,
                                               std::int64_t filetime) {
  PRJ_PLACEHOLDER_VERSION_INFO version_info{};
  static_assert(sizeof(version_info.ContentID) >= 2 * sizeof(std::int64_t));
  std::memcpy(version_info.ContentID, &size, sizeof(size));
  std::memcpy(version_info.ContentID + sizeof(size), &filetime,
              sizeof(filetime));
  return version_info;
}

// Describes a tree entry to ProjFS.
PRJ_PLACEHOLDER_INFO make_placeholder_info(const EntryInfo& status) {
  if (const auto* file = std::get_if<FileInfo>(&status)) {
    const std::int64_t filetime = to_filetime(file->mtime);
    const auto size = static_cast<std::int64_t>(file->size);
    return {
        .FileBasicInfo =
            {
                .IsDirectory = FALSE,
                .FileSize = size,
                .CreationTime = {.QuadPart = filetime},
                .LastAccessTime = {.QuadPart = filetime},
                .LastWriteTime = {.QuadPart = filetime},
                .ChangeTime = {.QuadPart = filetime},
                .FileAttributes = FILE_ATTRIBUTE_READONLY,
            },
        .VersionInfo = make_version_info(size, filetime),
    };
  }

  const auto& directory = std::get<DirectoryInfo>(status);
  const std::int64_t filetime = to_filetime(directory.mtime);

  // No FILE_ATTRIBUTE_READONLY here, unlike files. On a directory that flag
  // does not mean "cannot be modified" - Windows uses it to mark customised
  // folders.
  return {
      .FileBasicInfo =
          {
              .IsDirectory = TRUE,
              .FileSize = 0,
              .CreationTime = {.QuadPart = filetime},
              .LastAccessTime = {.QuadPart = filetime},
              .LastWriteTime = {.QuadPart = filetime},
              .ChangeTime = {.QuadPart = filetime},
              .FileAttributes = FILE_ATTRIBUTE_DIRECTORY,
          },
      .VersionInfo = make_version_info(0, filetime),
  };
}

// Frees a PrjAllocateAlignedBuffer allocation on scope exit. PrjWriteFileData
// requires the storage device's alignment, which is why the buffer cannot
// simply come from the allocator.
class AlignedBuffer {
  PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT m_context;
  void* m_buffer;

 public:
  AlignedBuffer(PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT context, UINT32 size)
      : m_context(context), m_buffer(PrjAllocateAlignedBuffer(context, size)) {}

  ~AlignedBuffer() {
    if (m_buffer != nullptr) {
      PrjFreeAlignedBuffer(m_buffer);
    }
  }

  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;
  AlignedBuffer(AlignedBuffer&&) = delete;
  AlignedBuffer& operator=(AlignedBuffer&&) = delete;

  [[nodiscard]] void* get() const { return m_buffer; }
};

// Readies `mountpoint` to be virtualized over: creates it (parents included)
// if absent, accepts it if it is an existing empty directory, and refuses
// anything else.
void create_mountpoint(const std::filesystem::path& mountpoint) {
  std::error_code error;
  if (std::filesystem::exists(mountpoint, error)) {
    throw std::filesystem::filesystem_error(
        "mountpoint already exists; refusing to reuse it", mountpoint,
        std::make_error_code(std::errc::file_exists));
  }

  // Call create_directories to create parents as well.
  std::filesystem::create_directories(mountpoint, error);
  if (error) {
    throw std::filesystem::filesystem_error("could not create mountpoint",
                                            mountpoint, error);
  }
}

// Removes a mountpoint this provider created.
void remove_mountpoint(const std::filesystem::path& mountpoint) noexcept {
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator it(mountpoint, error), end;
       it != end; it.increment(error)) {
    if (error) {
      break;
    }
    std::filesystem::permissions(it->path(),
                                 std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add, error);
  }
  std::filesystem::remove_all(mountpoint, error);
}

}  // namespace

// ProjFS provider over a DirectoryTree.
//
// ProjFS dispatches callbacks on its own pool threads, concurrently, so every
// member here is either immutable after construction or guarded. The tree's
// own const-means-thread-safe contract is what makes querying it from those
// threads sound.
class VirtualFileSystem::Impl {
 public:
  Impl(const DirectoryTree& tree, const std::filesystem::path& mountpoint)
      : m_tree(tree), m_root(to_projfs_path(mountpoint)) {
    // Create the mountpoint fresh, failing if it already exists - see
    // create_mountpoint. Because we always create it, teardown can always
    // remove it.
    create_mountpoint(mountpoint);

    GUID instance_id;
    if (const HRESULT hr = ::CoCreateGuid(&instance_id); FAILED(hr)) {
      throw_hresult(hr, "CoCreateGuid");
    }
    if (const HRESULT hr = PrjMarkDirectoryAsPlaceholder(
            m_root.c_str(), nullptr, nullptr, &instance_id);
        FAILED(hr)) {
      throw_hresult(hr, "PrjMarkDirectoryAsPlaceholder");
    }

    const PRJ_CALLBACKS callbacks = {
        .StartDirectoryEnumerationCallback = trampoline<&Impl::start_enum>,
        .EndDirectoryEnumerationCallback = trampoline<&Impl::end_enum>,
        .GetDirectoryEnumerationCallback = trampoline<&Impl::get_enum>,
        .GetPlaceholderInfoCallback = trampoline<&Impl::get_placeholder_info>,
        .GetFileDataCallback = trampoline<&Impl::get_file_data>,
    };

    if (const HRESULT hr = PrjStartVirtualizing(m_root.c_str(), &callbacks,
                                                this, nullptr, &m_context);
        FAILED(hr)) {
      throw_hresult(hr, "PrjStartVirtualizing");
    }

    // The notifier has to be running before a change can be queued for it, and
    // the subscription is taken last so no notification can arrive before there
    // is a context to service it with.
    m_notifier = std::thread([this]() { notify_loop(); });
    m_subscription.emplace(m_tree.subscribe_to_changes(
        [this](const DirectoryTreeDiff& diff) { on_tree_changed(diff); }));
  }

  ~Impl() {
    // Unsubscribe first: a notification landing after PrjStopVirtualizing
    // would call into a torn-down context.
    m_subscription.reset();

    // Then stop the notifier and wait for it to drain. It calls
    // PrjUpdateFileIfNeeded/PrjDeleteFile against m_context, so it has to be
    // gone before PrjStopVirtualizing tears that context down.
    {
      const std::lock_guard<std::mutex> lock(m_notify_mutex);
      m_stopping = true;
    }
    m_wake.notify_one();
    if (m_notifier.joinable()) {
      m_notifier.join();
    }

    if (m_context != nullptr) {
      PrjStopVirtualizing(m_context);
    }
    // Remove the mountpoint we created. Symmetric with the constructor, which
    // fails unless it created the directory fresh, so this always deletes only
    // what this provider made. It runs after PrjStopVirtualizing, since the
    // placeholders become ordinary, deletable files only once virtualization
    // has stopped.
    remove_mountpoint(std::filesystem::path(m_root));
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  // Snapshots the directory being enumerated.
  //
  // The snapshot is taken once here rather than re-queried per batch: ProjFS
  // drives one enumeration across as many GetDirectoryEnumeration calls as it
  // takes to drain, and a listing that shifted underneath those calls could
  // duplicate or skip entries.
  HRESULT start_enum(const PRJ_CALLBACK_DATA* data,
                     const GUID* enumeration_id) {
    std::expected<std::vector<TreeEntry>, std::error_code> entries =
        m_tree.ls(std::filesystem::path(data->FilePathName));
    if (!entries.has_value()) {
      return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    Enumeration session;
    session.entries = std::move(*entries);

    // ProjFS requires entries in PrjFileNameCompare order, and a provider that
    // returns them in any other order gets silently wrong directory listings.
    std::sort(session.entries.begin(), session.entries.end(),
              [](const TreeEntry& left, const TreeEntry& right) {
                return PrjFileNameCompare(left.name.c_str(),
                                          right.name.c_str()) < 0;
              });

    const std::lock_guard<std::mutex> lock(m_enum_mutex);
    m_enumerations[*enumeration_id] = std::move(session);
    return S_OK;
  }

  HRESULT end_enum(const PRJ_CALLBACK_DATA*, const GUID* enumeration_id) {
    const std::lock_guard<std::mutex> lock(m_enum_mutex);
    m_enumerations.erase(*enumeration_id);
    return S_OK;
  }

  // Fills one batch of entries, resuming where the previous call stopped.
  HRESULT get_enum(const PRJ_CALLBACK_DATA* data,
                   const GUID* enumeration_id,
                   PCWSTR search_expression,
                   PRJ_DIR_ENTRY_BUFFER_HANDLE buffer) {
    const std::lock_guard<std::mutex> lock(m_enum_mutex);
    const auto it = m_enumerations.find(*enumeration_id);
    if (it == m_enumerations.end()) {
      return E_INVALIDARG;
    }
    Enumeration& session = it->second;

    // Starting and restarting are the same operation - rewind the cursor and
    // take the filter - so they share a branch. A restart carrying no
    // expression is an unfiltered restart rather than a request to keep the
    // previous one, matching the NtQueryDirectoryFile RestartScan semantics
    // this callback sits on top of.
    const bool restart =
        (data->Flags & PRJ_CB_DATA_FLAG_ENUM_RESTART_SCAN) != 0;
    if (restart || !session.next.has_value()) {
      session.next = 0;
      session.search = search_expression != nullptr
                           ? std::optional<std::wstring>(search_expression)
                           : std::nullopt;
    }

    std::size_t& next = *session.next;
    while (next < session.entries.size()) {
      const TreeEntry& entry = session.entries[next];
      const std::wstring name = entry.name.filename().wstring();

      // Deliberately the captured filter, never the parameter - see
      // Enumeration::search.
      if (session.search.has_value() &&
          !PrjFileNameMatch(name.c_str(), session.search->c_str())) {
        ++next;
        continue;
      }

      PRJ_PLACEHOLDER_INFO info = make_placeholder_info(entry.info);
      if (FAILED(PrjFillDirEntryBuffer(name.c_str(), &info.FileBasicInfo,
                                       buffer))) {
        // The buffer is full. Returning success without advancing leaves this
        // entry as the first one the next call emits.
        return S_OK;
      }
      ++next;
    }
    return S_OK;
  }

  // Reports an entry's metadata so ProjFS can create its placeholder.
  HRESULT get_placeholder_info(const PRJ_CALLBACK_DATA* data) {
    const std::filesystem::path path(data->FilePathName);
    const std::expected<EntryInfo, std::error_code> status =
        m_tree.status(path);
    if (!status.has_value()) {
      return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    const PRJ_PLACEHOLDER_INFO info = make_placeholder_info(*status);
    const HRESULT hr =
        PrjWritePlaceholderInfo(data->NamespaceVirtualizationContext,
                                data->FilePathName, &info, sizeof(info));
    if (SUCCEEDED(hr)) {
      const std::lock_guard<std::mutex> lock(m_placeholder_mutex);
      m_placeholders.insert(path);
    }
    return hr;
  }

  // Hydrates a placeholder from the tree.
  HRESULT get_file_data(const PRJ_CALLBACK_DATA* data,
                        UINT64 byte_offset,
                        UINT32 length) {
    const std::filesystem::path path(data->FilePathName);

    const UINT32 capacity = std::min<UINT32>(length, k_write_chunk_bytes);
    const AlignedBuffer buffer(data->NamespaceVirtualizationContext, capacity);
    if (buffer.get() == nullptr) {
      return E_OUTOFMEMORY;
    }

    for (UINT32 written = 0; written < length;) {
      const UINT32 chunk = std::min<UINT32>(capacity, length - written);
      const UINT64 offset = byte_offset + written;

      const std::expected<std::string, std::error_code> bytes =
          m_tree.read(path, static_cast<Offset>(offset), chunk);
      if (!bytes.has_value()) {
        // read() carries an OS error code; forward it as an HRESULT the same
        // way the old throwing path did through the trampoline's catch.
        return HRESULT_FROM_WIN32(static_cast<DWORD>(bytes.error().value()));
      }

      std::memset(buffer.get(), 0, chunk);
      if (!bytes->empty()) {
        std::memcpy(buffer.get(), bytes->data(),
                    std::min<std::size_t>(bytes->size(), chunk));
      }

      const HRESULT hr =
          PrjWriteFileData(data->NamespaceVirtualizationContext,
                           &data->DataStreamId, buffer.get(), offset, chunk);
      if (FAILED(hr)) {
        return hr;
      }
      written += chunk;
    }
    return S_OK;
  }

 private:
  // One in-flight directory enumeration.
  struct Enumeration {
    std::vector<TreeEntry> entries;

    // Cursor into `entries`, unset until the first GetDirectoryEnumeration
    // call for this session. Unset doubles as "the filter below has not been
    // captured yet" - the two are always set together, so tracking it
    // separately would only create a state where they could disagree.
    std::optional<std::size_t> next;

    // The session's filter, owned rather than borrowed. ProjFS only
    // guarantees a search expression on the first call for a session and may
    // pass nullptr on later ones, so it has to be captured; and the string it
    // points at only lives for the duration of that callback, so it has to be
    // copied. Empty optional means the session is unfiltered, which is
    // distinct from a filter that happens to be an empty string.
    std::optional<std::wstring> search;
  };

  // Bridges a PRJ_* C callback to a member function, recovering the instance
  // from PRJ_CALLBACK_DATA::InstanceContext.
  template <auto MemFn>
  struct Bridge;

  template <typename... Args,
            HRESULT (Impl::*MemFn)(const PRJ_CALLBACK_DATA*, Args...)>
  struct Bridge<MemFn> {
    static HRESULT CALLBACK call(const PRJ_CALLBACK_DATA* data, Args... args) {
      try {
        auto* self = static_cast<Impl*>(data->InstanceContext);
        return (self->*MemFn)(data, args...);
      } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
      } catch (const std::system_error& error) {
        return HRESULT_FROM_WIN32(static_cast<DWORD>(error.code().value()));
      } catch (...) {
        return E_FAIL;
      }
    }
  };

  template <auto MemFn>
  static constexpr auto trampoline = Bridge<MemFn>::call;

  // Queues a tree change for the notifier thread.
  //
  // Runs on whichever thread the tree notifies from, so it does no I/O. The
  // first read of a lazily-built output runs the build synchronously inside
  // get_file_data, and the tree change that build produces is delivered right
  // there on the ProjFS callback thread; a PrjUpdateFileIfNeeded issued from
  // that stack would re-enter ProjFS re-entrantly and fail with
  // ERROR_SHARING_VIOLATION. Handing the poke to the notifier defers it until
  // get_file_data has returned.
  void on_tree_changed(const DirectoryTreeDiff& diff) {
    const std::lock_guard<std::mutex> lock(m_notify_mutex);
    if (m_stopping) {
      return;
    }
    if (diff.everything_dirty) {
      m_everything_dirty = true;
    } else {
      m_pending.insert(diff.entries_changed.begin(),
                       diff.entries_changed.end());
    }

    // child_lists_changed is deliberately not handled beyond what
    // entries_changed already covers, because ProjFS exposes no primitive for
    // invalidating a cached directory enumeration. Removals and modifications
    // arrive as entries_changed entries and are handled below; a *newly added*
    // file may therefore not appear in a listing of a directory that has
    // already been enumerated, until something touches it by name and
    // GetPlaceholderInfoCallback runs.
    //
    // TODO: Investigate further.
    m_wake.notify_one();
  }

  // Drains queued changes, invalidating each affected placeholder in turn.
  void notify_loop() {
    std::unique_lock<std::mutex> lock(m_notify_mutex);
    while (true) {
      m_wake.wait(lock, [this] {
        return m_stopping || m_everything_dirty || !m_pending.empty();
      });
      if (m_stopping) {
        break;
      }

      std::set<std::filesystem::path> batch;
      if (m_everything_dirty) {
        // The tree lost track of what changed, so everything we projected could
        // be stale. Invalidate the placeholders we know we created rather than
        // walking the mount: enumerating our own virtualization root would
        // re-enter the ProjFS callbacks.
        m_everything_dirty = false;
        m_pending.clear();
        const std::lock_guard<std::mutex> placeholder_lock(m_placeholder_mutex);
        batch = m_placeholders;
      } else {
        batch = std::exchange(m_pending, {});
      }

      // Unlocked for the invalidations themselves, so a change arriving
      // mid-batch simply queues up for the next pass.
      lock.unlock();
      for (const std::filesystem::path& path : batch) {
        invalidate(path);
      }
      lock.lock();
    }
  }

  // Re-pushes or removes one placeholder to match the tree.
  void invalidate(const std::filesystem::path& path) {
    {
      // A path we never projected has nothing cached against it, so there is
      // nothing to invalidate and no reason to query the tree.
      const std::lock_guard<std::mutex> lock(m_placeholder_mutex);
      if (m_placeholders.find(path) == m_placeholders.end()) {
        return;
      }
    }

    const std::wstring name = to_projfs_path(path);
    const std::expected<EntryInfo, std::error_code> status =
        m_tree.status(path);
    PRJ_UPDATE_FAILURE_CAUSES cause{};

    if (!status.has_value()) {
      PrjDeleteFile(m_context, name.c_str(), k_update_flags, &cause);
      const std::lock_guard<std::mutex> lock(m_placeholder_mutex);
      m_placeholders.erase(path);
      return;
    }

    const PRJ_PLACEHOLDER_INFO info = make_placeholder_info(*status);
    for (int attempt = 0; attempt < k_update_attempts; ++attempt) {
      const HRESULT hr = PrjUpdateFileIfNeeded(
          m_context, name.c_str(), &info, sizeof(info), k_update_flags, &cause);
      if (hr != HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)) {
        break;
      }
      std::this_thread::sleep_for(k_update_retry_delay);
    }
  }

  const DirectoryTree& m_tree;
  const std::wstring m_root;
  PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT m_context = nullptr;

  std::mutex m_enum_mutex;
  std::unordered_map<GUID, Enumeration, GuidHash> m_enumerations;

  // Paths we have written placeholders for, and therefore the only paths the
  // OS could be caching anything about.
  std::mutex m_placeholder_mutex;
  std::set<std::filesystem::path> m_placeholders;

  // The notifier thread and the queue on_tree_changed hands it. m_pending
  // coalesces: a path repeated across diffs is invalidated once per pass, and
  // m_everything_dirty supersedes the lot.
  std::mutex m_notify_mutex;
  std::condition_variable m_wake;
  std::set<std::filesystem::path> m_pending;
  bool m_everything_dirty = false;
  bool m_stopping = false;
  std::thread m_notifier;

  // Declared last so it is destroyed first, stopping notifications before the
  // state they touch goes away.
  std::optional<Subscription> m_subscription;
};

VirtualFileSystem::VirtualFileSystem(const DirectoryTree& tree,
                                     const std::filesystem::path& mountpoint)
    : m_impl(std::make_unique<Impl>(tree, mountpoint)) {}

VirtualFileSystem::~VirtualFileSystem() = default;

}  // namespace makebelieve
