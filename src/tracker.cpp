#include "tracker.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <regex>
#include <sstream>

#include "logger.h"
#include "utils.h"

Tracker::Tracker() : Tracker("default_project") {}

Tracker::Tracker(const std::string& project_name)
    : build_timestamp_(Utils::getCurrentTimestamp()),
      project_name_(project_name),
      output_file_("build_record.yaml"),
      log_dir_("/tmp") {
  ignore_patterns_.push_back("/tmp/");
  ignore_patterns_.push_back("/proc/");
  ignore_patterns_.push_back("/sys/");
  ignore_patterns_.push_back("/dev/");
}

void Tracker::setOutputFile(const std::string& output_file) {
  output_file_ = output_file;
}

void Tracker::addIgnorePattern(const std::string& pattern) {
  ignore_patterns_.push_back(pattern);
}

void Tracker::setLogDirectory(const std::string& log_dir) {
  log_dir_ = log_dir;
}

const BuildRecord& Tracker::getLastBuildRecord() const {
  return last_build_record_;
}

std::string Tracker::executeWithStrace(
    const std::vector<std::string>& command) {
  std::string cmd_str = joinCommand(command);
  std::string strace_log =
      log_dir_ + "/strace_" + std::to_string(getpid()) + ".log";
  std::string strace_cmd =
      "strace -e trace=openat,execve,execveat,creat -y -f -q -o " + strace_log +
      " " + cmd_str;

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

std::set<std::string> Tracker::parseSoFiles(const std::string& strace_output) {
  std::set<std::string> so_files;
  std::istringstream iss(strace_output);
  std::string line;

  std::regex openat_regex(R"(openat\([^,]+,\s*\"([^\"]*\.so[^\"]*)\")");
  std::smatch match;

  while (std::getline(iss, line)) {
    if (std::regex_search(line, match, openat_regex)) {
      std::string filepath = match[1].str();

      if (!std::filesystem::exists(filepath) || shouldIgnoreFile(filepath)) {
        continue;
      }

      Logger::debug("Found shared library: " + filepath);
      so_files.insert(filepath);
    }
  }

  return so_files;
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
    if (filepath.find(pattern) != std::string::npos) {
      return true;
    }
  }

  return false;
}

bool Tracker::shouldIgnoreExecutable(const std::string& filepath) const {
  // Ignore common shell and system utilities that are not build dependencies
  static const std::vector<std::string> ignore_execs = {
      "/bin/sh",      "/bin/bash",      "/bin/dash",        "/bin/zsh",
      "/usr/bin/env", "/usr/bin/which", "/usr/bin/dirname", "/usr/bin/basename",
      "/bin/echo",    "/bin/cat",       "/bin/grep",        "/bin/sed",
      "/bin/awk",     "/bin/ls",        "/bin/cp",          "/bin/mv",
      "/bin/rm",      "/bin/mkdir",     "/usr/bin/test",    "/usr/bin/[",
      "/bin/true",    "/bin/false"};

  for (const auto& ignored : ignore_execs) {
    if (filepath == ignored) {
      return true;
    }
  }

  // Use the same ignore patterns as for shared libraries
  for (const auto& pattern : ignore_patterns_) {
    if (filepath.find(pattern) != std::string::npos) {
      return true;
    }
  }

  // Skip if it's in the current build directory
  if (filepath.find("./") == 0 ||
      filepath.find("build/") != std::string::npos) {
    return true;
  }

  return false;
}

bool Tracker::shouldIgnoreArtifact(const std::string& filepath) const {
  // Ignore CMake temporary files and directories
  if (filepath.find("CMakeFiles/") != std::string::npos) {
    return true;
  }

  // Ignore CMake cache and configuration files
  if (filepath.find("CMakeCache.txt") != std::string::npos ||
      filepath.find("cmake_install.cmake") != std::string::npos ||
      filepath.find("Makefile") != std::string::npos) {
    return true;
  }

  // Ignore object files and temporary files
  if (filepath.size() >= 2 && filepath.substr(filepath.size() - 2) == ".o") {
    return true;
  }

  // Ignore temporary and intermediate files
  if (filepath.find(".tmp") != std::string::npos ||
      filepath.find(".temp") != std::string::npos) {
    return true;
  }

  // Use base ignore patterns
  return shouldIgnoreFile(filepath);
}

std::string Tracker::joinCommand(
    const std::vector<std::string>& command) const {
  std::ostringstream oss;
  for (size_t i = 0; i < command.size(); ++i) {
    if (i > 0) oss << " ";

    const std::string& arg = command[i];
    // Check if argument contains spaces or other shell special characters
    if (arg.find(' ') != std::string::npos ||
        arg.find('\t') != std::string::npos ||
        arg.find('&') != std::string::npos ||
        arg.find('|') != std::string::npos ||
        arg.find(';') != std::string::npos ||
        arg.find('(') != std::string::npos ||
        arg.find(')') != std::string::npos) {
      oss << "\"" << arg << "\"";
    } else {
      oss << arg;
    }
  }
  return oss.str();
}

void Tracker::prepareBuildEnvironment() {
  Logger::info("Preparing build environment...");

  // Set SOURCE_DATE_EPOCH using the timestamp from constructor
  Utils::setSourceDateEpoch(build_timestamp_);

  // Set compiler options for reproducible builds
  std::string current_path = std::filesystem::current_path().string();
  Utils::setCompilerOptions(current_path);
}

BuildRecord Tracker::trackBuild(const std::vector<std::string>& build_command) {
  Logger::info("Starting build tracking for project: " + project_name_);
  Logger::info("Build command: " + joinCommand(build_command));

  std::string strace_output;
  try {
    strace_output = executeWithStrace(build_command);
  } catch (const std::exception& e) {
    Logger::error("Error executing build command: " + std::string(e.what()));
    return BuildRecord(project_name_);
  }

  auto so_files = parseSoFiles(strace_output);
  auto executables = parseExecutables(strace_output);
  Logger::info("Found " + std::to_string(so_files.size()) +
               " shared libraries");
  Logger::info("Found " + std::to_string(executables.size()) + " executables");

  BuildRecord record(project_name_);

  // Set metadata information
  record.setArchitecture(Utils::getArchitecture());
  record.setDistribution(Utils::getDistribution());
  record.setBuildPath(std::filesystem::current_path().string());
  record.setBuildTimestamp(build_timestamp_);
  record.setHostname(Utils::getHostname());
  record.setLocale(Utils::getLocale());
  record.setUmask(Utils::getUmask());

  // Process shared libraries
  for (const auto& so_file : so_files) {
    try {
      Logger::debug("Processing shared library: " + so_file);

      DependencyPackage dep = DependencyPackage::fromRawFile(so_file);
      if (dep.isValid()) {
        record.addDependency(dep);
        Logger::debug("  Added: " + dep.getPackageName() + " v" +
                      dep.getVersion());
      } else {
        Logger::debug("  Skipped invalid dependency: " + so_file);
      }
    } catch (const std::exception& e) {
      Logger::warn("Error processing " + so_file + ": " + e.what());
    }
  }

  // Process executables
  for (const auto& executable : executables) {
    try {
      Logger::debug("Processing executable: " + executable);

      DependencyPackage dep = DependencyPackage::fromRawFile(executable);
      if (dep.isValid()) {
        record.addDependency(dep);
        Logger::debug("  Added: " + dep.getPackageName() + " v" +
                      dep.getVersion());
      } else {
        Logger::debug("  Skipped invalid dependency: " + executable);
      }
    } catch (const std::exception& e) {
      Logger::warn("Error processing " + executable + ": " + e.what());
    }
  }

  // Detect build artifacts from strace output
  detectBuildArtifacts(strace_output, record);

  try {
    record.saveToFile(output_file_);
    Logger::info("Saved build record to: " + output_file_);
  } catch (const std::exception& e) {
    Logger::error("Error saving build record: " + std::string(e.what()));
  }

  last_build_record_ = record;
  return record;
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
        bool has_so_extension =
            (filepath.size() >= 3 &&
             filepath.substr(filepath.size() - 3) == ".so") ||
            filepath.find(".so.") != std::string::npos;
        if (has_so_extension) {
          is_shared_lib = true;
        }
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
