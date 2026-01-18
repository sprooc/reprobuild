#include "utils.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <vector>

#include "logger.h"

namespace Utils {

bool contains(const std::string& s, const std::string& key) {
  return s.find(key) != std::string::npos;
}

bool startsWith(const std::string& s, const std::string& prefix) {
  return s.rfind(prefix, 0) == 0;
}

bool endsWith(const std::string& s, const std::string& suffix) {
  if (s.length() >= suffix.length()) {
    return (0 ==
            s.compare(s.length() - suffix.length(), suffix.length(), suffix));
  } else {
    return false;
  }
}

// Helper function to execute shell command and get output
std::string executeCommand(const std::string& command) {
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(command.c_str(), "r"),
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

std::string calculateFileHash(const std::string& filepath) {
  try {
    std::string hash_command =
        "sha256sum \"" + filepath + "\" 2>/dev/null | cut -d' ' -f1";
    return executeCommand(hash_command);
  } catch (const std::exception& e) {
    Logger::warn("Error calculating hash for " + filepath + ": " + e.what());
    return "";
  }
}

std::string getCurrentTimestamp() {
  std::string date_command = "date -Iseconds 2>/dev/null";
  std::string result = executeCommand(date_command);
  return result.empty() ? "Unknown" : result;
}

std::string getArchitecture() {
  std::string arch_command = "uname -m 2>/dev/null";
  std::string result = executeCommand(arch_command);
  return result.empty() ? "Unknown" : result;
}

std::string getDistribution() {
  // Try to get distribution info from /etc/os-release first
  std::ifstream os_release("/etc/os-release");
  if (os_release.is_open()) {
    std::string line;
    while (std::getline(os_release, line)) {
      if (line.find("PRETTY_NAME=") == 0) {
        std::string dist = line.substr(12);
        // Remove quotes if present
        if (dist.front() == '"' && dist.back() == '"') {
          dist = dist.substr(1, dist.length() - 2);
        }
        return dist;
      }
    }
    os_release.close();
  }

  // Fallback to lsb_release command
  std::string lsb_command = "lsb_release -d -s 2>/dev/null";
  std::string result = executeCommand(lsb_command);
  return result.empty() ? "Unknown" : result;
}

std::string getHostname() {
  std::string hostname_command = "uname -n 2>/dev/null";
  std::string result = executeCommand(hostname_command);
  return result.empty() ? "Unknown" : result;
}

std::string getLocale() {
  std::string locale_command = "locale 2>/dev/null";
  std::string result = executeCommand(locale_command);
  return result.empty() ? "Unknown" : result;
}

std::string getUmask() {
  // Get current umask by setting it temporarily
  mode_t current_mask = umask(0);
  umask(current_mask);  // Restore original umask

  // Convert to octal string format (e.g., "0022")
  std::ostringstream oss;
  oss << std::oct << std::setfill('0') << std::setw(4) << current_mask;
  return oss.str();
}

void setSourceDateEpoch(const std::string& timestamp) {
  Logger::debug("Setting SOURCE_DATE_EPOCH for timestamp: " + timestamp);

  // Convert ISO timestamp to Unix timestamp for SOURCE_DATE_EPOCH
  std::string epoch_command = "date -d '" + timestamp + "' +%s 2>/dev/null";
  std::string epoch_str = executeCommand(epoch_command);

  if (!epoch_str.empty()) {
    // Set SOURCE_DATE_EPOCH environment variable
    if (setenv("SOURCE_DATE_EPOCH", epoch_str.c_str(), 1) == 0) {
      Logger::info("Set SOURCE_DATE_EPOCH=" + epoch_str + " (" + timestamp +
                   ")");
    } else {
      Logger::warn("Failed to set SOURCE_DATE_EPOCH environment variable");
    }
  } else {
    Logger::warn("Failed to convert timestamp to epoch format");
  }
}

void appendEnvVar(const std::string& name, const std::string& value) {
  const char* existing = std::getenv(name.c_str());
  std::string updated_value;

  if (existing && existing[0] != '\0') {
    updated_value = std::string(existing) + " " + value;
  } else {
    updated_value = value;
  }

  if (setenv(name.c_str(), updated_value.c_str(), 1) == 0) {
    Logger::debug("Set " + name + "=" + updated_value);
  } else {
    Logger::warn("Failed to set " + name + " environment variable");
  }
}

std::string joinCommand(const std::vector<std::string>& command) {
  std::ostringstream oss;
  for (size_t i = 0; i < command.size(); ++i) {
    if (i > 0) oss << " ";

    const std::string& arg = command[i];
    // Check if argument contains spaces or other shell special characters
    if (contains(arg, " ") || contains(arg, "\t") || contains(arg, "&") ||
        contains(arg, "|") || contains(arg, ";") || contains(arg, "(") ||
        contains(arg, ")")) {
      oss << "\"" << arg << "\"";
    } else {
      oss << arg;
    }
  }
  return oss.str();
}

PackageMgr checkPackageManager() {
  std::string distro = getDistribution();

  if (contains(distro, "Ubuntu") || contains(distro, "Debian")) {
    return PackageMgr::APT;
  } else if (contains(distro, "Fedora")) {
    return PackageMgr::DNF;
  } else if (contains(distro, "CentOS")) {
    return PackageMgr::YUM;
  } else if (contains(distro, "Arch Linux")) {
    return PackageMgr::PACMAN;
  } else {
    return PackageMgr::UNKNOWN;  // Default to unknown
  }
}

bool isSharedLib(const std::string& filepath) {
  if (endsWith(filepath, ".so")) {
    return true;
  }
  // Check for .so.<version> pattern
  size_t so_pos = filepath.rfind(".so.");
  if (so_pos != std::string::npos) {
    // All characters after .so. should be digits or dots
    std::string version_part = filepath.substr(so_pos + 4);
    for (char c : version_part) {
      if (!isdigit(c) && c != '.') {
        return false;
      }
    }
    return true;
  }
  return false;
}

}  // namespace Utils