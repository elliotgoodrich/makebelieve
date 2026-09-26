// SPDX-License-Identifier: MIT
#include "virtualfilesystem.hpp"

#include "directorytree.hpp"
#include "processutil.hpp"
#include "tracer.hpp"

#include <stdexec/execution.hpp>

#include <windows.h>

#include <bcrypt.h>  // PNTSTATUS

#include <winfsp/winfsp.h>

#include <sddl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace makebelieve {

namespace {

// The process a request came from, as a row's group in the trace. WinFsp
// names the process itself, so there is nothing to resolve.
TraceProcess calling_process(std::uint32_t process) {
  // Only a process the trace has not seen needs a name looked up; the tracer
  // keeps the ones it has been given.
  return {.id = process,
          .name = g_tracer->knows_process(process)
                      ? std::string()
                      : ProcessUtil::name_of(process)};
}

// A nominal capacity to report for the volume. Nothing is ever written here,
// so this only exists to keep tools that divide by it happy.
constexpr UINT64 k_volume_size = UINT64{1} << 30;

// The unit WinFsp's samples use for sector size and allocation rounding.
constexpr UINT16 k_allocation_unit = 4096;

// The longest name this filesystem will hand out, and the bound on the
// over-allocated buffers below that carry one.
constexpr UINT16 k_max_component_length = 255;

// FspFileSystemNotifyBegin blocks concurrent renames, so it fails with
// STATUS_CANT_WAIT while one is in flight. Renames cannot happen on a
// read-only volume, but a retry is still cheaper than dropping the batch.
constexpr ULONG k_notify_timeout_ms = 500;
constexpr int k_notify_attempts = 5;
constexpr std::chrono::milliseconds k_notify_retry_delay{50};

// Full access for everyone, which is not the contradiction it looks like:
// ReadOnlyVolume below is what actually makes the projection read-only, at the
// volume level, and it does so whatever this says. Narrowing this instead
// would only add a second, subtler way for a legitimate read to be refused.
// This is the descriptor WinFsp's own samples use.
constexpr wchar_t k_security_descriptor[] =
    L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;WD)";

// Storage for one variable-length WinFsp record. Both FSP_FSCTL_DIR_INFO and
// FSP_FSCTL_NOTIFY_INFO end in a flexible array member holding a name, so they
// have to be over-allocated - and C++, unlike C, refuses to declare an object
// of such a type at all. Reserving the bytes and laying the record over them
// is how WinFsp's own C++ sample gets around that.
template <typename Record, std::size_t NameChars>
class RecordBuffer {
  static constexpr std::size_t k_size =
      sizeof(Record) + (NameChars * sizeof(WCHAR));

  alignas(Record) std::array<unsigned char, k_size> m_storage{};

 public:
  // The name is written through here, so the caller needs the room it has.
  static constexpr std::size_t max_name_chars = NameChars;

  [[nodiscard]] Record* get() {
    return reinterpret_cast<Record*>(m_storage.data());
  }
};

// sizeof() is the offset of the trailing name in both records - the flexible
// array member adds nothing to the size - which is what makes it the right
// base for the Size field each one carries.
static_assert(sizeof(FSP_FSCTL_DIR_INFO) ==
              FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf));
static_assert(sizeof(FSP_FSCTL_NOTIFY_INFO) ==
              FIELD_OFFSET(FSP_FSCTL_NOTIFY_INFO, FileNameBuf));

using DirEntryBuffer = RecordBuffer<FSP_FSCTL_DIR_INFO, k_max_component_length>;
using NotifyBuffer = RecordBuffer<FSP_FSCTL_NOTIFY_INFO, MAX_PATH>;

[[noreturn]] void throw_status(NTSTATUS status, const char* what) {
  // The tree, the standard library and every caller above us deal in Win32
  // codes, so an NTSTATUS is translated here rather than leaking outwards.
  throw std::system_error(static_cast<int>(FspWin32FromNtStatus(status)),
                          std::system_category(), what);
}

// WinFsp names paths from the volume root with backslashes and a leading
// separator, which is how the root itself arrives as "\". A DirectoryTree
// wants them relative to its own root, with the root as the empty path.
std::filesystem::path to_tree_path(PWSTR name) {
  std::wstring_view view(name);
  if (!view.empty() && view.front() == L'\\') {
    view.remove_prefix(1);
  }
  return {view};
}

// The reverse, for naming a path in a change notification.
std::wstring to_volume_path(std::filesystem::path path) {
  path.make_preferred();
  return L"\\" + path.wstring();
}

// MSVC's file_clock already counts 100ns ticks from the Windows epoch, which
// is precisely what every timestamp in FSP_FSCTL_FILE_INFO wants.
UINT64 to_filetime(std::chrono::file_clock::time_point time) {
  return static_cast<UINT64>(time.time_since_epoch().count());
}

// The attributes a tree entry is projected with. No FILE_ATTRIBUTE_READONLY on
// a directory, unlike a file: on a directory that flag does not mean "cannot
// be modified" - Windows uses it to mark customised folders.
UINT32 to_attributes(const EntryInfo& status) {
  return std::holds_alternative<FileInfo>(status) ? FILE_ATTRIBUTE_READONLY
                                                  : FILE_ATTRIBUTE_DIRECTORY;
}

// Describes a tree entry to WinFsp.
FSP_FSCTL_FILE_INFO make_file_info(const EntryInfo& status) {
  const auto* file = std::get_if<FileInfo>(&status);
  const UINT64 size = file != nullptr ? file->size : 0;
  const UINT64 time = to_filetime(
      file != nullptr ? file->mtime : std::get<DirectoryInfo>(status).mtime);
  return {
      .FileAttributes = to_attributes(status),
      .AllocationSize = (size + k_allocation_unit - 1) / k_allocation_unit *
                        k_allocation_unit,
      .FileSize = size,
      .CreationTime = time,
      .LastAccessTime = time,
      .LastWriteTime = time,
      .ChangeTime = time,
  };
}

// The NTSTATUS each portable error condition surfaces as. Windows has no
// errno-to-NTSTATUS translation of its own, so this is the one place that
// spells it out.
struct ErrcStatus {
  std::errc condition;
  NTSTATUS status;
};

constexpr std::array k_errc_statuses{
    ErrcStatus{.condition = std::errc::no_such_file_or_directory,
               .status = STATUS_OBJECT_NAME_NOT_FOUND},
    ErrcStatus{.condition = std::errc::permission_denied,
               .status = STATUS_ACCESS_DENIED},
    ErrcStatus{.condition = std::errc::operation_not_permitted,
               .status = STATUS_ACCESS_DENIED},
    ErrcStatus{.condition = std::errc::is_a_directory,
               .status = STATUS_FILE_IS_A_DIRECTORY},
    ErrcStatus{.condition = std::errc::not_a_directory,
               .status = STATUS_NOT_A_DIRECTORY},
    ErrcStatus{.condition = std::errc::invalid_argument,
               .status = STATUS_INVALID_PARAMETER},
    ErrcStatus{.condition = std::errc::file_exists,
               .status = STATUS_OBJECT_NAME_COLLISION},
    ErrcStatus{.condition = std::errc::directory_not_empty,
               .status = STATUS_DIRECTORY_NOT_EMPTY},
    ErrcStatus{.condition = std::errc::filename_too_long,
               .status = STATUS_NAME_TOO_LONG},
    ErrcStatus{.condition = std::errc::not_enough_memory,
               .status = STATUS_INSUFFICIENT_RESOURCES},
    ErrcStatus{.condition = std::errc::no_space_on_device,
               .status = STATUS_DISK_FULL},
    ErrcStatus{.condition = std::errc::read_only_file_system,
               .status = STATUS_MEDIA_WRITE_PROTECTED},
    ErrcStatus{.condition = std::errc::device_or_resource_busy,
               .status = STATUS_DEVICE_BUSY},
    ErrcStatus{.condition = std::errc::too_many_files_open,
               .status = STATUS_TOO_MANY_OPENED_FILES},
    ErrcStatus{.condition = std::errc::operation_canceled,
               .status = STATUS_CANCELLED},
    ErrcStatus{.condition = std::errc::timed_out, .status = STATUS_IO_TIMEOUT},
    ErrcStatus{.condition = std::errc::io_error,
               .status = STATUS_IO_DEVICE_ERROR},
    ErrcStatus{.condition = std::errc::function_not_supported,
               .status = STATUS_NOT_SUPPORTED},
    ErrcStatus{.condition = std::errc::not_supported,
               .status = STATUS_NOT_SUPPORTED},
    ErrcStatus{.condition = std::errc::operation_not_supported,
               .status = STATUS_NOT_SUPPORTED},
};

// Translates a tree error into the NTSTATUS WinFsp expects. The two
// DirectoryTree implementations report from different categories -
// RealDirectoryTree hands back Win32 codes, the in-memory trees hand back
// std::errc - so each is translated on its own terms rather than feeding a
// POSIX errno to a function that expects a Win32 error. Anything else is
// matched by the portable condition it is equivalent to.
NTSTATUS to_ntstatus(const std::error_code& error) {
  if (error.category() == std::system_category()) {
    return FspNtStatusFromWin32(static_cast<DWORD>(error.value()));
  }
  for (const ErrcStatus& entry : k_errc_statuses) {
    if (error == entry.condition) {
      return entry.status;
    }
  }
  return STATUS_UNSUCCESSFUL;
}

// An absolute mountpoint with no trailing separator. `mnt\` names the same
// directory as `mnt`, but its parent_path() is `mnt` itself - which would have
// create_mountpoint make the very directory WinFsp needs to create - and WinFsp
// crashes when handed a mount point spelled with the separator.
std::filesystem::path normalize_mountpoint(
    const std::filesystem::path& mountpoint) {
  std::filesystem::path result =
      std::filesystem::absolute(mountpoint).lexically_normal();
  while (!result.has_filename() && result.has_relative_path()) {
    result = result.parent_path();
  }
  return result;
}

// How a change is announced. An addition dominates a modification when the two
// coalesce: a watcher that never heard of the new name only makes sense of an
// "added".
enum class ChangeKind : std::uint8_t { modified, added };

using Changes = std::map<std::filesystem::path, ChangeKind>;

// Folds @a from into @a into, keeping the dominant kind for a repeated path.
void merge_changes(Changes& into, const Changes& from) {
  for (const auto& [path, kind] : from) {
    const auto [it, inserted] = into.try_emplace(path, kind);
    if (!inserted && kind == ChangeKind::added) {
      it->second = ChangeKind::added;
    }
  }
}

// Readies `mountpoint` for WinFsp, which creates the mount directory itself -
// as a reparse point into the volume - and removes it again on unmount. So
// unlike a provider that virtualizes an existing directory, this refuses a
// path that is already there and only makes sure the parent exists for WinFsp
// to create into.
void create_mountpoint(const std::filesystem::path& mountpoint) {
  std::error_code error;
  if (std::filesystem::exists(mountpoint, error)) {
    throw std::filesystem::filesystem_error(
        "mountpoint already exists; refusing to reuse it", mountpoint,
        std::make_error_code(std::errc::file_exists));
  }

  const std::filesystem::path parent = mountpoint.parent_path();
  if (parent.empty()) {
    return;
  }
  std::filesystem::create_directories(parent, error);
  if (error) {
    throw std::filesystem::filesystem_error("could not create mountpoint",
                                            mountpoint, error);
  }
}

// Removes whatever is left of the mount directory once the filesystem has
// stopped. WinFsp normally takes it away with the mount, so this is only here
// to catch the case where it did not.
void remove_mountpoint(const std::filesystem::path& mountpoint) noexcept {
  std::error_code error;
  std::filesystem::remove(mountpoint, error);
}

// The single security descriptor every entry in this filesystem reports,
// built once from SDDL and freed on destruction.
class SecurityDescriptor {
  PSECURITY_DESCRIPTOR m_descriptor = nullptr;
  ULONG m_size = 0;

 public:
  SecurityDescriptor() {
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            k_security_descriptor, SDDL_REVISION_1, &m_descriptor, &m_size)) {
      throw std::system_error(static_cast<int>(GetLastError()),
                              std::system_category(),
                              "ConvertStringSecurityDescriptorToSecurityDescri"
                              "ptorW failed");
    }
  }

  ~SecurityDescriptor() {
    if (m_descriptor != nullptr) {
      LocalFree(m_descriptor);
    }
  }

  SecurityDescriptor(const SecurityDescriptor&) = delete;
  SecurityDescriptor& operator=(const SecurityDescriptor&) = delete;
  SecurityDescriptor(SecurityDescriptor&&) = delete;
  SecurityDescriptor& operator=(SecurityDescriptor&&) = delete;

  [[nodiscard]] PSECURITY_DESCRIPTOR get() const { return m_descriptor; }
  [[nodiscard]] SIZE_T size() const { return m_size; }
};

}  // namespace

// WinFsp provider over a DirectoryTree.
//
// WinFsp dispatches operations on its own pool threads, concurrently, so every
// member here is either immutable after construction or guarded. The tree's
// own const-means-thread-safe contract is what makes querying it from those
// threads sound.
class VirtualFileSystem::Impl {
 public:
  // Selects the constructor that claims the mountpoint and starts nothing, so
  // that the one below can delegate to it.
  struct Unstarted {};

  // Delegates first, so that once the mountpoint is claimed the object counts
  // as constructed: anything below that throws still runs ~Impl, which tears
  // down exactly the parts that got started.
  Impl(const DirectoryTree& tree,
       const std::filesystem::path& mountpoint,
       exec::static_thread_pool::scheduler scheduler)
      : Impl(tree, mountpoint, scheduler, Unstarted{}) {
    // WinFsp's DLL lives in its own install directory rather than anywhere the
    // loader searches, so the import library is delay-loaded and this is what
    // resolves it. It has to run before any other WinFsp call, and it is what
    // turns "WinFsp is not installed" into a diagnosable error rather than a
    // loader failure before main().
    if (const NTSTATUS status = FspLoad(nullptr); !NT_SUCCESS(status)) {
      throw_status(status, "FspLoad failed; is WinFsp installed?");
    }

    const UINT64 created = to_filetime(std::chrono::file_clock::now());
    const FSP_FSCTL_VOLUME_PARAMS params = {
        .SectorSize = k_allocation_unit,
        .SectorsPerAllocationUnit = 1,
        .MaxComponentLength = k_max_component_length,
        .VolumeCreationTime = created,
        .VolumeSerialNumber = static_cast<UINT32>(created >> 16U),
        // No metadata caching at all: every stat and every read reaches the
        // tree, which is what lets a lazily built output be correct the moment
        // it is asked for rather than whenever an invalidation catches up.
        .FileInfoTimeout = 0,
        // Case-sensitive, because the tree is: a lookup reaches it with the
        // case the caller typed and it matches exactly. Declaring otherwise
        // has Windows promise case-insensitive lookups the tree cannot keep,
        // and also changes what FspFileSystemNotify expects names to look like
        // - a case-insensitive volume that does not normalize names must
        // announce them upper-cased, and anything else below the root is
        // silently dropped.
        .CaseSensitiveSearch = 1,
        .CasePreservedNames = 1,
        .UnicodeOnDisk = 1,
        .PersistentAcls = 0,
        // The whole projection is read-only; this is what enforces it, so no
        // operation below has to check.
        .ReadOnlyVolume = 1,
        // Every Cleanup reaches user mode, not only those for modified files:
        // the last handle closing is what releases notifications held back
        // for an open file (see cleanup()).
        .PostCleanupWhenModifiedOnly = 0,
        // The context open() hands back is one OpenFile per handle, not one
        // per file. By default WinFsp assumes the latter - every open of a
        // name must return the same pointer while any handle to it is live -
        // and it keeps just one of them per file, closing that one pointer
        // once per handle. Handing it a fresh OpenFile each time then
        // double-frees whenever two handles to one file overlap. This flag
        // makes the context per-handle, which is what OpenFile, its directory
        // buffer and m_open_counts assume.
        .UmFileContextIsUserContext2 = 1,
        .FileSystemName = L"makebelieve",
    };

    // FspFileSystemCreate takes a mutable pointer it never writes through, so
    // the name is spelled as a string rather than cast from a literal.
    std::wstring device(L"" FSP_FSCTL_DISK_DEVICE_NAME);
    if (const NTSTATUS status = FspFileSystemCreate(
            device.data(), &params, &interface_table(), &m_filesystem);
        !NT_SUCCESS(status)) {
      throw_status(status, "FspFileSystemCreate failed");
    }
    m_filesystem->UserContext = this;

    if (const NTSTATUS status =
            FspFileSystemSetMountPoint(m_filesystem, m_root.data());
        !NT_SUCCESS(status)) {
      throw_status(status, "FspFileSystemSetMountPoint failed");
    }
    m_mounted = true;

    // 0 asks for WinFsp's default thread count, which is what every sample
    // uses and scales with the machine.
    if (const NTSTATUS status = FspFileSystemStartDispatcher(m_filesystem, 0);
        !NT_SUCCESS(status)) {
      throw_status(status, "FspFileSystemStartDispatcher failed");
    }
    m_dispatching = true;

    // The subscription is taken last so no notification can arrive before
    // there is a filesystem to announce it through.
    m_subscription.emplace(m_tree.subscribe_to_changes(
        [this](const DirectoryTreeDiff& diff) { on_tree_changed(diff); }));
  }

  // Absolute, because WinFsp holds the mount point for as long as the
  // filesystem lives, by which time the process's working directory may have
  // moved on from whatever made a relative path meaningful. Fails unless it
  // can claim the path fresh - see create_mountpoint - so teardown only ever
  // removes what this object brought into being.
  Impl(const DirectoryTree& tree,
       const std::filesystem::path& mountpoint,
       exec::static_thread_pool::scheduler scheduler,
       Unstarted)
      : m_tree(tree),
        m_mountpoint(normalize_mountpoint(mountpoint)),
        m_root(m_mountpoint.wstring()),
        m_scheduler(scheduler) {
    create_mountpoint(m_mountpoint);
  }

  ~Impl() {
    // Unsubscribe first: a notification landing after the dispatcher stops
    // would call into a torn-down filesystem.
    m_subscription.reset();

    // Then stop the notifier and wait out a pass under way, which sees
    // m_stopping and ends after the batch it is on. It issues
    // FspFileSystemNotify against m_filesystem, so it has to be gone before
    // that object is deleted.
    {
      const std::lock_guard<std::mutex> lock(m_notify_mutex);
      m_stopping = true;
    }
    stdexec::sync_wait(m_notifications.join());

    if (m_filesystem != nullptr) {
      if (m_dispatching) {
        FspFileSystemStopDispatcher(m_filesystem);
      }
      if (m_mounted) {
        // Takes the mount directory with it. Done before the delete below so
        // that reaching the end of this destructor really does mean the mount
        // is gone, which is what this class promises.
        FspFileSystemRemoveMountPoint(m_filesystem);
      }
      FspFileSystemDelete(m_filesystem);
    }

    // Only once WinFsp has had the path: before that nothing here created it,
    // and whatever is there is not ours to remove.
    if (m_mounted) {
      remove_mountpoint(m_mountpoint);
    }
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

 private:
  // What Open hands back to WinFsp and every later operation on that handle
  // hands us - one per handle, see UmFileContextIsUserContext2. It holds the
  // path rather than a snapshot of the entry, so a read through a long-lived
  // handle sees the tree as it is now rather than as it was when the file was
  // opened - which is the whole point of a build that fills the file in later.
  struct OpenFile {
    std::filesystem::path path;

    // WinFsp's directory buffer: filled on the first ReadDirectory for this
    // handle, then read back from in sorted, resumable order.
    PVOID directory_buffer = nullptr;

    // Whether this handle is still counted in m_open_counts. Cleared by
    // whichever of cleanup() and close() gets there first.
    bool counted = false;
  };

  // Bridges an FSP_FILE_SYSTEM_INTERFACE C callback to a member function,
  // recovering the instance from FSP_FILE_SYSTEM::UserContext.
  template <auto MemFn>
  struct Bridge;

  template <typename... Args, NTSTATUS (Impl::*MemFn)(Args...)>
  struct Bridge<MemFn> {
    static NTSTATUS call(FSP_FILE_SYSTEM* filesystem, Args... args) {
      try {
        auto* self = static_cast<Impl*>(filesystem->UserContext);
        return (self->*MemFn)(args...);
      } catch (const std::bad_alloc&) {
        return STATUS_INSUFFICIENT_RESOURCES;
      } catch (const std::system_error& error) {
        return to_ntstatus(error.code());
      } catch (...) {
        return STATUS_UNSUCCESSFUL;
      }
    }
  };

  template <typename... Args, VOID (Impl::*MemFn)(Args...)>
  struct Bridge<MemFn> {
    static VOID call(FSP_FILE_SYSTEM* filesystem, Args... args) {
      try {
        auto* self = static_cast<Impl*>(filesystem->UserContext);
        (self->*MemFn)(args...);
      } catch (...) {  // NOLINT(bugprone-empty-catch)
        // Cleanup and Close are the void operations, and neither has
        // anywhere to report a failure to.
      }
    }
  };

  template <auto MemFn>
  static constexpr auto trampoline = Bridge<MemFn>::call;

  // The operations this filesystem implements. Everything left out is
  // unsupported, which for a read-only projection is most of the interface.
  // FspFileSystemCreate keeps the pointer rather than copying, so this outlives
  // every filesystem built from it.
  static const FSP_FILE_SYSTEM_INTERFACE& interface_table() {
    static const FSP_FILE_SYSTEM_INTERFACE table = {
        .GetVolumeInfo = trampoline<&Impl::get_volume_info>,
        .GetSecurityByName = trampoline<&Impl::get_security_by_name>,
        .Create = trampoline<&Impl::create>,
        .Open = trampoline<&Impl::open>,
        .Overwrite = trampoline<&Impl::overwrite>,
        .Cleanup = trampoline<&Impl::cleanup>,
        .Close = trampoline<&Impl::close>,
        .Read = trampoline<&Impl::read>,
        .GetFileInfo = trampoline<&Impl::get_file_info>,
        .CanDelete = trampoline<&Impl::can_delete>,
        .ReadDirectory = trampoline<&Impl::read_directory>,
    };
    return table;
  }

  NTSTATUS get_volume_info(FSP_FSCTL_VOLUME_INFO* info) {
    *info = {
        .TotalSize = k_volume_size,
        .FreeSize = 0,  // nothing can ever be written here
        // In bytes, and without the terminator the literal carries.
        .VolumeLabelLength = sizeof(L"makebelieve") - sizeof(WCHAR),
        .VolumeLabel = L"makebelieve",
    };
    return STATUS_SUCCESS;
  }

  // Answers "does this path exist, and who may touch it" without opening
  // anything. WinFsp calls this while resolving a path, so it runs for every
  // component on the way to a file as well as for the file itself.
  NTSTATUS get_security_by_name(PWSTR name,
                                PUINT32 attributes,
                                PSECURITY_DESCRIPTOR descriptor,
                                SIZE_T* descriptor_size) {
    const std::expected<EntryInfo, std::error_code> status =
        m_tree.status(to_tree_path(name));
    if (!status.has_value()) {
      return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    if (attributes != nullptr) {
      *attributes = to_attributes(*status);
    }
    if (descriptor_size != nullptr) {
      if (m_security.size() > *descriptor_size) {
        // Asks WinFsp to come back with a buffer this big.
        *descriptor_size = m_security.size();
        return STATUS_BUFFER_OVERFLOW;
      }
      *descriptor_size = m_security.size();
      if (descriptor != nullptr) {
        std::memcpy(descriptor, m_security.get(), m_security.size());
      }
    }
    return STATUS_SUCCESS;
  }

  // Nothing here can be created or truncated, and ReadOnlyVolume means the FSD
  // turns such a request away before it ever reaches user mode. These two exist
  // anyway because WinFsp treats Create, Open and Overwrite as one block and
  // refuses to dispatch a create request at all - a plain FILE_OPEN of an
  // existing file included - unless all three are wired. Leaving either out
  // fails every open with STATUS_INVALID_DEVICE_REQUEST.
  NTSTATUS create(PWSTR,
                  UINT32,
                  UINT32,
                  UINT32,
                  PSECURITY_DESCRIPTOR,
                  UINT64,
                  PVOID*,
                  FSP_FSCTL_FILE_INFO*) {
    return STATUS_MEDIA_WRITE_PROTECTED;
  }

  NTSTATUS overwrite(PVOID, UINT32, BOOLEAN, UINT64, FSP_FSCTL_FILE_INFO*) {
    return STATUS_MEDIA_WRITE_PROTECTED;
  }

  // A handle that can read data blocks in the tree's open() until the file is
  // final, and reports that size. Stats also open files, but for attributes
  // only, so they neither wait nor build.
  NTSTATUS open(PWSTR name,
                UINT32 create_options,
                UINT32 granted_access,
                PVOID* file_context,
                FSP_FSCTL_FILE_INFO* file_info) {
    const std::filesystem::path path = to_tree_path(name);
    // WinFsp names the process, not the thread within it.
    const std::uint32_t caller = FspFileSystemOperationProcessId();
    MB_TRACE_POOL_SCOPE(m_reader_lanes, calling_process(caller), "vfs",
                        std::tie("open", path), "caller", caller);
    std::expected<EntryInfo, std::error_code> status = m_tree.status(path);
    if (!status.has_value()) {
      return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (std::holds_alternative<FileInfo>(*status) &&
        (granted_access & (FILE_READ_DATA | FILE_EXECUTE)) != 0) {
      const std::expected<FileInfo, std::error_code> opened = m_tree.open(path);
      if (!opened.has_value()) {
        return to_ntstatus(opened.error());
      }
      status = *opened;
    }

    const bool is_directory = std::holds_alternative<DirectoryInfo>(*status);
    if (is_directory && (create_options & FILE_NON_DIRECTORY_FILE) != 0) {
      return STATUS_FILE_IS_A_DIRECTORY;
    }
    if (!is_directory && (create_options & FILE_DIRECTORY_FILE) != 0) {
      return STATUS_NOT_A_DIRECTORY;
    }

    auto handle = std::make_unique<OpenFile>();
    handle->path = path;
    if (!is_directory) {
      remember(path);
      const std::lock_guard<std::mutex> lock(m_open_mutex);
      ++m_open_counts[path];
      handle->counted = true;
    }

    *file_info = make_file_info(*status);
    *file_context = handle.release();
    return STATUS_SUCCESS;
  }

  // The handle is gone from the process that held it. The file itself can
  // outlive this - a memory mapping keeps it referenced, and Close only comes
  // once the mapping goes too - so this, not close(), is where the handle
  // stops counting as open.
  VOID cleanup(PVOID file_context, PWSTR /*name*/, ULONG /*flags*/) {
    uncount(*static_cast<OpenFile*>(file_context));
  }

  // Normally leaves the count alone, since cleanup() already dropped it; it
  // still covers a handle WinFsp closes without cleaning it up first.
  VOID close(PVOID file_context) {
    const std::unique_ptr<OpenFile> handle(
        static_cast<OpenFile*>(file_context));
    FspFileSystemDeleteDirectoryBuffer(&handle->directory_buffer);
    uncount(*handle);
  }

  // Nothing can be deleted. ReadOnlyVolume refuses writes and a file's
  // read-only attribute refuses its deletion, but neither covers deleting a
  // directory: without this, that reports success and does nothing.
  NTSTATUS can_delete(PVOID /*file_context*/, PWSTR /*name*/) {
    return STATUS_MEDIA_WRITE_PROTECTED;
  }

  // Drops @a handle from m_open_counts, once, and lets out any notification
  // held back for its file if it was the last handle on it.
  void uncount(OpenFile& handle) {
    bool last = false;
    {
      const std::lock_guard<std::mutex> lock(m_open_mutex);
      if (!std::exchange(handle.counted, false)) {
        return;
      }
      const auto it = m_open_counts.find(handle.path);
      if (it != m_open_counts.end() && --it->second <= 0) {
        m_open_counts.erase(it);
        last = true;
      }
    }
    // Outside m_open_mutex: this runs on a dispatcher thread, and the notifier
    // takes these two locks the other way round.
    if (last) {
      release_deferred(handle.path);
    }
  }

  NTSTATUS read(PVOID file_context,
                PVOID buffer,
                UINT64 offset,
                ULONG length,
                PULONG bytes_transferred) {
    const auto* handle = static_cast<const OpenFile*>(file_context);
    const std::expected<std::string, std::error_code> bytes =
        m_tree.read(handle->path, static_cast<Offset>(offset), length);
    if (!bytes.has_value()) {
      return to_ntstatus(bytes.error());
    }
    if (bytes->empty()) {
      // read() promises only "up to size bytes", and nothing at all is how it
      // spells a read that started at or past the end of the file.
      return STATUS_END_OF_FILE;
    }

    const auto count =
        static_cast<ULONG>(std::min<std::size_t>(length, bytes->size()));
    std::memcpy(buffer, bytes->data(), count);
    *bytes_transferred = count;
    return STATUS_SUCCESS;
  }

  // Re-queries rather than reporting what Open saw, so a file that was built
  // while this handle was open reports its real size here. FileInfoTimeout is
  // 0, so Windows asks every time rather than trusting a cached answer.
  NTSTATUS get_file_info(PVOID file_context, FSP_FSCTL_FILE_INFO* file_info) {
    const auto* handle = static_cast<const OpenFile*>(file_context);
    const std::expected<EntryInfo, std::error_code> status =
        m_tree.status(handle->path);
    if (!status.has_value()) {
      return to_ntstatus(status.error());
    }
    *file_info = make_file_info(*status);
    return STATUS_SUCCESS;
  }

  // Lists a directory through WinFsp's directory buffer, which is what makes
  // an unordered ls() usable: it sorts what goes in and resumes from `marker`
  // on the calls that drain it. The buffer is filled once per handle, so a
  // listing cannot duplicate or skip entries because the tree shifted halfway
  // through being read out.
  NTSTATUS read_directory(PVOID file_context,
                          PWSTR /*pattern*/,
                          PWSTR marker,
                          PVOID buffer,
                          ULONG length,
                          PULONG bytes_transferred) {
    auto* handle = static_cast<OpenFile*>(file_context);

    NTSTATUS result = STATUS_SUCCESS;
    if (FspFileSystemAcquireDirectoryBuffer(&handle->directory_buffer,
                                            marker == nullptr, &result)) {
      fill_directory_buffer(handle, &result);
      FspFileSystemReleaseDirectoryBuffer(&handle->directory_buffer);
    }
    if (!NT_SUCCESS(result)) {
      return result;
    }

    FspFileSystemReadDirectoryBuffer(&handle->directory_buffer, marker, buffer,
                                     length, bytes_transferred);
    return STATUS_SUCCESS;
  }

  void fill_directory_buffer(OpenFile* handle, PNTSTATUS result) {
    const std::expected<std::vector<TreeEntry>, std::error_code> entries =
        m_tree.ls(handle->path);
    if (!entries.has_value()) {
      *result = to_ntstatus(entries.error());
      return;
    }

    // Windows expects the dot entries from every directory but the volume
    // root, where there is no parent to name.
    if (!handle->path.empty()) {
      const std::expected<EntryInfo, std::error_code> self =
          m_tree.status(handle->path);
      const std::expected<EntryInfo, std::error_code> parent =
          m_tree.status(handle->path.parent_path());
      if (self.has_value() && !add_dir_entry(handle, L".", *self, result)) {
        return;
      }
      if (parent.has_value() &&
          !add_dir_entry(handle, L"..", *parent, result)) {
        return;
      }
    }

    for (const TreeEntry& entry : *entries) {
      // TreeEntry::name is the leaf name only.
      if (!add_dir_entry(handle, entry.name.wstring(), entry.info, result)) {
        return;
      }
    }
  }

  static bool add_dir_entry(OpenFile* handle,
                            const std::wstring& name,
                            const EntryInfo& status,
                            PNTSTATUS result) {
    if (name.size() > DirEntryBuffer::max_name_chars) {
      return true;  // unnameable here, so skip it rather than fail the listing
    }

    // Field by field rather than from a braced initializer: MSVC will not
    // create even a temporary of a type ending in a flexible array member. The
    // storage starts zeroed, so the fields left alone are already right.
    DirEntryBuffer storage;
    FSP_FSCTL_DIR_INFO* entry = storage.get();
    entry->Size = static_cast<UINT16>(sizeof(FSP_FSCTL_DIR_INFO) +
                                      (name.size() * sizeof(WCHAR)));
    entry->FileInfo = make_file_info(status);
    std::memcpy(entry->FileNameBuf, name.data(), name.size() * sizeof(WCHAR));

    return FspFileSystemFillDirectoryBuffer(&handle->directory_buffer, entry,
                                            result);
  }

  // Records a file something has opened through us, which bounds what an
  // "everything changed" notification has to announce. It only grows while the
  // mount lives (minus what notify() finds deleted).
  void remember(const std::filesystem::path& path) {
    const std::lock_guard<std::mutex> lock(m_known_mutex);
    m_known.insert(path);
  }

  // Queues a tree change for the notifier thread.
  //
  // Runs on whichever thread the tree notifies from, so it does no I/O. The
  // first read of a lazily-built output runs the build synchronously inside
  // read(), and the tree change that build produces is delivered right there
  // on a WinFsp dispatcher thread; announcing it from that stack would have
  // the filesystem call into WinFsp in the middle of servicing a WinFsp
  // request. Handing it to the notifier defers it until read() has returned.
  void on_tree_changed(const DirectoryTreeDiff& diff) {
    const std::lock_guard<std::mutex> lock(m_notify_mutex);
    if (m_stopping) {
      return;
    }
    if (diff.everything_dirty) {
      m_everything_dirty = true;
    } else {
      // An entry whose parent's child list changed in the same diff was added
      // or removed rather than rewritten; which of the two is settled when it
      // is announced, by whether it still exists. That is all
      // child_lists_changed is needed for: a watcher on the parent learns about
      // a new or missing child from the child's own record.
      const std::set<std::filesystem::path> parents(
          diff.child_lists_changed.begin(), diff.child_lists_changed.end());
      Changes changes;
      for (const std::filesystem::path& path : diff.entries_changed) {
        changes.emplace(path, parents.contains(path.parent_path())
                                  ? ChangeKind::added
                                  : ChangeKind::modified);
      }
      merge_changes(m_pending, changes);
    }
    start_notifying();
  }

  // Starts a pass announcing the queued changes, unless one is under way or we
  // are stopping. A pass that cannot be started is retried by the next change.
  // @pre m_notify_mutex is held.
  void start_notifying() noexcept {
    if (m_notifying || m_stopping) {
      return;
    }
    m_notifying = true;
    try {
      stdexec::spawn(stdexec::schedule(m_scheduler) |
                         stdexec::then([this]() noexcept { notify_pending(); }),
                     m_notifications.get_token());
    } catch (...) {
      m_notifying = false;
    }
  }

  // One pass of the notifier: announces each affected path in turn until
  // nothing is queued, then stands down.
  void notify_pending() noexcept {
    MB_TRACE_SCOPE("vfs", "notify");
    std::unique_lock<std::mutex> lock(m_notify_mutex);
    // Best-effort: a change dropped for want of memory goes unannounced, as
    // one a watcher's own buffer overflows on does.
    try {
      while (!m_stopping && (m_everything_dirty || !m_pending.empty())) {
        Changes batch;
        if (m_everything_dirty) {
          // The tree lost track of what changed, so everything handed out
          // could be stale. Announce the paths we know were opened rather than
          // walking the mount: enumerating our own volume would re-enter the
          // operations above from here.
          m_everything_dirty = false;
          m_pending.clear();
          const std::lock_guard<std::mutex> known_lock(m_known_mutex);
          for (const std::filesystem::path& path : m_known) {
            batch.emplace(path, ChangeKind::modified);
          }
        } else {
          batch = std::exchange(m_pending, {});
        }

        // Unlocked for the announcements themselves, so a change arriving
        // mid-batch simply queues up for the next round.
        lock.unlock();
        send_notifications(std::move(batch));
        lock.lock();
      }
    } catch (...) {  // NOLINT(bugprone-empty-catch)
      if (!lock.owns_lock()) {
        lock.lock();
      }
    }
    m_notifying = false;
  }

  void send_notifications(Changes batch) {
    // Windows silently drops a change notification naming a file that has a
    // handle open on it - FspFileSystemNotify still reports success - so
    // anything open is set aside here for cleanup() to announce once the reader
    // has let go. That is exactly the shape of a first build: the read that
    // triggers it is still holding the file when the build reports back.
    Changes deferred;
    for (auto it = batch.begin(); it != batch.end();) {
      if (is_open(it->first)) {
        const auto open = it++;
        deferred.insert(batch.extract(open));
      } else {
        ++it;
      }
    }
    park(deferred);

    if (batch.empty()) {
      return;
    }
    for (int attempt = 0; attempt < k_notify_attempts; ++attempt) {
      if (FspFileSystemNotifyBegin(m_filesystem, k_notify_timeout_ms) ==
          STATUS_SUCCESS) {
        for (const auto& [path, kind] : batch) {
          notify(path, kind);
        }
        FspFileSystemNotifyEnd(m_filesystem);
        return;
      }
      std::this_thread::sleep_for(k_notify_retry_delay);
    }
  }

  [[nodiscard]] bool is_open(const std::filesystem::path& path) {
    const std::lock_guard<std::mutex> lock(m_open_mutex);
    return m_open_counts.contains(path);
  }

  // Holds changes to files that are open until cleanup() lets them out.
  void park(const Changes& changes) {
    if (changes.empty()) {
      return;
    }
    {
      const std::lock_guard<std::mutex> lock(m_notify_mutex);
      if (m_stopping) {
        return;
      }
      merge_changes(m_deferred, changes);
    }
    // A file that closed between the is_open() test above and this park missed
    // its release and would sit here until something changed it again, so
    // whatever is already closed is taken straight back out.
    for (const auto& [path, kind] : parked()) {
      if (!is_open(path)) {
        release_deferred(path);
      }
    }
  }

  [[nodiscard]] Changes parked() {
    const std::lock_guard<std::mutex> lock(m_notify_mutex);
    return m_deferred;
  }

  // Requeues the change parked against @a path, if there is one.
  void release_deferred(const std::filesystem::path& path) {
    {
      const std::lock_guard<std::mutex> lock(m_notify_mutex);
      const auto it = m_deferred.find(path);
      if (m_stopping || it == m_deferred.end()) {
        return;
      }
      merge_changes(m_pending, Changes{*it});
      m_deferred.erase(it);
      start_notifying();
    }
  }

  // Announces one changed path, so anything watching the mount with
  // ReadDirectoryChangesW hears about it.
  //
  // This is only ever a message. Nothing about the file itself has to be
  // pushed anywhere, because FileInfoTimeout is 0 and Windows caches none of
  // it: a reader that comes back after this reaches the tree and gets whatever
  // is there now.
  void notify(const std::filesystem::path& path, ChangeKind kind) {
    const std::wstring name = to_volume_path(path);
    if (name.size() > NotifyBuffer::max_name_chars) {
      return;
    }

    const std::expected<EntryInfo, std::error_code> status =
        m_tree.status(path);
    const bool present = status.has_value();
    if (!present) {
      // Gone from the tree. Forget it, so a path that comes back is re-learned
      // by the open that finds it.
      const std::lock_guard<std::mutex> lock(m_known_mutex);
      m_known.erase(path);
    }

    struct Announcement {
      UINT32 filter;
      UINT32 action;
    };
    const Announcement announcement = [&]() -> Announcement {
      if (!present) {
        // The entry is gone, and with it any record of whether it was a file
        // or a directory, so both kinds of watcher are told.
        return {.filter =
                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
                .action = FILE_ACTION_REMOVED};
      }
      if (kind == ChangeKind::added) {
        return {.filter = std::holds_alternative<DirectoryInfo>(*status)
                              ? UINT32{FILE_NOTIFY_CHANGE_DIR_NAME}
                              : UINT32{FILE_NOTIFY_CHANGE_FILE_NAME},
                .action = FILE_ACTION_ADDED};
      }
      return {.filter = FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
              .action = FILE_ACTION_MODIFIED};
    }();

    // Field by field for the same reason as in add_dir_entry.
    NotifyBuffer storage;
    FSP_FSCTL_NOTIFY_INFO* info = storage.get();
    info->Size = static_cast<UINT16>(sizeof(FSP_FSCTL_NOTIFY_INFO) +
                                     (name.size() * sizeof(WCHAR)));
    info->Filter = announcement.filter;
    info->Action = announcement.action;
    std::memcpy(info->FileNameBuf, name.data(), name.size() * sizeof(WCHAR));

    FspFileSystemNotify(m_filesystem, info, info->Size);
  }

  // One row per open being served at once, so a reader's whole request - the
  // build it waits for included - reads as one worker rather than as whichever
  // WinFsp thread happened to take it.
  mutable TraceLanePool m_reader_lanes{"reader"};

  const DirectoryTree& m_tree;
  const std::filesystem::path m_mountpoint;

  // The mount point as WinFsp wants it: its own buffer, because
  // FspFileSystemSetMountPoint takes a mutable pointer.
  std::wstring m_root;

  // Where notifier passes run.
  exec::static_thread_pool::scheduler m_scheduler;

  const SecurityDescriptor m_security;

  FSP_FILE_SYSTEM* m_filesystem = nullptr;
  bool m_mounted = false;
  bool m_dispatching = false;

  // Files something has opened through us, and so the candidate set for an
  // "everything changed" notification.
  std::mutex m_known_mutex;
  std::set<std::filesystem::path> m_known;

  // Files with a handle open right now, counted because one file can be open
  // several times over. Consulted before every notification and drained by
  // cleanup().
  std::mutex m_open_mutex;
  std::map<std::filesystem::path, int> m_open_counts;

  // The queue on_tree_changed hands the notifier. m_pending coalesces: a path
  // repeated across diffs is announced once per pass, and m_everything_dirty
  // supersedes the lot.
  std::mutex m_notify_mutex;
  Changes m_pending;

  // Changes that could not be announced because the file was open, waiting on
  // the cleanup that lets them through.
  Changes m_deferred;

  bool m_everything_dirty = false;
  bool m_stopping = false;

  // Whether a notifier pass is under way, and that pass, joined on
  // destruction.
  bool m_notifying = false;
  stdexec::counting_scope m_notifications;

  // Declared last so it is destroyed first, stopping notifications before the
  // state they touch goes away.
  std::optional<Subscription> m_subscription;
};

VirtualFileSystem::VirtualFileSystem(
    const DirectoryTree& tree,
    const std::filesystem::path& mountpoint,
    exec::static_thread_pool::scheduler scheduler)
    : m_impl(std::make_unique<Impl>(tree, mountpoint, scheduler)) {}

VirtualFileSystem::~VirtualFileSystem() = default;

}  // namespace makebelieve
