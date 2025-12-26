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

 private:
  std::string project_name_;
  std::map<std::string, DependencyPackage> dependencies_;
  std::vector<BuildArtifact> artifacts_;
};

#endif  // BUILD_RECORD_H
