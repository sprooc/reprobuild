#ifndef BUILD_INFO_H
#define BUILD_INFO_H

#include <string>

#include "build_record.h"

class BuildInfo {
 public:
  BuildInfo(std::string build_command, std::string output_file,
            std::string log_dir);

  std::string build_command_;
  std::string output_file_;
  std::string log_dir_;

  std::string build_timestamp_;
  std::string build_path_;
  std::string random_seed_;

  std::string interceptor_lib_path_;

  BuildRecord build_record_;

  void fillBuildRecordMetadata();
};

#endif