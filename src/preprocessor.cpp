#include "preprocessor.h"

#include <filesystem>
#include <fstream>

#include "interceptor_embedded.h"
#include "logger.h"
#include "utils.h"
#include <unistd.h>


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