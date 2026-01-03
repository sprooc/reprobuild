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

  void prepareBuildEnvironment();

  void setOutputFile(const std::string& output_file);
  void setLogDirectory(const std::string& log_dir);
  void addIgnorePattern(const std::string& pattern);

  const BuildRecord& getLastBuildRecord() const;

 private:
  std::string build_timestamp_;
  std::string project_name_;
  std::string output_file_;
  std::string log_dir_;
  std::vector<std::string> ignore_patterns_;
  BuildRecord last_build_record_;
  std::string random_seed_;

  void setCompilerOptions(const std::string& build_path);
  std::string executeWithStrace(const std::vector<std::string>& command);
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
  std::string joinCommand(const std::vector<std::string>& command) const;

};

#endif  // DEPENDENCY_TRACKER_H
