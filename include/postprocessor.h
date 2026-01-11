#ifndef POSTPROCESSOR_H
#define POSTPROCESSOR_H
#include <memory>

#include "build_info.h"

class Postprocessor {
 public:
  Postprocessor(std::shared_ptr<BuildInfo> build_info)
      : build_info_(build_info) {}
  void postprocess();

 private:
  std::shared_ptr<BuildInfo> build_info_;

  void processGitCloneLogs();
};

#endif
