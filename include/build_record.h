#ifndef BUILD_RECORD_H
#define BUILD_RECORD_H

#include <map>
#include <string>
#include <vector>

#include "dependency_package.h"

struct BuildArtifact {
  std::string
      path;  // File path (relative if in current dir, absolute otherwise)
  std::string hash;  // SHA256 hash of the file
  std::string type;  // "executable" or "shared_library"

  BuildArtifact() = default;
  BuildArtifact(const std::string& p, const std::string& h,
                const std::string& t)
      : path(p), hash(h), type(t) {}
};

class BuildRecord {
 public:
  BuildRecord();
  BuildRecord(const std::string& project_name);

  void addDependency(const DependencyPackage& package);
  void removeDependency(const std::string& package_name);

  void addArtifact(const BuildArtifact& artifact);
  void clearArtifacts();
  const std::vector<BuildArtifact>& getArtifacts() const;

  bool hasDependency(const std::string& package_name) const;
  DependencyPackage getDependency(const std::string& package_name) const;
  std::vector<DependencyPackage> getAllDependencies() const;

  size_t getDependencyCount() const;

  std::string toString() const;
  bool matches(const BuildRecord& other) const;
  void saveToFile(const std::string& filepath) const;
  static BuildRecord loadFromFile(const std::string& filepath);

  const std::string& getProjectName() const { return project_name_; }
  void setProjectName(const std::string& name) { project_name_ = name; }

  // Metadata setters and getters
  void setArchitecture(const std::string& arch) { architecture_ = arch; }
  void setDistribution(const std::string& dist) { distribution_ = dist; }
  void setBuildPath(const std::string& path) { build_path_ = path; }
  void setBuildTimestamp(const std::string& timestamp) {
    build_timestamp_ = timestamp;
  }
  void setHostname(const std::string& hostname) { hostname_ = hostname; }
  void setLocale(const std::string& locale) { locale_ = locale; }
  void setUmask(const std::string& umask) { umask_ = umask; }
  void setRandomSeed(const std::string& seed) { random_seed_ = seed; }
  void setBuildCommand(const std::string& cmd) { build_cmd_ = cmd; }

  const std::string& getArchitecture() const { return architecture_; }
  const std::string& getDistribution() const { return distribution_; }
  const std::string& getBuildPath() const { return build_path_; }
  const std::string& getBuildTimestamp() const { return build_timestamp_; }
  const std::string& getHostname() const { return hostname_; }
  const std::string& getLocale() const { return locale_; }
  const std::string& getUmask() const { return umask_; }
  const std::string& getRandomSeed() const { return random_seed_; }
  const std::string& getBuildCommand() const { return build_cmd_; }

 private:
  std::string project_name_;
  std::map<std::string, DependencyPackage> dependencies_;
  std::vector<BuildArtifact> artifacts_;

  // Metadata fields
  std::string architecture_;
  std::string distribution_;
  std::string build_path_;
  std::string build_timestamp_;
  std::string hostname_;
  std::string locale_;
  std::string umask_;
  std::string random_seed_;
  std::string build_cmd_;
};

#endif  // BUILD_RECORD_H
