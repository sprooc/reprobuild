#ifndef DEPENDENCY_TRACKER_H
#define DEPENDENCY_TRACKER_H

#include <set>
#include <string>
#include <vector>

#include "build_record.h"
#include "dependency_package.h"

class Tracker {
 public:
  Tracker();
  Tracker(const std::string& project_name);

  BuildRecord trackBuild(const std::vector<std::string>& build_command);

  void setOutputFile(const std::string& output_file);
  void setLogDirectory(const std::string& log_dir);
  void addIgnorePattern(const std::string& pattern);

  const BuildRecord& getLastBuildRecord() const;

 private:
  std::string project_name_;
  std::string output_file_;
  std::string log_dir_;
  std::vector<std::string> ignore_patterns_;
  BuildRecord last_build_record_;

  std::string executeWithStrace(const std::vector<std::string>& command);
  std::set<std::string> parseSoFiles(const std::string& strace_output);
  bool shouldIgnoreFile(const std::string& filepath) const;
  std::string joinCommand(const std::vector<std::string>& command) const;
};

#endif  // DEPENDENCY_TRACKER_H
