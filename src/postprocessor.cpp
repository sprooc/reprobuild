#include "postprocessor.h"

#include <fstream>

#include "build_record.h"

void Postprocessor::postprocess() { processGitCloneLogs(); }

void Postprocessor::processGitCloneLogs() {
  std::string log_path = build_info_->git_commit_log_path_;

  std::ifstream log_file(log_path);
  if (!log_file.is_open()) {
    return;
  }

  BuildRecord& record = build_info_->build_record_;

  std::string line;
  while (std::getline(log_file, line)) {
    size_t sep_pos = line.find(' ');
    if (sep_pos != std::string::npos) {
      std::string repo_url = line.substr(0, sep_pos);
      std::string commit_id = line.substr(sep_pos + 1);
      record.addGitCommitId(repo_url, commit_id);
    }
  }
  
  log_file.close();

  // Delete the log file after processing
  std::remove(log_path.c_str());
}