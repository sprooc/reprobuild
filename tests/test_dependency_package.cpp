#include <gtest/gtest.h>

#include <iostream>
#include <sstream>

#include "dependency_package.h"
#include "build_info.h"
#include "utils.h"

class DependencyPackageTest : public ::testing::Test {
 protected:
  void SetUp() override {}

  DependencyPackage valid_package;
};

TEST_F(DependencyPackageTest, FromRawFileAPT) {

  if (Utils::checkPackageManager() != PackageMgr::APT) {
    GTEST_SKIP() << "Skipping test: APT package manager not detected.";
  }

  DependencyPackage pkg =
      DependencyPackage::fromRawFile("/lib/x86_64-linux-gnu/libc.so.6", PackageMgr::APT);

  EXPECT_EQ(pkg.getPackageName(), "libc6");
  EXPECT_EQ(pkg.getOriginalPath(), "/usr/lib/x86_64-linux-gnu/libc.so.6");
  EXPECT_EQ(pkg.getVersion(), "2.41-6ubuntu1.2");
  EXPECT_EQ(pkg.getHashValue(),
            "1b839cf653ce393601ab7cc2910d6b9f8e91d421ebf41c277147cf88f19b0afe");
}

TEST_F(DependencyPackageTest, FromRawFileRPM) {
  
  if (Utils::checkPackageManager() != PackageMgr::DNF) {
    GTEST_SKIP() << "Skipping test: DNF package manager not detected.";
  }

  DependencyPackage pkg =
      DependencyPackage::fromRawFile("/usr/lib64/libc.so.6", PackageMgr::DNF);

  EXPECT_EQ(pkg.getPackageName(), "glibc");
  EXPECT_EQ(pkg.getOriginalPath(), "/usr/lib64/libc.so.6");
  EXPECT_EQ(pkg.getVersion(), "2.39-38.fc40");
  EXPECT_EQ(pkg.getHashValue(),
            "069a4451877087e91a970a07ad34ad523c07488df75382cfd540b35f5039095a");
}
