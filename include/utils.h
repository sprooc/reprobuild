#ifndef UTILS_H
#define UTILS_H

#include <string>

namespace Utils {
std::string calculateFileHash(const std::string& filepath);
std::string getCurrentTimestamp();
std::string getArchitecture();
std::string getDistribution();
std::string getHostname();
std::string getLocale();
std::string getUmask();
void setSourceDateEpoch(const std::string& timestamp);
void setCompilerOptions(const std::string& build_path);
}  // namespace Utils

#endif  // UTILS_H