#ifndef DEPENDENCY_TRACKER_H
#define DEPENDENCY_TRACKER_H

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "build_graph.h"
#include "build_info.h"
#include "build_record.h"
#include "dependency_package.h"

struct TrackingTiming {
  long long preprocessing_ms = 0;
  long long build_execution_ms = 0;
  long long bpftrace_finalization_ms = 0;
  long long raw_output_write_ms = 0;
  long long dependency_file_parse_ms = 0;
  long long dependency_resolution_ms = 0;
  long long artifact_detection_ms = 0;
  long long graph_parse_ms = 0;
  long long graph_prune_ms = 0;
  long long graph_total_ms = 0;
  long long postprocessing_ms = 0;
  long long total_ms = 0;
};

class Tracker {
 public:
  Tracker(std::shared_ptr<BuildInfo> build_info);

  void trackBuild();
  const TrackingTiming& getTiming() const;

  void addIgnorePattern(const std::string& pattern);

 private:
  std::vector<std::string> ignore_patterns_;
  std::shared_ptr<BuildInfo> build_info_;
  TrackingTiming timing_;

  std::string executeWithBpftrace(const std::string& command);
  std::string processBpftraceOutput(const std::string& raw_output);
  std::set<std::string> parseLibFiles(const std::string& bpftrace_output);
  std::set<std::string> parseHeaderFiles(const std::string& bpftrace_output);
  std::set<std::string> parseExecutables(const std::string& bpftrace_output);
  void detectBuildArtifacts(const std::string& bpftrace_output,
                            BuildRecord& record);
  void processCreatedFiles(const std::set<std::string>& created_files,
                           BuildRecord& record);
  BuildGraph parseBuildGraph(const std::string& bpftrace_output);
  std::string makeRelativePath(const std::string& filepath,
                               const std::string& base_dir);
  bool shouldIgnoreFile(const std::string& filepath) const;
  bool shouldIgnoreLib(const std::string& filepath) const;
  bool shouldIgnoreHeader(const std::string& filepath) const;
  bool shouldIgnoreExecutable(const std::string& filepath) const;
  bool shouldIgnoreArtifact(const std::string& filepath) const;
};

#endif  // DEPENDENCY_TRACKER_H
