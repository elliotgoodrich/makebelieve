// Prints a fixed line, so a test can tell it ran in full from a copy of it
// generated through the mount.
//
// Given two paths instead, it is a program that keeps running: it writes
// "started" to the first, waits for the second to exist, and only then runs
// the rest of its code, appending "finished" to the first. A test can change
// the file it was started from in between.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

int main(int argc, char** argv) {
  if (argc == 3) {
    std::ofstream(argv[1]) << "started\n";
    while (!std::filesystem::exists(argv[2])) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::ofstream(argv[1], std::ios::app) << "finished\n";
    return 0;
  }
  std::puts("hello from a generated executable");
  return 0;
}
