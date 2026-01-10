#ifndef PREPROCESSOR_H
#define PREPROCESSOR_H
#include <memory>

#include "build_info.h"

class Preprocessor {
 public:
  Preprocessor(std::shared_ptr<BuildInfo> build_info)
      : build_info_(build_info) {}
  void prepareBuildEnvironment();

 private:
  std::shared_ptr<BuildInfo> build_info_;
  std::string getInterceptorLibraryPath() const;
  void setCompilerOptions();
};

#endif
