#include "dependency_package.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>

DependencyPackage::DependencyPackage()
    : package_name_(""), original_path_(""), version_(""), hash_value_("") {
}

DependencyPackage::DependencyPackage(const std::string& package_name,
                                     const std::string& original_path,
                                     const std::string& version,
                                     const std::string& hash_value)
    : package_name_(package_name),
      original_path_(original_path),
      version_(version),
      hash_value_(hash_value) {}

// Utility methods

bool DependencyPackage::isValid() const {
  // A valid dependency package must have non-empty name, version, and hash
  return !package_name_.empty() && !version_.empty() && !hash_value_.empty();
}

std::string DependencyPackage::generateUniqueId() const {
  // Generate unique identifier as "package_name@version"
  return package_name_ + "@" + version_;
}

bool DependencyPackage::matches(const DependencyPackage& other) const {
  // Two packages match if they have the same name and version
  return package_name_ == other.package_name_ && version_ == other.version_;
}

bool DependencyPackage::verifyIntegrity(
    const std::string& computed_hash) const {
  // Compare the stored hash with the computed hash
  return hash_value_ == computed_hash;
}

std::string DependencyPackage::toString() const {
  std::ostringstream oss;
  oss << "DependencyPackage{";
  oss << "name: \"" << package_name_ << "\", ";
  oss << "path: \"" << original_path_ << "\", ";
  oss << "version: \"" << version_ << "\", ";
  oss << "hash: \"" << hash_value_ << "\"";
  oss << "}";
  return oss.str();
}

// Stream output operator
std::ostream& operator<<(std::ostream& os, const DependencyPackage& package) {
  os << package.toString();
  return os;
}

// Helper function to execute shell command and get output
static std::string executeCommand(const std::string& command) {
  std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"),
                                                pclose);

  if (!pipe) {
    throw std::runtime_error("Failed to execute command: " + command);
  }

  std::string result;
  char buffer[128];
  while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
    result += buffer;
  }

  // Remove trailing newline
  if (!result.empty() && result.back() == '\n') {
    result.pop_back();
  }

  return result;
}

// Static method to create a DependencyPackage from a raw file
DependencyPackage DependencyPackage::fromRawFile(
    const std::string& raw_file_path) {
  try {
    // Check if file exists
    if (!std::filesystem::exists(raw_file_path)) {
      throw std::runtime_error("File does not exist: " + raw_file_path);
    }

    // Step 1: Get realpath of the file
    std::string realpath_command = "realpath " + raw_file_path;
    std::string real_path = executeCommand(realpath_command);

    // Step 2: Use dpkg -S to find which package owns the file
    std::string dpkg_command =
        "dpkg -S " + real_path + " 2>/dev/null | head -1 | cut -d: -f1";
    std::string package_name = executeCommand(dpkg_command);

    if (package_name.empty()) {
      throw std::runtime_error("Could not find package owner for file: " +
                               raw_file_path);
    }

    // Step 3: Get precise version using dpkg-query
    std::string version_command =
        "dpkg-query -W -f='${Version}\\n' " + package_name + " 2>/dev/null";
    std::string version = executeCommand(version_command);

    if (version.empty()) {
      throw std::runtime_error("Could not get version for package: " +
                               package_name);
    }

    // Step 4: Calculate SHA256 hash of the file
    std::string hash_command =
        "sha256sum " + real_path + " 2>/dev/null | cut -d' ' -f1";
    std::string hash_value = executeCommand(hash_command);

    if (hash_value.empty()) {
      throw std::runtime_error("Could not calculate hash for file: " +
                               raw_file_path);
    }

    // Create and return DependencyPackage object
    return DependencyPackage(package_name, real_path, version, hash_value);

  } catch (const std::exception& e) {
    // If any step fails, return an invalid package with error information
    DependencyPackage invalid_package;
    invalid_package.setOriginalPath(raw_file_path);
    invalid_package.setHashValue("ERROR: " + std::string(e.what()));
    return invalid_package;
  }
}
