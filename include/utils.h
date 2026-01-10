#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <vector>

namespace Utils {
std::string calculateFileHash(const std::string& filepath);
std::string getCurrentTimestamp();
std::string getArchitecture();
std::string getDistribution();
std::string getHostname();
std::string getLocale();
std::string getUmask();
void setSourceDateEpoch(const std::string& timestamp);
void appendEnvVar(const std::string& name, const std::string& value);
std::string joinCommand(const std::vector<std::string>& command);
}  // namespace Utils

#endif  // UTILS_H