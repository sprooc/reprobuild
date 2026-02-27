#include "tracker.h"

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
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

}  // namespace

Tracker::Tracker(std::shared_ptr<BuildInfo> build_info)
    : build_info_(build_info) {
  // Initialize default ignore patterns for system directories
  ignore_patterns_.push_back("/tmp/");
  ignore_patterns_.push_back("/proc/");
  ignore_patterns_.push_back("/sys/");
  ignore_patterns_.push_back("/dev/");
}

void Tracker::addIgnorePattern(const std::string& pattern) {
  ignore_patterns_.push_back(pattern);
}

std::string Tracker::executeWithBpftrace(const std::string& command) {
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
  Logger::debug("Executing: " + command);
  const int exit_code = std::system(command.c_str());
  if (exit_code != 0) {
    Logger::warn("Command exited with code: " + std::to_string(exit_code));
  }

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

  return processBpftraceOutput(raw_output);
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

    if (pos >= raw_output.size() || raw_output[pos] != '|') {
      break;  // Invalid format or end of data
    }

    const int pid = std::stoi(raw_output.substr(pid_start, pos - pid_start));
    ++pos;  // Skip delimiter '|'

    // Parse content until next delimiter
    const size_t content_start = pos;
    while (pos < raw_output.size() && raw_output[pos] != '|') {
      ++pos;
    }

    if (pos >= raw_output.size()) {
      break;  // Missing closing delimiter
    }

    const std::string content =
        raw_output.substr(content_start, pos - content_start);
    pid_streams[pid] += content;

    ++pos;  // Skip delimiter '|'
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
  Logger::info("Build command: " + build_info_->build_command_);
  auto start_time = std::chrono::high_resolution_clock::now();
  std::string bpftrace_output;
  try {
    bpftrace_output = executeWithBpftrace(build_info_->build_command_);
  } catch (const std::exception& e) {
    Logger::error("Error executing build command: " + std::string(e.what()));
    return;
  }
  auto build_end_time = std::chrono::high_resolution_clock::now();

  // Write raw bpftrace output to a file for debugging
  const std::string raw_output_path = build_info_->log_dir_ +
                                      "/bpftrace_raw_output_" +
                                      std::to_string(getpid()) + ".log";
  {
    std::ofstream raw_output_file(raw_output_path);
    if (raw_output_file.is_open()) {
      raw_output_file << bpftrace_output;
    }
  }

  auto library_files = parseLibFiles(bpftrace_output);
  auto header_files = parseHeaderFiles(bpftrace_output);
  auto executables = parseExecutables(bpftrace_output);
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

  // Detect build artifacts from bpftrace output
  detectBuildArtifacts(bpftrace_output, record);

  // Build and save the topology graph
  try {
    if (!build_info_->graph_output_file_.empty()) {
      build_info_->build_graph_ = parseBuildGraph(bpftrace_output);
      // Prune to only edges reachable from detected artifacts.
      // Use basenames only to avoid path-form mismatches between what
      // the linker passed to -o and what detectBuildArtifacts recorded.
      std::unordered_set<std::string> roots;
      for (const auto& a : record.getArtifacts()) {
        roots.insert(std::filesystem::path(a.path).filename().string());
      }
      build_info_->build_graph_.pruneGraph(roots);
    }
  } catch (const std::exception& e) {
    Logger::warn("Failed to save build graph: " + std::string(e.what()));
  }

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

      // Check file type
      const auto perms = std::filesystem::status(filepath).permissions();
      const bool is_executable = (perms & std::filesystem::perms::owner_exec) !=
                                 std::filesystem::perms::none;
      const bool is_shared_lib = Utils::isSharedLib(filepath);

      if (!is_executable && !is_shared_lib) {
        continue;
      }

      Logger::debug("Created file " + filepath +
                    (is_executable ? " is executable." : "") +
                    (is_shared_lib ? " is shared library." : ""));

      // Add artifact to build record
      const std::string hash = Utils::calculateFileHash(filepath);
      const std::string display_path = makeRelativePath(filepath, ".");
      const std::string artifact_type =
          is_shared_lib ? "shared_library" : "executable";

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
  // Build tools whose invocations constitute edges in the graph
  static const std::unordered_set<std::string> kBuildTools = {
      "gcc",     "g++",    "cc",      "c++",    "clang",
      "clang++", "ar",     "ld",      "ld.bfd", "ld.gold",
      "ld.lld",  "ranlib", "objcopy", "strip",  "libtool",
  };

  // File extensions recognised as source / intermediate inputs
  static const std::unordered_set<std::string> kInputExts = {
      ".c", ".cpp", ".cc", ".cxx", ".C", ".s", ".S",  // sources
      ".o", ".lo",  ".a",  ".la",                     // intermediates
  };

  // - strip numeric version suffixes ("gcc-12" -> "gcc")
  //   ("x86_64-linux-gnu-g++-14" -> "g++")
  auto normalize_tool = [&](const std::string& name) -> std::string {
    std::string normalized = name;

    const size_t dash = normalized.rfind('-');
    if (dash != std::string::npos && dash + 1 < normalized.size() &&
        std::all_of(normalized.begin() + dash + 1, normalized.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; })) {
      normalized = normalized.substr(0, dash);
    }

    if (kBuildTools.find(normalized) != kBuildTools.end()) {
      return normalized;
    }

    for (const auto& tool : kBuildTools) {
      if (Utils::endsWith(normalized, tool)) {
        return tool;
      }
    }

    return normalized;
  };

  // Determine a node's type from its file extension / properties.
  auto classify = [&](const std::string& p, bool is_output) -> BuildNodeType {
    const std::string ext = std::filesystem::path(p).extension().string();
    if (ext == ".o" || ext == ".lo") return BuildNodeType::INTERMEDIATE;
    if (ext == ".a" || ext == ".la") return BuildNodeType::INTERMEDIATE;
    if (ext == ".c" || ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
        ext == ".C" || ext == ".s" || ext == ".S") {
      return BuildNodeType::SOURCE;
    }
    if (Utils::isSharedLib(p)) {
      return is_output ? BuildNodeType::ARTIFACT : BuildNodeType::INTERMEDIATE;
    }
    return is_output ? BuildNodeType::ARTIFACT : BuildNodeType::UNKNOWN;
  };

  BuildGraph graph;

  // Lazily add (or update) a node; compute hash only once per path.
  auto ensure_node = [&](const std::string& path, bool is_output) {
    if (path.empty()) return;
    if (graph.hasNode(path)) return;

    BuildNode node;
    node.path = path;
    node.type = classify(path, is_output);
    if (std::filesystem::exists(path)) {
      node.hash = Utils::calculateFileHash(path);
    }
    graph.addNode(node);
  };

  int current_pid = -1;
  std::string current_cmd_path;
  std::vector<std::string> current_args;

  auto flush = [&]() {
    if (current_cmd_path.empty() || !std::filesystem::exists(current_cmd_path))
      return;

    const std::string basename =
        std::filesystem::path(current_cmd_path).filename().string();
    const std::string tool = normalize_tool(basename);

    if (kBuildTools.find(tool) == kBuildTools.end()) {
      current_cmd_path.clear();
      current_args.clear();
      return;
    }

    BuildEdge edge;
    edge.command = tool;
    edge.command_path = current_cmd_path;
    edge.args = current_args;
    edge.pid = current_pid;

    // Parse inputs / output from the argument list.
    if (tool == "ar") {
      bool found_output = false;
      for (const auto& arg : current_args) {
        if (arg.empty() || arg[0] == '-') continue;
        if (!found_output && std::filesystem::path(arg).has_extension()) {
          edge.output = arg;
          found_output = true;
        } else if (found_output) {
          const std::string ext =
              std::filesystem::path(arg).extension().string();
          if (kInputExts.count(ext)) {
            edge.inputs.push_back(arg);
          }
        }
      }
    } else if (tool == "ranlib") {
      for (const auto& arg : current_args) {
        if (!arg.empty() && arg[0] != '-') {
          edge.inputs.push_back(arg);
          break;
        }
      }
    } else {
      // gcc, g++, ld, clang, ...
      // Flags that consume the NEXT argument as a non-file parameter.
      static const std::unordered_set<std::string> kSkipArgFlags = {
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

      bool next_is_output = false;
      bool next_is_skip = false;
      for (const auto& arg : current_args) {
        if (next_is_output) {
          edge.output = arg;
          next_is_output = false;
        } else if (next_is_skip) {
          next_is_skip = false;  // discard this value
        } else if (arg == "-o") {
          next_is_output = true;
        } else if (kSkipArgFlags.count(arg)) {
          next_is_skip = true;
        } else if (!arg.empty() && arg[0] != '-') {
          const std::string ext =
              std::filesystem::path(arg).extension().string();
          if (kInputExts.count(ext) || Utils::isSharedLib(arg)) {
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

    graph.addEdge(edge);

    current_cmd_path.clear();
    current_args.clear();
  };

  std::istringstream stream(bpftrace_output);
  std::string line;

  while (std::getline(stream, line)) {
    if (line.empty()) continue;

    // "ID <PID>: " header → flush the previous PID's command and start fresh.
    if (Utils::startsWith(line, "ID ")) {
      const std::string rest = line.substr(3);  // skip "ID "
      try {
        current_pid = std::stoi(rest);
      } catch (...) {
        current_pid = -1;
      }
      continue;
    }

    if (Utils::startsWith(line, "execve ") ||
        Utils::startsWith(line, "execveat ")) {
      std::istringstream ls(line);
      std::string syscall;
      ls >> syscall >> current_cmd_path;
      current_args.clear();
      std::string arg;
      while (ls >> arg) {
        current_args.push_back(arg);
      }
      flush();
      continue;
    }
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
