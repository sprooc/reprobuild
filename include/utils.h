#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <vector>
#include "build_info.h"


namespace Utils {
bool contains(const std::string& s, const std::string& key);
bool startsWith(const std::string& s, const std::string& prefix);
bool endsWith(const std::string& s, const std::string& suffix);
std::string executeCommand(const std::string& command);
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
PackageMgr checkPackageManager();
bool isSharedLib(const std::string& filepath);
}  // namespace Utils

#endif  // UTILS_H