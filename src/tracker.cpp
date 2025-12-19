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

Tracker::Tracker()
    : project_name_("unknown"),
      output_file_("build_record.yaml"),
      log_dir_("/tmp") {
  ignore_patterns_.push_back("/tmp/");
  ignore_patterns_.push_back("/proc/");
  ignore_patterns_.push_back("/sys/");
  ignore_patterns_.push_back("/dev/");
}

Tracker::Tracker(const std::string& project_name)
    : project_name_(project_name),
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
      "strace -e openat -f -q -o " + strace_log + " " + cmd_str;

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

      Logger::debug("Found .so file: " + filepath);
      so_files.insert(filepath);
    }
  }

  return so_files;
}

bool Tracker::shouldIgnoreFile(const std::string& filepath) const {
  for (const auto& pattern : ignore_patterns_) {
    if (filepath.find(pattern) != std::string::npos) {
      return true;
    }
  }

  if (filepath.find("./") == 0 ||
      filepath.find("build/") != std::string::npos) {
    return true;
  }

  return false;
}

std::string Tracker::joinCommand(
    const std::vector<std::string>& command) const {
  std::ostringstream oss;
  for (size_t i = 0; i < command.size(); ++i) {
    if (i > 0) oss << " ";
    oss << command[i];
  }
  return oss.str();
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
  Logger::info("Found " + std::to_string(so_files.size()) + " .so files");

  BuildRecord record(project_name_);

  for (const auto& so_file : so_files) {
    try {
      Logger::debug("Processing: " + so_file);

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

  try {
    record.saveToFile(output_file_);
    Logger::info("Saved build record to: " + output_file_);
  } catch (const std::exception& e) {
    Logger::error("Error saving build record: " + std::string(e.what()));
  }

  last_build_record_ = record;
  return record;
}
