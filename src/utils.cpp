#include "utils.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

#include "logger.h"

namespace Utils {

std::string calculateFileHash(const std::string& filepath) {
  try {
    std::string hash_command =
        "sha256sum \"" + filepath + "\" 2>/dev/null | cut -d' ' -f1";
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(hash_command.c_str(), "r"),
                                               pclose);

    if (!pipe) {
      return "";
    }

    char buffer[128];
    std::string result;
    if (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
      result = buffer;
      // Remove trailing newline
      if (!result.empty() && result.back() == '\n') {
        result.pop_back();
      }
    }

    return result;
  } catch (const std::exception& e) {
    Logger::warn("Error calculating hash for " + filepath + ": " + e.what());
    return "";
  }
}

std::string getCurrentTimestamp() {
  std::string date_command = "date -Iseconds 2>/dev/null";
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(date_command.c_str(), "r"),
                                             pclose);

  if (!pipe) {
    return "Unknown";
  }

  char buffer[128];
  std::string result;
  if (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
    result = buffer;
    // Remove trailing newline
    if (!result.empty() && result.back() == '\n') {
      result.pop_back();
    }
  }

  return result.empty() ? "Unknown" : result;
}

std::string getArchitecture() {
  std::string arch_command = "uname -m 2>/dev/null";
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(arch_command.c_str(), "r"),
                                             pclose);

  if (!pipe) {
    return "Unknown";
  }

  char buffer[128];
  std::string result;
  if (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
    result = buffer;
    // Remove trailing newline
    if (!result.empty() && result.back() == '\n') {
      result.pop_back();
    }
  }

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
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(lsb_command.c_str(), "r"),
                                             pclose);

  if (pipe) {
    char buffer[256];
    std::string result;
    if (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
      result = buffer;
      // Remove trailing newline
      if (!result.empty() && result.back() == '\n') {
        result.pop_back();
      }
      // Remove quotes if present
      if (result.front() == '"' && result.back() == '"') {
        result = result.substr(1, result.length() - 2);
      }
      if (!result.empty()) {
        return result;
      }
    }
  }

  return "Unknown";
}

std::string getHostname() {
  std::string hostname_command = "hostname 2>/dev/null";
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(
      popen(hostname_command.c_str(), "r"), pclose);

  if (!pipe) {
    return "Unknown";
  }

  char buffer[128];
  std::string result;
  if (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
    result = buffer;
    // Remove trailing newline
    if (!result.empty() && result.back() == '\n') {
      result.pop_back();
    }
  }

  return result.empty() ? "Unknown" : result;
}

std::string getLocale() {
  std::string locale_command = "locale 2>/dev/null";
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(locale_command.c_str(), "r"),
                                             pclose);

  if (!pipe) {
    return "Unknown";
  }

  std::string result;
  char buffer[256];
  // Read all lines and concatenate them
  while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
    std::string line = buffer;
    // Remove trailing newline
    if (!line.empty() && line.back() == '\n') {
      line.pop_back();
    }
    if (!result.empty()) {
      result += ";";
    }
    result += line;
  }

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
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(epoch_command.c_str(), "r"),
                                             pclose);

  std::string epoch_str;
  if (pipe) {
    char buffer[64];
    if (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
      epoch_str = buffer;
      // Remove trailing newline
      if (!epoch_str.empty() && epoch_str.back() == '\n') {
        epoch_str.pop_back();
      }
    }
  }

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

}  // namespace Utils