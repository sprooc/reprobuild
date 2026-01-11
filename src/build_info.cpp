#include "build_info.h"

#include <filesystem>

#include "utils.h"

BuildInfo::BuildInfo(std::string build_command, std::string output_file,
                     std::string log_dir)
    : build_command_(build_command),
      output_file_(output_file),
      log_dir_(log_dir),
      random_seed_("0") {
  build_timestamp_ = Utils::getCurrentTimestamp();
  build_path_ = std::filesystem::current_path().string();
  git_commit_log_path_ = log_dir_ + "/git_clone_commits.log";
}

void BuildInfo::fillBuildRecordMetadata() {
  build_record_.setArchitecture(Utils::getArchitecture());
  build_record_.setDistribution(Utils::getDistribution());
  build_record_.setBuildPath(build_path_);
  build_record_.setBuildTimestamp(build_timestamp_);
  build_record_.setHostname(Utils::getHostname());
  build_record_.setLocale(Utils::getLocale());
  build_record_.setUmask(Utils::getUmask());
  build_record_.setRandomSeed(random_seed_);
  build_record_.setBuildCommand(build_command_);
}