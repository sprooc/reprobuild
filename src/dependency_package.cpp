#include "dependency_package.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>

#include "utils.h"

DependencyPackage::DependencyPackage()
    : package_name_(""), original_path_(""), version_(""), hash_value_("") {}

DependencyPackage::DependencyPackage(const std::string& package_name,
                                     //  const DependencyKind kind,
                                     const DependencyOrigin origin,
                                     const std::string& original_path,
                                     const std::string& version,
                                     const std::string& hash_value)
    : package_name_(package_name),
      // kind_(kind),
      origin_(origin),
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
  return package_name_ == other.package_name_ &&
         //  kind_ == other.kind_ &&
         origin_ == other.origin_ && original_path_ == other.original_path_ &&
         version_ == other.version_ && hash_value_ == other.hash_value_;
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

bool DependencyPackage::operator==(const DependencyPackage& other) const {
  return package_name_ == other.package_name_ &&
         //  kind_ == other.kind_ &&
         origin_ == other.origin_ && original_path_ == other.original_path_ &&
         version_ == other.version_ && hash_value_ == other.hash_value_;
}

bool DependencyPackage::operator!=(const DependencyPackage& other) const {
  return !(*this == other);
}

bool DependencyPackage::operator<(const DependencyPackage& other) const {
  // Sort by package name first, then by version
  if (package_name_ != other.package_name_) {
    return package_name_ < other.package_name_;
  }
  return version_ < other.version_;
}

// Stream output operator
std::ostream& operator<<(std::ostream& os, const DependencyPackage& package) {
  os << package.toString();
  return os;
}

bool checkPackageWithDpkg(const std::string& raw_file_path,
                          DependencyPackage& package) {
  // Get realpath of the file
  std::string realpath_command = "realpath " + raw_file_path;
  std::string real_path = Utils::executeCommand(realpath_command);

  // Use dpkg -S to find which package owns the file
  // Try raw file path first, then real path if needed
  std::string dpkg_command =
      "dpkg -S " + raw_file_path + " 2>/dev/null | head -1 | cut -d: -f1";
  std::string package_name = Utils::executeCommand(dpkg_command);

  if (package_name.empty() || Utils::startsWith(package_name, "diversion by")) {
    // If raw path failed, try with real path
    dpkg_command =
        "dpkg -S " + real_path + " 2>/dev/null | head -1 | cut -d: -f1";
    package_name = Utils::executeCommand(dpkg_command);
  }

  if (package_name.empty()) {
    return false;  // Package not found
  }

  // Get precise version using dpkg-query
  std::string version_command =
      "dpkg-query -W -f='${Version}\\n' " + package_name + " 2>/dev/null";
  std::string version = Utils::executeCommand(version_command);

  if (version.empty()) {
    throw std::runtime_error("Could not get version for package: " +
                             package_name);
  }

  // Calculate SHA256 hash of the file
  std::string hash_value = Utils::calculateFileHash(real_path);

  if (hash_value.empty()) {
    throw std::runtime_error("Could not calculate hash for file: " +
                             raw_file_path);
  }

  package = DependencyPackage(package_name, DependencyOrigin::APT, real_path,
                              version, hash_value);
  return true;
}

bool checkPackageWithRpm(const std::string& raw_file_path,
                         DependencyPackage& package) {
  // Get realpath of the file
  std::string realpath_command = "realpath " + raw_file_path;
  std::string real_path = Utils::executeCommand(realpath_command);

  // Use rpm -qf to find which package owns the file
  std::string rpm_command =
      "rpm -qf " + raw_file_path + " 2>/dev/null | head -1 | cut -d: -f1";
  std::string package_name = Utils::executeCommand(rpm_command);

  if (package_name.empty()) {
    // If raw path failed, try with real path
    rpm_command =
        "rpm -qf " + real_path + " 2>/dev/null | head -1 | cut -d: -f1";
    package_name = Utils::executeCommand(rpm_command);
  }

  if (package_name.empty()) {
    return false;  // Package not found
  }

  // Get precise package name without version
  std::string name_command =
      "rpm -q --qf '%{NAME}\n' " + package_name + " 2>/dev/null";
  std::string package_name_clean = Utils::executeCommand(name_command);
  if (package_name_clean.empty()) {
    throw std::runtime_error("Could not get name for package: " + package_name);
  }

  // Get precise version using rpm
  std::string version_command =
      "rpm -q --qf '%{VERSION}-%{RELEASE}\n' " + package_name + " 2>/dev/null";
  std::string version = Utils::executeCommand(version_command);

  if (version.empty()) {
    throw std::runtime_error("Could not get version for package: " +
                             package_name);
  }

  // Calculate SHA256 hash of the file
  std::string hash_value = Utils::calculateFileHash(real_path);

  if (hash_value.empty()) {
    throw std::runtime_error("Could not calculate hash for file: " +
                             raw_file_path);
  }

  package = DependencyPackage(package_name_clean, DependencyOrigin::DNF,
                              real_path, version, hash_value);
  return true;
}

// Static method to create a DependencyPackage from a raw file
DependencyPackage DependencyPackage::fromRawFile(
    const std::string& raw_file_path, PackageMgr pkg_mgr) {
  try {
    // Check if file exists
    if (!std::filesystem::exists(raw_file_path)) {
      throw std::runtime_error("File does not exist: " + raw_file_path);
    }

    DependencyPackage package;
    bool success = false;

    switch (pkg_mgr) {
      case PackageMgr::APT:
        success = checkPackageWithDpkg(raw_file_path, package);
        break;
      case PackageMgr::DNF:
      case PackageMgr::YUM:
        success = checkPackageWithRpm(raw_file_path, package);
        break;
      default:
        throw std::runtime_error("Unsupported package manager");
    }

    if (success) {
      return package;
    } else {
      // Custom file not owned by any package
      std::string realpath_command = "realpath " + raw_file_path;
      std::string real_path = Utils::executeCommand(realpath_command);
      std::string package_name =
          std::filesystem::path(real_path).filename().string();
      std::string hash_value = Utils::calculateFileHash(real_path);
      DependencyPackage package =
          DependencyPackage(package_name, DependencyOrigin::CUSTOM,
                            real_path, "custom", hash_value);
      return package;
    }

  } catch (const std::exception& e) {
    // If any step fails, return an invalid package with error information
    DependencyPackage invalid_package;
    invalid_package.setOriginalPath(raw_file_path);
    invalid_package.setHashValue("ERROR: " + std::string(e.what()));
    return invalid_package;
  }
}
