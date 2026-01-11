#include "preprocessor.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <regex>

#include "canonicalizer.h"
#include "interceptor_embedded.h"
#include "logger.h"
#include "utils.h"

std::string Preprocessor::getInterceptorLibraryPath() const {
  Logger::debug("Extracting embedded execve interceptor library...");

  // Create temporary file for the interceptor library
  std::string lib_path = build_info_->log_dir_ + "/reprobuild_interceptor_" +
                         std::to_string(getpid()) + ".so";

  try {
    // Write embedded library data to temporary file
    std::ofstream lib_file(lib_path, std::ios::binary);
    if (!lib_file.is_open()) {
      Logger::warn("Failed to create temporary interceptor library file: " +
                   lib_path);
      return "";
    }

    lib_file.write(
        reinterpret_cast<const char*>(EmbeddedInterceptor::INTERCEPTOR_DATA),
        EmbeddedInterceptor::INTERCEPTOR_SIZE);
    lib_file.close();

    // Verify the file was written correctly
    if (!std::filesystem::exists(lib_path)) {
      Logger::warn("Failed to extract interceptor library to: " + lib_path);
      return "";
    }

    // Make the file executable
    std::filesystem::permissions(lib_path,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write |
                                     std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_read |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_read |
                                     std::filesystem::perms::others_exec);

    Logger::debug("Successfully extracted interceptor library to: " + lib_path);
    Logger::debug("Library size: " +
                  std::to_string(EmbeddedInterceptor::INTERCEPTOR_SIZE) +
                  " bytes");

    return lib_path;

  } catch (const std::exception& e) {
    Logger::warn("Error extracting interceptor library: " +
                 std::string(e.what()));
    return "";
  }
}

void Preprocessor::setCompilerOptions() {
  Logger::info("Setting compiler options for reproducible builds...");

  std::string comp_opts;
  comp_opts.reserve(256);

  // Create -ffile-prefix-map option to normalize paths
  std::string prefix_map_option =
      " -ffile-prefix-map=" + build_info_->build_path_ + "=.";
  comp_opts.append(prefix_map_option);
  // Set random seed
  std::string seed_option = " -frandom-seed=" + build_info_->random_seed_;
  comp_opts.append(seed_option);

  Utils::appendEnvVar("CFLAGS", comp_opts);
  Utils::appendEnvVar("CXXFLAGS", comp_opts);
  Utils::appendEnvVar("CPPFLAGS", comp_opts);
  Utils::appendEnvVar("REPROBUILD_COMPILER_FLAGS", comp_opts);
}

void Preprocessor::prepareBuildEnvironment() {
  Logger::info("Preparing build environment...");

  // Set SOURCE_DATE_EPOCH using the timestamp from constructor
  Utils::setSourceDateEpoch(build_info_->build_timestamp_);
  // Set git commit log path environment variable
  Utils::appendEnvVar("REPROBUILD_LOG_GIT_CLONES",
                      build_info_->git_commit_log_path_);
  setenv("REPROBUILD_STAGE", "build", 1);

  // Set compiler options for reproducible builds
  setCompilerOptions();

  // Load the interceptor library
  auto interceptor_lib_path = getInterceptorLibraryPath();
  if (!interceptor_lib_path.empty()) {
    build_info_->interceptor_lib_path_ = interceptor_lib_path;
    Utils::appendEnvVar("LD_PRELOAD", interceptor_lib_path);
  } else {
    Logger::warn("Interceptor library path is empty, build tracking may fail.");
  }
}

void Preprocessor::fixMakefile() {
  std::string build_cmd = build_info_->build_command_;
  if (build_cmd.find("make") == std::string::npos) {
    Logger::debug(
        "Build command does not contain 'make', skipping makefile fixing");
    return;
  }

  // Parse build command to find make execution directory
  std::string make_dir = ".";  // Default to current directory

  // Check for "cd <dir> && make" pattern
  std::regex cd_make_pattern(R"(cd\s+([^\s;&|]+)\s*&&[^;]*make)");
  std::smatch match;

  if (std::regex_search(build_cmd, match, cd_make_pattern)) {
    make_dir = match[1].str();
    Logger::info("Found 'cd && make' pattern, make directory: " + make_dir);
  } else {
    Logger::debug("No 'cd && make' pattern found, using current directory");
  }

  // Convert relative path to absolute
  std::filesystem::path abs_make_dir;
  try {
    if (std::filesystem::path(make_dir).is_relative()) {
      abs_make_dir = std::filesystem::current_path() / make_dir;
    } else {
      abs_make_dir = make_dir;
    }
    abs_make_dir = std::filesystem::canonical(abs_make_dir);
  } catch (const std::exception& e) {
    Logger::warn("Failed to resolve make directory: " + std::string(e.what()));
    abs_make_dir = std::filesystem::current_path();
  }

  Logger::debug("Make execution directory: " + abs_make_dir.string());

  // Find Makefile in the make directory
  std::vector<std::string> makefile_candidates = {"Makefile", "makefile",
                                                  "GNUmakefile"};

  std::string makefile_path;
  for (const auto& candidate : makefile_candidates) {
    std::filesystem::path candidate_path = abs_make_dir / candidate;
    if (std::filesystem::exists(candidate_path)) {
      makefile_path = candidate_path.string();
      Logger::info("Found makefile: " + makefile_path);
      break;
    }
  }

  if (makefile_path.empty()) {
    Logger::warn("No makefile found in " + abs_make_dir.string());
    return;
  }

  Canonicalizer canon;
  canon.add_default_rules();
  canon.apply_to_file(makefile_path);

  Logger::info("Successfully applied reproducible build fixes to: " +
               makefile_path);
}
