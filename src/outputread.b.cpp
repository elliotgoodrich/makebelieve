// SPDX-License-Identifier: MIT
//
// Whether serving built outputs from memory beats serving them from files on
// disk: each benchmark opens one file and reads all of it, as a reader that
// opens an output does. The `tree` benchmarks open through the DirectoryTree
// alone; the `mount` ones go through a real mount of that tree, and `direct`
// reads the same file from disk without makebelieve, for scale. On Windows
// every open of the mount reads the whole file from the tree again; on Linux
// the kernel keeps the pages of a file opened before, so there `mount`
// measures opening an unchanged file again.
//
// Backends:
//   memory - InMemoryDirectoryTree's own bytes, as outputs are served today.
//   handle - one native handle on a file on disk, shared by every open.
//   file   - the file on disk opened afresh by each open.
#include "directorytreeutil.hpp"
#include "filesystemutil.hpp"
#include "inmemorydirectorytree.hpp"
#include "iocontext.hpp"
#include "realdirectorytree.hpp"
#include "virtualfilesystem.hpp"

#include <exec/static_thread_pool.hpp>

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace makebelieve;

constexpr std::size_t k_read_size = std::size_t{1} << 20;

// The tree it is given, but handing every open of a file the one handle that
// share() opened on it.
class SharedHandleTree final : public DirectoryTree {
  const DirectoryTree& m_tree;
  std::map<std::filesystem::path, std::shared_ptr<const OpenedFile>> m_files;

 public:
  explicit SharedHandleTree(const DirectoryTree& tree) : m_tree(tree) {}

  void share(const std::filesystem::path& path) {
    auto opened = DirectoryTreeUtil::open(m_tree, path);
    if (!opened.has_value()) {
      throw std::system_error(opened.error(), "opening a fixture");
    }
    m_files.insert_or_assign(path, std::move(*opened));
  }

  [[nodiscard]] std::expected<EntryInfo, std::error_code> status(
      const std::filesystem::path& path) const override {
    return m_tree.status(path);
  }

  [[nodiscard]] std::expected<std::vector<TreeEntry>, std::error_code> ls(
      const std::filesystem::path& path) const override {
    return m_tree.ls(path);
  }

  [[nodiscard]] OpenSender open(const std::filesystem::path& path,
                                const OpenContext& context) const override {
    if (const auto it = m_files.find(path); it != m_files.end()) {
      return stdexec::just(
          std::expected<std::shared_ptr<const OpenedFile>, std::error_code>(
              it->second));
    }
    return m_tree.open(path, context);
  }

  [[nodiscard]] Subscription subscribe_to_changes(
      const std::function<void(const DirectoryTreeDiff&)>& callback)
      const override {
    return m_tree.subscribe_to_changes(callback);
  }
};

// Everything each benchmark reads from: one file per size in every backend,
// each backend tree also mounted. Made on first use and kept for the run.
class Fixture {
 public:
  std::filesystem::path scratch =
      std::filesystem::temp_directory_path() /
      std::format("makebelieve-outputread-{}", std::random_device{}());
  std::filesystem::path objects = scratch / "objects";
  IoContext io;
  exec::static_thread_pool notifier{1};
  InMemoryDirectoryTree memory;
  std::optional<RealDirectoryTree> file;
  std::optional<SharedHandleTree> handle;
  std::optional<VirtualFileSystem> memory_mount;
  std::optional<VirtualFileSystem> handle_mount;
  std::optional<VirtualFileSystem> file_mount;

  static Fixture& get() {
    static Fixture fixture;
    return fixture;
  }

  static std::string name(std::int64_t size) {
    return std::format("{}.bin", size);
  }

  Fixture() {
    std::filesystem::create_directories(objects);
    file.emplace(objects, io);
    handle.emplace(*file);
    std::mt19937_64 random(42);
    for (const std::int64_t size : sizes()) {
      std::string bytes(static_cast<std::size_t>(size), '\0');
      for (char& byte : bytes) {
        byte = static_cast<char>(random());
      }
      const std::filesystem::path path = objects / name(size);
      if (const std::error_code error =
              FileSystemUtil::write_file(path, bytes)) {
        throw std::system_error(error, "writing a fixture");
      }
      memory.write_file(name(size), bytes);
      handle->share(name(size));
    }
    memory_mount.emplace(memory, scratch / "memory", notifier.get_scheduler());
    handle_mount.emplace(*handle, scratch / "handle", notifier.get_scheduler());
    file_mount.emplace(*file, scratch / "file", notifier.get_scheduler());
  }

  ~Fixture() {
    memory_mount.reset();
    handle_mount.reset();
    file_mount.reset();
    handle.reset();
    file.reset();
    std::error_code ignored;
    std::filesystem::remove_all(scratch, ignored);
  }

  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
  Fixture(Fixture&&) = delete;
  Fixture& operator=(Fixture&&) = delete;

  static std::vector<std::int64_t> sizes() {
    return {4 << 10, 64 << 10, 1 << 20, 16 << 20};
  }
};

void read_tree(benchmark::State& state, const DirectoryTree& tree) {
  const std::string name = Fixture::name(state.range(0));
  for (auto _ : state) {
    std::expected<std::string, std::error_code> bytes =
        DirectoryTreeUtil::wait_read(DirectoryTreeUtil::read_whole(tree, name));
    if (!bytes.has_value() ||
        bytes->size() != static_cast<std::size_t>(state.range(0))) {
      state.SkipWithError("short read");
      return;
    }
    benchmark::DoNotOptimize(bytes->data());
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

// Opens @a path and reads it to the end, as a program reading a file would.
void read_path(benchmark::State& state, const std::filesystem::path& path) {
  std::vector<char> buffer(k_read_size);
  for (auto _ : state) {
    std::ifstream in(path, std::ios::binary);
    std::size_t total = 0;
    while (
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
        in.gcount() > 0) {
      total += static_cast<std::size_t>(in.gcount());
    }
    if (total != static_cast<std::size_t>(state.range(0))) {
      state.SkipWithError("short read");
      return;
    }
    benchmark::DoNotOptimize(buffer.data());
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

void tree_memory(benchmark::State& state) {
  read_tree(state, Fixture::get().memory);
}
void tree_handle(benchmark::State& state) {
  read_tree(state, *Fixture::get().handle);
}
void tree_file(benchmark::State& state) {
  read_tree(state, *Fixture::get().file);
}
void mount_memory(benchmark::State& state) {
  read_path(state,
            Fixture::get().scratch / "memory" / Fixture::name(state.range(0)));
}
void mount_handle(benchmark::State& state) {
  read_path(state,
            Fixture::get().scratch / "handle" / Fixture::name(state.range(0)));
}
void mount_file(benchmark::State& state) {
  read_path(state,
            Fixture::get().scratch / "file" / Fixture::name(state.range(0)));
}
void direct(benchmark::State& state) {
  read_path(state, Fixture::get().objects / Fixture::name(state.range(0)));
}

// What publishing an output only once it is on disk would add to each build:
// writing it under a scratch name and renaming it into place, as the cache
// does.
void write_object(benchmark::State& state) {
  const Fixture& fixture = Fixture::get();
  const std::string bytes(static_cast<std::size_t>(state.range(0)), 'x');
  const std::filesystem::path scratch = fixture.scratch / "write.tmp";
  const std::filesystem::path object = fixture.scratch / "write.bin";
  for (auto _ : state) {
    if (FileSystemUtil::write_file(scratch, bytes)) {
      state.SkipWithError("write failed");
      return;
    }
    std::filesystem::rename(scratch, object);
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

void sizes(benchmark::internal::Benchmark* benchmark) {
  for (const std::int64_t size : Fixture::sizes()) {
    benchmark->Arg(size);
  }
}

BENCHMARK(tree_memory)->Apply(sizes);
BENCHMARK(tree_handle)->Apply(sizes);
BENCHMARK(tree_file)->Apply(sizes);
BENCHMARK(mount_memory)->Apply(sizes);
BENCHMARK(mount_handle)->Apply(sizes);
BENCHMARK(mount_file)->Apply(sizes);
BENCHMARK(direct)->Apply(sizes);
BENCHMARK(write_object)->Apply(sizes);

}  // namespace
