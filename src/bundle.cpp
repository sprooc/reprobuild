#include "bundle.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "logger.h"

namespace fs = std::filesystem;

void createBundle(const BuildRecord& record, const std::string& bundle_path) {
  try {
    // Generate unique temporary directory name
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << "reprobuild_tmp_"
        << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S") << "_"
        << std::rand() % 10000;
    std::string temp_dir = "/tmp/" + oss.str();

    std::string abs_bundle_path = fs::absolute(bundle_path).string();

    // Create temporary directory for staging
    fs::create_directories(temp_dir);
    Logger::debug("Created temporary directory: " + temp_dir);

    // 1. Copy build_path folder if it exists and is not empty
    std::string build_path = record.getBuildPath();
    if (!build_path.empty() && fs::exists(build_path)) {
      std::string dest_build_path = temp_dir + "/build";
      fs::create_directories(dest_build_path);

      // Copy the entire build directory
      fs::copy(
          build_path, dest_build_path,
          fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    } else {
      Logger::warn("Build path '" + build_path +
                   "' does not exist or is empty");
    }

    // 2. Copy all custom dependencies
    std::string deps_dir = temp_dir + "/dependencies";
    fs::create_directories(deps_dir);

    auto dependencies = record.getAllDependencies();
    int custom_dep_count = 0;

    for (const auto& dep : dependencies) {
      if (dep.getOrigin() == DependencyOrigin::Custom) {
        std::string original_path = dep.getOriginalPath();

        if (!original_path.empty() && fs::exists(original_path)) {
          std::string dep_name = dep.getPackageName();
          std::string dest_path = deps_dir + "/" + dep_name;

          if (fs::is_directory(original_path)) {
            // Copy directory
            fs::copy(original_path, dest_path,
                     fs::copy_options::recursive |
                         fs::copy_options::overwrite_existing);
          } else {
            // Copy file and create directory structure if needed
            fs::create_directories(fs::path(dest_path).parent_path());
            fs::copy_file(original_path, dest_path,
                          fs::copy_options::overwrite_existing);
          }

          Logger::info("Copied custom dependency: " + dep_name);
          custom_dep_count++;
        } else {
          Logger::warn("Original path for dependency '" + dep.getPackageName() +
                       "' does not exist: " + original_path);
        }
      }
    }

    Logger::debug("Copied " + std::to_string(custom_dep_count) +
                  " custom dependencies");

    // 3. Save build_record.yaml
    std::string yaml_path = temp_dir + "/build_record.yaml";
    record.saveToFile(yaml_path);

    // 4. Create compressed archive
    std::string bundle_name = fs::path(abs_bundle_path).stem().string();
    std::string archive_cmd;

    // Determine compression format based on file extension
    std::string ext = fs::path(abs_bundle_path).extension().string();
    if (ext == ".tar.gz" || ext == ".tgz") {
      archive_cmd = "cd " + temp_dir + " && tar -czf " + abs_bundle_path + " .";
    } else if (ext == ".tar.bz2" || ext == ".tbz2") {
      archive_cmd = "cd " + temp_dir + " && tar -cjf " + abs_bundle_path + " .";
    } else if (ext == ".tar.xz") {
      archive_cmd = "cd " + temp_dir + " && tar -cJf " + abs_bundle_path + " .";
    } else if (ext == ".zip") {
      archive_cmd = "cd " + temp_dir + " && zip -q -r " + abs_bundle_path + " .";
    } else {
      // Default to tar.gz
      archive_cmd =
          "cd " + temp_dir + " && tar -czf " + abs_bundle_path + ".tar.gz .";
      Logger::warn("Unrecognized extension '" + ext +
                   "', defaulting to .tar.gz format");
    }

    Logger::info("Creating archive with command: " + archive_cmd);
    int result = std::system(archive_cmd.c_str());

    if (result != 0) {
      throw std::runtime_error("Failed to create archive. Command returned: " +
                               std::to_string(result));
    }

    // 5. Clean up temporary directory
    fs::remove_all(temp_dir);

    Logger::info("Bundle created successfully: " + bundle_path);

  } catch (const std::exception& e) {
    Logger::error("Error creating bundle: " + std::string(e.what()));
    throw;
  }
}