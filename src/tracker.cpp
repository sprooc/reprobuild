#include "tracker.h"

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "bpftrace_script.h"
#include "build_graph.h"
#include "interceptor_embedded.h"
#include "logger.h"
#include "thread_pool.h"
#include "utils.h"

namespace {
// Bpftrace configuration
const char* const kPidPlaceholder = "$pid = *;";
const char* const kBpftraceCommand = "sudo bpftrace0.24";

// Timing constants (milliseconds)
const int kBpftraceAttachCheckInterval = 100;
const int kBpftraceProcessingDelay = 200;
const int kBpftraceFlushDelay = 500;
const int kBpftraceAttachTimeout = 10000;  // 10 seconds

using Clock = std::chrono::steady_clock;

long long elapsedMs(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
      .count();
}

std::set<std::string> mergeDependencyFiles(
    const std::set<std::string>& library_files,
    const std::set<std::string>& header_files,
    const std::set<std::string>& executables) {
  std::set<std::string> dependency_files;
  dependency_files.insert(library_files.begin(), library_files.end());
  dependency_files.insert(header_files.begin(), header_files.end());
  dependency_files.insert(executables.begin(), executables.end());
  return dependency_files;
}

struct PathMapping {
  std::string observed_prefix;
  std::string local_prefix;
};

std::string normalizePath(const std::string& path) {
  return std::filesystem::path(path).lexically_normal().string();
}

bool hasPathPrefix(const std::string& path, const std::string& prefix) {
  if (path == prefix) {
    return true;
  }
  if (path.size() <= prefix.size()) {
    return false;
  }
  if (path.compare(0, prefix.size(), prefix) != 0) {
    return false;
  }
  return path[prefix.size()] == '/';
}

bool startsWithView(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() &&
         text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWithView(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string_view nextToken(std::string_view& text) {
  size_t pos = 0;
  while (pos < text.size() &&
         std::isspace(static_cast<unsigned char>(text[pos]))) {
    ++pos;
  }
  text.remove_prefix(pos);

  const size_t token_end = text.find_first_of(" \t\r\n\f\v");
  if (token_end == std::string_view::npos) {
    std::string_view token = text;
    text = {};
    return token;
  }

  std::string_view token = text.substr(0, token_end);
  text.remove_prefix(token_end);
  return token;
}

std::string_view filenameView(std::string_view path) {
  const size_t slash = path.find_last_of("/\\");
  if (slash == std::string_view::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

std::string_view extensionView(std::string_view path) {
  const std::string_view filename = filenameView(path);
  const size_t dot = filename.rfind('.');
  if (dot == std::string_view::npos || dot == 0) {
    return {};
  }
  return filename.substr(dot);
}

bool isSharedLibPath(std::string_view path) {
  if (endsWithView(path, ".so")) {
    return true;
  }

  const size_t so_pos = path.rfind(".so.");
  if (so_pos == std::string_view::npos) {
    return false;
  }

  for (char c : path.substr(so_pos + 4)) {
    if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') {
      return false;
    }
  }
  return true;
}

bool isInputExtension(std::string_view ext) {
  static constexpr std::array<std::string_view, 11> kInputExts = {
      ".c", ".cpp", ".cc", ".cxx", ".C", ".s",
      ".S", ".o",   ".lo", ".a",   ".la"};
  return std::find(kInputExts.begin(), kInputExts.end(), ext) !=
         kInputExts.end();
}

std::vector<PathMapping> loadPathMappings() {
  const char* raw_env = std::getenv("REPROBUILD_PATH_MAP");
  if (!raw_env || raw_env[0] == '\0') {
    return {};
  }

  std::string env_value(raw_env);
  std::replace(env_value.begin(), env_value.end(), '\n', ';');

  std::vector<PathMapping> mappings;
  std::stringstream ss(env_value);
  std::string entry;
  while (std::getline(ss, entry, ';')) {
    if (entry.empty()) {
      continue;
    }

    const size_t eq_pos = entry.find('=');
    if (eq_pos == std::string::npos || eq_pos == 0 ||
        eq_pos == entry.size() - 1) {
      continue;
    }

    PathMapping mapping;
    mapping.observed_prefix = normalizePath(entry.substr(0, eq_pos));
    mapping.local_prefix = normalizePath(entry.substr(eq_pos + 1));
    if (mapping.observed_prefix.empty() || mapping.local_prefix.empty()) {
      continue;
    }
    mappings.push_back(std::move(mapping));
  }

  std::sort(mappings.begin(), mappings.end(),
            [](const PathMapping& lhs, const PathMapping& rhs) {
              return lhs.observed_prefix.size() > rhs.observed_prefix.size();
            });
  return mappings;
}

std::string remapObservedPath(const std::string& original_path) {
  if (original_path.empty()) {
    return original_path;
  }

  const std::string normalized = normalizePath(original_path);
  static const std::vector<PathMapping> mappings = loadPathMappings();
  for (auto mapping : mappings) {
    Logger::debug("Loaded path mapping: " + mapping.observed_prefix + " -> " +
                 mapping.local_prefix);
  }
  for (const auto& mapping : mappings) {
    if (!hasPathPrefix(normalized, mapping.observed_prefix)) {
      continue;
    }

    if (normalized == mapping.observed_prefix) {
      Logger::debug("Exact match for path mapping: " + normalized + " -> " +
                   mapping.local_prefix);
      return mapping.local_prefix;
    }

    const std::string suffix =
        normalized.substr(mapping.observed_prefix.size());
    return normalizePath(mapping.local_prefix + suffix);
  }
  return normalized;
}

}  // namespace

Tracker::Tracker(std::shared_ptr<BuildInfo> build_info)
    : build_info_(build_info) {
  // Initialize default ignore patterns for system directories
  ignore_patterns_.push_back("/tmp/");
  ignore_patterns_.push_back("/proc/");
  ignore_patterns_.push_back("/sys/");
  ignore_patterns_.push_back("/dev/");
  ignore_patterns_.push_back("libreprobuild_interceptor.so");
}

void Tracker::addIgnorePattern(const std::string& pattern) {
  ignore_patterns_.push_back(pattern);
}

const TrackingTiming& Tracker::getTiming() const { return timing_; }

std::string Tracker::executeWithBpftrace(const std::string& command) {
  const auto preprocessing_start = Clock::now();
  const pid_t current_pid = getpid();
  const std::string pid_str = std::to_string(current_pid);

  // Generate file paths for bpftrace artifacts
  const std::string bpftrace_script =
      build_info_->log_dir_ + "/script_" + pid_str + ".bt";
  const std::string bpftrace_log =
      build_info_->log_dir_ + "/bpftrace_" + pid_str + ".log";
  const std::string bpftrace_stderr_log =
      build_info_->log_dir_ + "/bpftrace_stderr_" + pid_str + ".log";
  const std::string bpftrace_pidfile =
      build_info_->log_dir_ + "/bpftrace_pid_" + pid_str + ".pid";

  // Prepare bpftrace script with current PID
  std::string script_content = BpftraceScript::SCRIPT_TEMPLATE;
  const std::string pid_replacement = "$pid = " + pid_str + ";";

  const size_t placeholder_pos = script_content.find(kPidPlaceholder);
  if (placeholder_pos != std::string::npos) {
    script_content.replace(placeholder_pos, strlen(kPidPlaceholder),
                           pid_replacement);
  } else {
    Logger::warn("Could not find PID placeholder in bpftrace script");
  }

  // Write the modified script to temporary file
  {
    std::ofstream script_file(bpftrace_script);
    if (!script_file.is_open()) {
      Logger::error("Failed to create temporary bpftrace script: " +
                    bpftrace_script);
      return "";
    }
    script_file << script_content;
  }  // File auto-closed by RAII

  // Start bpftrace in background and capture its PID
  const std::string bpftrace_cmd = std::string(kBpftraceCommand) + " " +
                                   bpftrace_script + " > " + bpftrace_log +
                                   " 2> " + bpftrace_stderr_log +
                                   " & echo $! > " + bpftrace_pidfile;

  Logger::debug("Starting bpftrace: " + bpftrace_cmd);
  const int bpftrace_ret = std::system(bpftrace_cmd.c_str());
  if (bpftrace_ret != 0) {
    Logger::warn("Failed to start bpftrace (exit code: " +
                 std::to_string(bpftrace_ret) + ")");
  }

  // Wait for bpftrace to attach by monitoring stderr output
  bool attached = false;
  const auto start_wait = std::chrono::steady_clock::now();
  const auto timeout = std::chrono::milliseconds(kBpftraceAttachTimeout);

  while (!attached) {
    std::ifstream stderr_file(bpftrace_stderr_log);
    if (stderr_file.is_open()) {
      std::string line;
      while (std::getline(stderr_file, line)) {
        if (line.find("Attached") != std::string::npos) {
          Logger::debug("Bpftrace attached: " + line);
          attached = true;
          break;
        }
      }
    }

    if (!attached) {
      const auto elapsed = std::chrono::steady_clock::now() - start_wait;
      if (elapsed > timeout) {
        Logger::warn("Timeout waiting for bpftrace to attach");
        break;
      }
      std::this_thread::sleep_for(
          std::chrono::milliseconds(kBpftraceAttachCheckInterval));
    }
  }

  // Execute the actual build command
  const auto build_start = Clock::now();
  timing_.preprocessing_ms += elapsedMs(preprocessing_start, build_start);

  Logger::debug("Executing: " + command);
  const int exit_code = std::system(command.c_str());
  const auto build_end = Clock::now();
  timing_.build_execution_ms += elapsedMs(build_start, build_end);

  if (exit_code != 0) {
    Logger::warn("Command exited with code: " + std::to_string(exit_code));
  }

  const auto postprocessing_start = build_end;

  // Give bpftrace time to finish processing
  std::this_thread::sleep_for(
      std::chrono::milliseconds(kBpftraceProcessingDelay));

  // Stop bpftrace by reading its PID and sending SIGINT
  {
    std::ifstream pidfile(bpftrace_pidfile);
    if (pidfile.is_open()) {
      std::string bpftrace_pid_str;
      std::getline(pidfile, bpftrace_pid_str);

      if (!bpftrace_pid_str.empty()) {
        try {
          const pid_t bpftrace_pid = std::stoi(bpftrace_pid_str);
          Logger::debug("Stopping bpftrace PID " + bpftrace_pid_str);

          // Use kill() syscall directly to avoid creating traced processes
          if (kill(bpftrace_pid, SIGINT) == 0) {
            Logger::debug("Successfully sent SIGINT to bpftrace");
          } else {
            Logger::warn("Failed to send SIGINT to bpftrace: " +
                         std::string(std::strerror(errno)));
          }

          // Clean up PID file
          std::filesystem::remove(bpftrace_pidfile);
        } catch (const std::exception& e) {
          Logger::warn("Failed to parse bpftrace PID: " +
                       std::string(e.what()));
        }
      }
    } else {
      Logger::warn("Failed to read bpftrace PID file: " + bpftrace_pidfile);
    }
  }  // File auto-closed by RAII

  // Wait for bpftrace to flush output
  std::this_thread::sleep_for(std::chrono::milliseconds(kBpftraceFlushDelay));

  // Read bpftrace output from log file
  std::string raw_output;
  {
    std::ifstream bpftrace_file(bpftrace_log);
    if (bpftrace_file.is_open()) {
      std::string line;
      while (std::getline(bpftrace_file, line)) {
        raw_output += line + "\n";
      }
      // Note: Temporary files cleanup is disabled for debugging
      // std::filesystem::remove(bpftrace_script);
      // std::filesystem::remove(bpftrace_log);
    } else {
      Logger::warn("Failed to read bpftrace log file: " + bpftrace_log);
    }
  }

  std::string processed_output = processBpftraceOutput(raw_output);
  timing_.bpftrace_finalization_ms +=
      elapsedMs(postprocessing_start, Clock::now());
  timing_.postprocessing_ms += timing_.bpftrace_finalization_ms;
  return processed_output;
}

std::string Tracker::processBpftraceOutput(const std::string& raw_output) {
  // Convert bpftrace raw output to c1 format
  // Input:  PID|content|PID|content|...
  // Output: ID PID: \n content \n
  std::unordered_map<int, std::string> pid_streams;

  size_t pos = 0;
  while (pos < raw_output.size()) {
    // Skip leading whitespace
    while (pos < raw_output.size() && std::isspace(raw_output[pos])) {
      ++pos;
    }

    // Parse PID (sequence of digits)
    const size_t pid_start = pos;
    while (pos < raw_output.size() && std::isdigit(raw_output[pos])) {
      ++pos;
    }

    if (pos >= raw_output.size() || raw_output[pos] != (char)0x80) {
      break;  // Invalid format or end of data
    }

    int pid = std::stoi(raw_output.substr(pid_start, pos - pid_start));
    ++pos;  // Skip delimiter 0x80

    // Parse content until next delimiter
    const size_t content_start = pos;
    while (pos < raw_output.size() && raw_output[pos] != (char)0x80) {
      ++pos;
    }

    if (pos >= raw_output.size()) {
      break;  // Missing closing delimiter
    }

    const std::string content =
        raw_output.substr(content_start, pos - content_start);
    pid_streams[pid] += content;
    ++pos;  // Skip delimiter 0x80
  }

  // Format output in c1 format: group by PID
  std::ostringstream result;
  for (const auto& [pid, content] : pid_streams) {
    result << "ID " << pid << ": \n" << content << "\n";
  }

  return result.str();
}

std::set<std::string> Tracker::parseLibFiles(
    const std::string& bpftrace_output) {
  std::set<std::string> library_files;
  std::istringstream input_stream(bpftrace_output);
  std::string line;

  while (std::getline(input_stream, line)) {
    // Skip ID header lines and empty lines
    if (line.empty() || Utils::startsWith(line, "ID ")) {
      continue;
    }

    // Match openat syscall lines: "openat /path/to/file flags"
    if (!Utils::startsWith(line, "openat ")) {
      continue;
    }

    std::istringstream line_stream(line);
    std::string syscall, filepath;
    line_stream >> syscall >> filepath;
    filepath = remapObservedPath(filepath);

    if (filepath.empty()) {
      continue;
    }

    // Check if file is a library (.so or .a)
    const bool is_static_lib = Utils::endsWith(filepath, ".a");
    const bool is_dynamic_lib = Utils::isSharedLib(filepath);

    if (!is_static_lib && !is_dynamic_lib) {
      continue;
    }

    if (!std::filesystem::exists(filepath) || shouldIgnoreLib(filepath)) {
      continue;
    }

    const char* lib_type = is_static_lib ? "static" : "shared";
    Logger::debug(std::string("Found ") + lib_type + " library: " + filepath);
    library_files.insert(filepath);
  }

  return library_files;
}

std::set<std::string> Tracker::parseHeaderFiles(
    const std::string& bpftrace_output) {
  // Recognized header file extensions
  static const std::vector<std::string> kHeaderExtensions = {
      ".h", ".hpp", ".hxx", ".hh", ".H"};

  std::set<std::string> header_files;
  std::istringstream input_stream(bpftrace_output);
  std::string line;

  while (std::getline(input_stream, line)) {
    // Skip ID header lines and empty lines
    if (line.empty() || Utils::startsWith(line, "ID ")) {
      continue;
    }

    // Match openat syscall lines: "openat /path/to/file flags"
    if (!Utils::startsWith(line, "openat ")) {
      continue;
    }

    std::istringstream line_stream(line);
    std::string syscall, filepath;
    line_stream >> syscall >> filepath;
    filepath = remapObservedPath(filepath);

    if (filepath.empty()) {
      continue;
    }

    // Check if file has a header extension
    bool is_header_file = false;
    for (const auto& ext : kHeaderExtensions) {
      if (Utils::endsWith(filepath, ext)) {
        is_header_file = true;
        break;
      }
    }

    if (!is_header_file) {
      continue;
    }

    if (!std::filesystem::exists(filepath) || shouldIgnoreHeader(filepath)) {
      continue;
    }

    Logger::debug("Found header file: " + filepath);
    header_files.insert(filepath);
  }

  return header_files;
}

std::set<std::string> Tracker::parseExecutables(
    const std::string& bpftrace_output) {
  std::set<std::string> executables;
  std::istringstream input_stream(bpftrace_output);
  std::string line;

  while (std::getline(input_stream, line)) {
    // Skip ID header lines and empty lines
    if (line.empty() || Utils::startsWith(line, "ID ")) {
      continue;
    }

    // Match execve/execveat syscalls: "execve /path/to/executable arg1 ..."
    const bool is_execve = Utils::startsWith(line, "execve ");
    const bool is_execveat = Utils::startsWith(line, "execveat ");

    if (!is_execve && !is_execveat) {
      continue;
    }

    std::istringstream line_stream(line);
    std::string syscall, exec_path;
    line_stream >> syscall >> exec_path;
    exec_path = remapObservedPath(exec_path);

    if (exec_path.empty()) {
      continue;
    }

    // Skip if path doesn't exist or should be ignored
    if (!std::filesystem::exists(exec_path) ||
        shouldIgnoreExecutable(exec_path)) {
      continue;
    }

    Logger::debug("Found executable: " + exec_path);
    executables.insert(exec_path);
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
  timing_ = TrackingTiming{};
  Logger::info("Build command: " + build_info_->build_command_);
  const auto tracking_start = Clock::now();
  std::string bpftrace_output;
  try {
    bpftrace_output = executeWithBpftrace(build_info_->build_command_);
  } catch (const std::exception& e) {
    Logger::error("Error executing build command: " + std::string(e.what()));
    return;
  }
  const auto analysis_start = Clock::now();

  // Write raw bpftrace output to a file for debugging
  const std::string raw_output_path = build_info_->log_dir_ +
                                      "/bpftrace_raw_output_" +
                                      std::to_string(getpid()) + ".log";
  const auto raw_output_write_start = Clock::now();
  {
    std::ofstream raw_output_file(raw_output_path);
    if (raw_output_file.is_open()) {
      raw_output_file << bpftrace_output;
    }
  }
  timing_.raw_output_write_ms +=
      elapsedMs(raw_output_write_start, Clock::now());

  const auto dependency_file_parse_start = Clock::now();
  auto library_files = parseLibFiles(bpftrace_output);
  auto header_files = parseHeaderFiles(bpftrace_output);
  auto executables = parseExecutables(bpftrace_output);
  timing_.dependency_file_parse_ms +=
      elapsedMs(dependency_file_parse_start, Clock::now());
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

  auto dependency_files =
      mergeDependencyFiles(library_files, header_files, executables);
  Logger::info("Processing " + std::to_string(dependency_files.size()) +
               " unique dependency files");

  const auto dependency_resolution_start = Clock::now();
  for (const auto& dependency_file : dependency_files) {
    futures.emplace_back(pool.enqueue(process_file, dependency_file));
  }

  // Wait for all tasks to complete
  for (auto& future : futures) {
    future.get();
  }
  timing_.dependency_resolution_ms +=
      elapsedMs(dependency_resolution_start, Clock::now());

  // Detect build artifacts from bpftrace output
  const auto artifact_detection_start = Clock::now();
  detectBuildArtifacts(bpftrace_output, record);
  timing_.artifact_detection_ms +=
      elapsedMs(artifact_detection_start, Clock::now());

  // Build and save the topology graph
  try {
    if (!build_info_->graph_output_file_.empty()) {
      const auto graph_start = Clock::now();
      build_info_->build_graph_ = parseBuildGraph(bpftrace_output);
      timing_.graph_parse_ms += elapsedMs(graph_start, Clock::now());
      // Prune to only edges reachable from detected artifacts.
      // Use basenames only to avoid path-form mismatches between what
      // the linker passed to -o and what detectBuildArtifacts recorded.
      const auto graph_prune_start = Clock::now();
      std::unordered_set<std::string> roots;
      for (const auto& a : record.getArtifacts()) {
        roots.insert(std::filesystem::path(a.path).filename().string());
      }
      build_info_->build_graph_.pruneGraph(roots);
      timing_.graph_prune_ms += elapsedMs(graph_prune_start, Clock::now());
      timing_.graph_total_ms += elapsedMs(graph_start, Clock::now());
    }
  } catch (const std::exception& e) {
    Logger::warn("Failed to save build graph: " + std::string(e.what()));
  }

  const auto analysis_end = Clock::now();
  timing_.postprocessing_ms += elapsedMs(analysis_start, analysis_end);
  timing_.total_ms = elapsedMs(tracking_start, analysis_end);

  Logger::debug("Tracker preprocessing time: " +
                std::to_string(timing_.preprocessing_ms) + " ms");
  Logger::debug("Tracker build execution time: " +
                std::to_string(timing_.build_execution_ms) + " ms");
  Logger::debug("Tracker postprocessing time: " +
                std::to_string(timing_.postprocessing_ms) + " ms");
  Logger::debug("Tracker total time: " + std::to_string(timing_.total_ms) +
                " ms");
  Logger::info("Tracker postprocessing detail: bpftrace_finalization=" +
               std::to_string(timing_.bpftrace_finalization_ms) +
               " ms, raw_output_write=" +
               std::to_string(timing_.raw_output_write_ms) +
               " ms, dependency_file_parse=" +
               std::to_string(timing_.dependency_file_parse_ms) +
               " ms, dependency_resolution=" +
               std::to_string(timing_.dependency_resolution_ms) +
               " ms, artifact_detection=" +
               std::to_string(timing_.artifact_detection_ms) +
               " ms, graph_parse=" + std::to_string(timing_.graph_parse_ms) +
               " ms, graph_prune=" + std::to_string(timing_.graph_prune_ms) +
               " ms");
}

void Tracker::detectBuildArtifacts(const std::string& bpftrace_output,
                                   BuildRecord& record) {
  Logger::debug("Detecting build artifacts from bpftrace output");

  std::istringstream input_stream(bpftrace_output);
  std::string line;
  std::set<std::string> created_files;

  while (std::getline(input_stream, line)) {
    // Skip ID header lines and empty lines
    if (line.empty() || Utils::startsWith(line, "ID ")) {
      continue;
    }

    std::string filepath;

    // Match creat syscall: "creat /path/to/file"
    if (Utils::startsWith(line, "creat ")) {
      std::istringstream line_stream(line);
      std::string syscall;
      line_stream >> syscall >> filepath;
    }
    // Match openat with O_CREAT flag: "openat /path/to/file flags"
    else if (Utils::startsWith(line, "openat ")) {
      std::istringstream line_stream(line);
      std::string syscall, path;
      int flags;
      line_stream >> syscall >> path >> flags;

      // Check if O_CREAT flag is set (64 = 0100 octal)
      if ((flags & 64) != 0) {
        filepath = path;
      }
    }

    filepath = remapObservedPath(filepath);
    if (!filepath.empty() && std::filesystem::exists(filepath)) {
      Logger::debug("Found created file: " + filepath);
      created_files.insert(filepath);
    }
  }

  // Process created files and identify build artifacts
  processCreatedFiles(created_files, record);
}

void Tracker::processCreatedFiles(const std::set<std::string>& created_files,
                                  BuildRecord& record) {
  for (const auto& filepath : created_files) {
    try {
      if (shouldIgnoreArtifact(filepath)) {
        continue;
      }

      const auto status = std::filesystem::status(filepath);
      if (!std::filesystem::is_regular_file(status)) {
        continue;
      }

      // Check file type
      const auto perms = status.permissions();
      const bool is_executable = (perms & std::filesystem::perms::owner_exec) !=
                                 std::filesystem::perms::none;
      const bool is_shared_lib = Utils::isSharedLib(filepath);
      const bool is_static_lib = Utils::isStaticLib(filepath);

      if (!is_executable && !is_shared_lib && !is_static_lib) {
        continue;
      }

      Logger::debug("Created file " + filepath +
                    (is_executable ? " is executable." : "") +
                    (is_shared_lib ? " is shared library." : "") +
                    (is_static_lib ? " is static library." : ""));

      // Add artifact to build record
      const std::string hash = Utils::calculateFileHash(filepath);
      const std::string display_path = makeRelativePath(filepath, ".");
      std::string artifact_type = "executable";
      if (is_shared_lib) {
        artifact_type = "shared_library";
      } else if (is_static_lib) {
        artifact_type = "static_library";
      }

      BuildArtifact artifact(display_path, hash, artifact_type);
      record.addArtifact(artifact);

      Logger::debug("Added artifact: " + display_path + " (" + artifact_type +
                    ")");
    } catch (const std::exception& e) {
      Logger::warn("Error processing created file " + filepath + ": " +
                   e.what());
    }
  }
}

BuildGraph Tracker::parseBuildGraph(const std::string& bpftrace_output) {
  // Longer tool names come first so suffix matching keeps ld.gold/clang++
  // distinct from ld/g++.
  static constexpr std::array<std::string_view, 15> kBuildTools = {
      "clang++", "clang", "ld.gold", "ld.lld", "ld.bfd",
      "libtool", "ranlib", "objcopy", "strip", "gcc",
      "g++",     "c++",   "cc",      "ar",     "ld"};

  auto is_build_tool = [&](std::string_view tool) -> bool {
    return std::find(kBuildTools.begin(), kBuildTools.end(), tool) !=
           kBuildTools.end();
  };

  // - strip numeric version suffixes ("gcc-12" -> "gcc")
  //   ("x86_64-linux-gnu-g++-14" -> "g++")
  auto normalize_tool = [&](std::string_view name) -> std::string_view {
    std::string_view normalized = name;

    const size_t dash = normalized.rfind('-');
    bool has_numeric_suffix = dash != std::string_view::npos &&
                              dash + 1 < normalized.size();
    for (size_t i = dash + 1; has_numeric_suffix && i < normalized.size();
         ++i) {
      has_numeric_suffix =
          std::isdigit(static_cast<unsigned char>(normalized[i])) != 0;
    }
    if (has_numeric_suffix) {
      normalized.remove_suffix(normalized.size() - dash);
    }

    if (is_build_tool(normalized)) {
      return normalized;
    }

    for (const auto& tool : kBuildTools) {
      if (endsWithView(normalized, tool)) {
        return tool;
      }
    }

    return normalized;
  };

  // Determine a node's type from its file extension / properties.
  auto classify = [&](const std::string& p, bool is_output) -> BuildNodeType {
    const std::string_view ext = extensionView(p);
    if (ext == ".o" || ext == ".lo") return BuildNodeType::INTERMEDIATE;
    if (ext == ".a" || ext == ".la") return BuildNodeType::INTERMEDIATE;
    if (ext == ".c" || ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
        ext == ".C" || ext == ".s" || ext == ".S") {
      return BuildNodeType::SOURCE;
    }
    if (isSharedLibPath(p)) {
      return is_output ? BuildNodeType::ARTIFACT : BuildNodeType::INTERMEDIATE;
    }
    return is_output ? BuildNodeType::ARTIFACT : BuildNodeType::UNKNOWN;
  };

  BuildGraph graph;
  std::unordered_set<std::string> seen_node_paths;

  // Lazily add (or update) a node; compute hash only once per path.
  auto ensure_node = [&](const std::string& path, bool is_output) {
    if (path.empty()) return;
    if (!seen_node_paths.insert(path).second) return;

    BuildNode node;
    node.path = path;
    node.type = classify(path, is_output);
    if (std::filesystem::exists(path)) {
      node.hash = Utils::calculateFileHash(path);
    }
    graph.addNode(std::move(node));
  };

  int current_pid = -1;

  auto process_exec = [&](std::string_view rest) {
    const std::string_view command_path_view = nextToken(rest);
    if (command_path_view.empty()) {
      return;
    }

    const std::string_view tool =
        normalize_tool(filenameView(command_path_view));
    if (!is_build_tool(tool)) {
      return;
    }

    std::string command_path(command_path_view);
    if (!std::filesystem::exists(command_path)) {
      return;
    }

    BuildEdge edge;
    edge.command.assign(tool.data(), tool.size());
    edge.command_path = std::move(command_path);
    edge.pid = current_pid;
    edge.args.reserve(16);

    while (true) {
      const std::string_view arg = nextToken(rest);
      if (arg.empty()) {
        break;
      }
      edge.args.emplace_back(arg);
    }

    // Parse inputs / output from the argument list.
    if (tool == "ar") {
      bool found_output = false;
      for (const auto& arg : edge.args) {
        if (arg.empty() || arg[0] == '-') continue;
        if (!found_output && !extensionView(arg).empty()) {
          edge.output = arg;
          found_output = true;
        } else if (found_output) {
          if (isInputExtension(extensionView(arg))) {
            edge.inputs.push_back(arg);
          }
        }
      }
    } else if (tool == "ranlib") {
      for (const auto& arg : edge.args) {
        if (!arg.empty() && arg[0] != '-') {
          edge.inputs.push_back(arg);
          break;
        }
      }
    } else {
      // gcc, g++, ld, clang, ...
      // Flags that consume the NEXT argument as a non-file parameter.
      static constexpr std::array<std::string_view, 17> kSkipArgFlags = {
          "-MT",
          "-MF",
          "-MQ",
          "-x",
          "-isystem",
          "-isysroot",
          "--sysroot",
          "-rpath",
          "-rpath-link",
          "-soname",
          "-Wl,-soname",
          "-plugin",
          "-plugin-opt",
          "--dynamic-linker",
          "-dumpbase",
          "-m",
          "--dependency-file",
      };
      auto is_skip_arg_flag = [&](const std::string& arg) -> bool {
        const std::string_view arg_view(arg.data(), arg.size());
        return std::find(kSkipArgFlags.begin(), kSkipArgFlags.end(),
                         arg_view) != kSkipArgFlags.end();
      };

      bool next_is_output = false;
      bool next_is_skip = false;
      for (const auto& arg : edge.args) {
        if (next_is_output) {
          edge.output = arg;
          next_is_output = false;
        } else if (next_is_skip) {
          next_is_skip = false;  // discard this value
        } else if (arg == "-o") {
          next_is_output = true;
        } else if (is_skip_arg_flag(arg)) {
          next_is_skip = true;
        } else if (!arg.empty() && arg[0] != '-') {
          if (isInputExtension(extensionView(arg)) || isSharedLibPath(arg)) {
            edge.inputs.push_back(arg);
          }
        }
      }
    }

    if (edge.output.empty() || edge.inputs.empty()) {
      return;
    }

    // Register nodes for every file referenced by this edge.
    for (const auto& inp : edge.inputs) {
      ensure_node(inp, /*is_output=*/false);
    }
    ensure_node(edge.output, /*is_output=*/true);

    graph.addEdge(std::move(edge));
  };

  size_t line_start = 0;
  while (line_start <= bpftrace_output.size()) {
    size_t line_end = bpftrace_output.find('\n', line_start);
    if (line_end == std::string::npos) {
      line_end = bpftrace_output.size();
    }
    std::string_view line(bpftrace_output.data() + line_start,
                          line_end - line_start);

    if (!line.empty()) {
      // "ID <PID>: " header -> update PID context for subsequent exec lines.
      if (startsWithView(line, "ID ")) {
        const char* first = line.data() + 3;
        const char* last = line.data() + line.size();
        int parsed_pid = -1;
        const auto result = std::from_chars(first, last, parsed_pid);
        if (result.ec == std::errc()) {
          current_pid = parsed_pid;
        } else {
          current_pid = -1;
        }
      } else if (startsWithView(line, "execve ")) {
        process_exec(line.substr(7));
      } else if (startsWithView(line, "execveat ")) {
        process_exec(line.substr(9));
      }
    }

    if (line_end == bpftrace_output.size()) {
      break;
    }
    line_start = line_end + 1;
  }

  Logger::debug("Build graph: " + std::to_string(graph.nodeCount()) +
                " nodes, " + std::to_string(graph.edgeCount()) + " edges");
  return graph;
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
