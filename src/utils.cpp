#include "utils.h"

#include <cstdio>
#include <memory>

#include "logger.h"

namespace Utils {

std::string calculateFileHash(const std::string& filepath) {
  try {
    std::string hash_command =
        "sha256sum \"" + filepath + "\" 2>/dev/null | cut -d' ' -f1";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(
        popen(hash_command.c_str(), "r"), pclose);

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

}  // namespace Utils