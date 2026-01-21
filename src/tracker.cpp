#include "tracker.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>

#include "interceptor_embedded.h"
#include "logger.h"
#include "thread_pool.h"
#include "utils.h"

Tracker::Tracker(std::shared_ptr<BuildInfo> build_info)
    : build_info_(build_info) {
  ignore_patterns_.push_back("/tmp/");
  ignore_patterns_.push_back("/proc/");
  ignore_patterns_.push_back("/sys/");
  ignore_patterns_.push_back("/dev/");
}

void Tracker::addIgnorePattern(const std::string& pattern) {
  ignore_patterns_.push_back(pattern);
}

std::string Tracker::executeWithStrace(const std::string& command) {
  std::string strace_log =
      build_info_->log_dir_ + "/strace_" + std::to_string(getpid()) + ".log";
  std::string strace_cmd =
      "strace -e trace=openat,execve,execveat,creat -y -f -q -o " + strace_log +
      " " + command;

  Logger::debug("Executing: " + strace_cmd);

  // Execute the command, allowing original command output to go to stdout
  int exit_code = std::system(strace_cmd.c_str());
  if (exit_code != 0) {
    Logger::warn("Command exited with code: " + std::to_string(exit_code));
  }

  // Read strace output from the log file
  std::string strace_output;
  std::ifstream strace_file(strace_log);
  if (strace_file.is_open()) {
    std::string line;
    while (std::getline(strace_file, line)) {
      strace_output += line + "\n";
    }
    strace_file.close();

    // // Clean up the temporary log file
    // std::filesystem::remove(strace_log);
  } else {
    Logger::warn("Failed to read strace log file: " + strace_log);
  }

  return strace_output;
}

std::set<std::string> Tracker::parseLibFiles(const std::string& strace_output) {
  std::set<std::string> library_files;
  std::istringstream iss(strace_output);
  std::string line;

  // Match both .so (dynamic) and .a (static) libraries
  std::regex openat_regex(R"(openat\([^,]+,\s*\"([^\"]*\.(?:so|a)[^\"]*)\")");
  std::smatch match;

  while (std::getline(iss, line)) {
    if (std::regex_search(line, match, openat_regex)) {
      std::string filepath = match[1].str();

      if (!std::filesystem::exists(filepath) || shouldIgnoreLib(filepath)) {
        continue;
      }

      // Determine library type for better logging
      bool is_static = Utils::endsWith(filepath, ".a");
      bool is_dynamic = Utils::isSharedLib(filepath);

      if (!is_static && !is_dynamic) {
        continue;  // Not a recognized library type
      }

      if (is_static) {
        Logger::debug("Found static library: " + filepath);
      } else if (is_dynamic) {
        Logger::debug("Found shared library: " + filepath);
      }

      library_files.insert(filepath);
    }
  }

  return library_files;
}

std::set<std::string> Tracker::parseHeaderFiles(
    const std::string& strace_output) {
  std::set<std::string> header_files;
  std::istringstream iss(strace_output);
  std::string line;

  // Match header files (.h, .hpp, .hxx, .hh, .H)
  std::regex openat_regex(
      R"(openat\([^,]+,\s*\"([^\"]*\.(?:h|hpp|hxx|hh|H)[^\"]*)\")");
  std::smatch match;

  while (std::getline(iss, line)) {
    if (std::regex_search(line, match, openat_regex)) {
      std::string filepath = match[1].str();

      if (!std::filesystem::exists(filepath) || shouldIgnoreHeader(filepath)) {
        continue;
      }

      Logger::debug("Found header file: " + filepath);
      header_files.insert(filepath);
    }
  }

  return header_files;
}

std::set<std::string> Tracker::parseExecutables(
    const std::string& strace_output) {
  std::set<std::string> executables;
  std::istringstream iss(strace_output);
  std::string line;

  // Match execve and execveat syscalls in strace format: PID  execve("path",
  // ...)
  std::regex exec_regex(R"(\d+\s+(?:execve|execveat)\(\"([^\"]+)\")");
  std::smatch match;

  while (std::getline(iss, line)) {
    if (std::regex_search(line, match, exec_regex)) {
      std::string exec_path = match[1].str();

      // Skip if path doesn't exist or should be ignored
      if (!std::filesystem::exists(exec_path) ||
          shouldIgnoreExecutable(exec_path)) {
        continue;
      }

      Logger::debug("Found executable: " + exec_path);
      executables.insert(exec_path);
    }
  }

  return executables;
}

bool Tracker::shouldIgnoreFile(const std::string& filepath) const {
  for (const auto& pattern : ignore_patterns_) {
    if (Utils::contains(filepath, pattern)) {
      return true;
    }
  }

  return false;
}

bool Tracker::shouldIgnoreLib(const std::string& filepath) const {
  static const std::set<std::string> ignore_libs = {
      "/etc/ld.so.cache", "/lib64/ld-linux-x86-64.so.2"};

  if (ignore_libs.find(filepath) != ignore_libs.end()) {
    return true;
  }

  if (Utils::contains(filepath, build_info_->build_path_)) {
    // Ignore libraries in the build directory
    return true;
  }

  return shouldIgnoreFile(filepath);
}

bool Tracker::shouldIgnoreHeader(const std::string& filepath) const {
  if (filepath[0] != '/') {
    // Ignore relative paths
    return true;
  }

  if (Utils::contains(filepath, build_info_->build_path_)) {
    // Ignore headers in the build directory
    return true;
  }
  return shouldIgnoreFile(filepath);
}

bool Tracker::shouldIgnoreExecutable(const std::string& filepath) const {
  // Ignore common shell and system utilities that are not build dependencies
  static const std::set<std::string> ignore_execs = {
      "/bin/sh",      "/bin/bash",      "/bin/dash",        "/bin/zsh",
      "/usr/bin/env", "/usr/bin/which", "/usr/bin/dirname", "/usr/bin/basename",
      "/bin/echo",    "/bin/cat",       "/bin/grep",        "/bin/sed",
      "/bin/awk",     "/bin/ls",        "/bin/cp",          "/bin/mv",
      "/bin/rm",      "/bin/mkdir",     "/usr/bin/test",    "/usr/bin/[",
      "/bin/true",    "/bin/false"};

  if (filepath[0] != '/') {
    // Ignore relative paths
    return true;
  }

  if (Utils::contains(filepath, build_info_->build_path_)) {
    // Ignore executables in the build directory
    return true;
  }

  if (ignore_execs.find(filepath) != ignore_execs.end()) {
    return true;
  }

  // Use the same ignore patterns as for shared libraries
  for (const auto& pattern : ignore_patterns_) {
    if (Utils::contains(filepath, pattern)) {
      return true;
    }
  }

  return false;
}

bool Tracker::shouldIgnoreArtifact(const std::string& filepath) const {
  // Ignore CMake temporary files and directories
  if (Utils::contains(filepath, "CMakeFiles/")) {
    return true;
  }

  // Ignore CMake cache and configuration files
  if (Utils::contains(filepath, "CMakeCache.txt") ||
      Utils::contains(filepath, "cmake_install.cmake") ||
      Utils::contains(filepath, "Makefile")) {
    return true;
  }

  // Ignore object files and temporary files
  if (Utils::endsWith(filepath, ".o")) {
    return true;
  }

  // Ignore temporary and intermediate files
  if (Utils::contains(filepath, ".tmp") || Utils::contains(filepath, ".temp")) {
    return true;
  }

  // Use base ignore patterns
  return shouldIgnoreFile(filepath);
}

void Tracker::trackBuild() {
  Logger::info("Build command: " + build_info_->build_command_);
  auto start_time = std::chrono::high_resolution_clock::now();
  std::string strace_output;
  try {
    strace_output = executeWithStrace(build_info_->build_command_);
  } catch (const std::exception& e) {
    Logger::error("Error executing build command: " + std::string(e.what()));
    return;
  }
  auto build_end_time = std::chrono::high_resolution_clock::now();

  auto library_files = parseLibFiles(strace_output);
  auto header_files = parseHeaderFiles(strace_output);
  auto executables = parseExecutables(strace_output);
  Logger::info("Found " + std::to_string(library_files.size()) + " libraries");
  Logger::info("Found " + std::to_string(header_files.size()) +
               " header files");
  Logger::info("Found " + std::to_string(executables.size()) + " executables");

  BuildRecord& record = build_info_->build_record_;

  // Process all files concurrently using thread pool
  const size_t max_threads =
      std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
  ThreadPool pool(max_threads);
  std::mutex record_mutex;  // Protect record operations
  std::vector<std::future<void>> futures;

  auto process_file = [&](const std::string& file_path) {
    try {
      Logger::debug("Processing file: " + file_path);

      DependencyPackage dep =
          DependencyPackage::fromRawFile(file_path, build_info_->package_mgr_);
      if (dep.isValid()) {
        {
          std::lock_guard<std::mutex> lock(record_mutex);
          record.addDependency(dep);
        }
        Logger::debug("  Added: " + dep.getPackageName() + " v" +
                      dep.getVersion());
      } else {
        Logger::debug("  Skipped invalid dependency: " + file_path);
      }
    } catch (const std::exception& e) {
      Logger::warn("Error processing file " + file_path + ": " + e.what());
    }
  };

  for (const auto& library_file : library_files) {
    futures.emplace_back(pool.enqueue(process_file, library_file));
  }

  for (const auto& header_file : header_files) {
    futures.emplace_back(pool.enqueue(process_file, header_file));
  }

  for (const auto& executable : executables) {
    futures.emplace_back(pool.enqueue(process_file, executable));
  }

  // Wait for all tasks to complete
  for (auto& future : futures) {
    future.get();
  }

  // Detect build artifacts from strace output
  detectBuildArtifacts(strace_output, record);

  auto end_time = std::chrono::high_resolution_clock::now();
  auto build_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            build_end_time - start_time)
                            .count();
  auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            end_time - start_time)
                            .count();

  Logger::info("Build execution time: " + std::to_string(build_duration) +
               " ms");
  Logger::info("Total tracking time: " + std::to_string(total_duration) +
               " ms");
}

void Tracker::detectBuildArtifacts(const std::string& strace_output,
                                   BuildRecord& record) {
  Logger::debug("Detecting build artifacts from strace output");

  std::istringstream iss(strace_output);
  std::string line;
  std::set<std::string> created_files;

  // With -y option, AT_FDCWD and other paths are resolved, so we can match
  // paths directly Pattern matches: PID syscall(AT_FDCWD</path/to/dir>,
  // "filename", ..., O_CREAT)
  std::regex create_regex(
      R"(\d+\s+(?:openat|creat)\([^,]*<([^>]+)>,\s*\"([^\"]+)\"[^)]*O_CREAT)");
  // Also match simple creat calls: PID creat("filepath", ...)
  std::regex creat_regex(R"(\d+\s+creat\(\"([^\"]+)\")");
  // Match openat with absolute paths directly: openat(..., "/absolute/path",
  // ..., O_CREAT)
  std::regex absolute_create_regex(
      R"(\d+\s+openat\([^,]*,\s*\"(/[^\"]+)\"[^)]*O_CREAT)");
  std::smatch match;

  while (std::getline(iss, line)) {
    std::string filepath;

    // Check for openat with AT_FDCWD resolved by -y option
    if (std::regex_search(line, match, create_regex)) {
      std::string base_dir = match[1].str();
      std::string filename = match[2].str();
      filepath = base_dir + "/" + filename;
    }
    // Check for openat with absolute paths
    else if (std::regex_search(line, match, absolute_create_regex)) {
      filepath = match[1].str();
    }
    // Check for simple creat syscall
    else if (std::regex_search(line, match, creat_regex)) {
      filepath = match[1].str();
    }

    if (!filepath.empty() && std::filesystem::exists(filepath)) {
      Logger::debug("Found created file: " + filepath);
      created_files.insert(filepath);
    }
  }

  // Process created files and check if they are executables or shared libraries
  for (const auto& filepath : created_files) {
    try {
      if (!shouldIgnoreArtifact(filepath)) {
        bool is_executable = false;
        bool is_shared_lib = false;

        // Check file permissions for executables
        auto perms = std::filesystem::status(filepath).permissions();
        if ((perms & std::filesystem::perms::owner_exec) !=
            std::filesystem::perms::none) {
          is_executable = true;
        }

        // Check extension for shared libraries
        is_shared_lib = Utils::isSharedLib(filepath);

        Logger::debug("Created file " + filepath +
                      (is_executable ? " is executable." : "") +
                      (is_shared_lib ? " is shared library." : ""));

        if (is_executable || is_shared_lib) {
          std::string hash = Utils::calculateFileHash(filepath);
          std::string display_path = makeRelativePath(filepath, ".");
          std::string type = is_shared_lib ? "shared_library" : "executable";

          BuildArtifact artifact(display_path, hash, type);
          record.addArtifact(artifact);

          Logger::debug("Added artifact: " + display_path + " (" + type + ")");
        }
      }
    } catch (const std::exception& e) {
      Logger::warn("Error processing created file " + filepath + ": " +
                   e.what());
    }
  }
}

std::string Tracker::makeRelativePath(const std::string& filepath,
                                      const std::string& base_dir) {
  try {
    std::filesystem::path file_path = std::filesystem::absolute(filepath);
    std::filesystem::path base_path = std::filesystem::absolute(base_dir);

    // Normalize the base path to remove trailing dots
    base_path = base_path.lexically_normal();

    // Use filesystem::relative to determine if file is under base directory
    std::error_code ec;
    std::filesystem::path rel_path =
        std::filesystem::relative(file_path, base_path, ec);

    if (!ec) {
      std::string rel_str = rel_path.string();
      // Check if the relative path doesn't start with ".." (meaning it's under
      // base dir)
      if (rel_str.size() < 2 || rel_str.substr(0, 2) != "..") {
        return rel_str;
      }
    }

    // File is outside base directory, return absolute path
    return file_path.string();
  } catch (const std::exception& e) {
    Logger::warn("Error making relative path for " + filepath + ": " +
                 e.what());
    return filepath;
  }
}
