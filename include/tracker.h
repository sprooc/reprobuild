#ifndef DEPENDENCY_TRACKER_H
#define DEPENDENCY_TRACKER_H

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "build_info.h"
#include "build_record.h"
#include "dependency_package.h"

class Tracker {
 public:
  Tracker(std::shared_ptr<BuildInfo> build_info);

  void trackBuild();

  void addIgnorePattern(const std::string& pattern);

 private:
  std::vector<std::string> ignore_patterns_;
  std::shared_ptr<BuildInfo> build_info_;

  std::string executeWithStrace(const std::string& command);
  std::set<std::string> parseLibFiles(const std::string& strace_output);
  std::set<std::string> parseHeaderFiles(const std::string& strace_output);
  std::set<std::string> parseExecutables(const std::string& strace_output);
  void detectBuildArtifacts(const std::string& strace_output,
                            BuildRecord& record);
  std::string makeRelativePath(const std::string& filepath,
                               const std::string& base_dir);
  bool shouldIgnoreFile(const std::string& filepath) const;
  bool shouldIgnoreLib(const std::string& filepath) const;
  bool shouldIgnoreHeader(const std::string& filepath) const;
  bool shouldIgnoreExecutable(const std::string& filepath) const;
  bool shouldIgnoreArtifact(const std::string& filepath) const;
};

#endif  // DEPENDENCY_TRACKER_H
