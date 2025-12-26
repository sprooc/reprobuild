#include <gtest/gtest.h>

#include <iostream>
#include <sstream>

#include "dependency_package.h"

class DependencyPackageTest : public ::testing::Test {
 protected:
  void SetUp() override {}

  DependencyPackage valid_package;
};

TEST_F(DependencyPackageTest, FromRawFileValid) {
  DependencyPackage pkg =
      DependencyPackage::fromRawFile("/lib/x86_64-linux-gnu/libc.so.6");

  EXPECT_EQ(pkg.getPackageName(), "libc6");
  EXPECT_EQ(pkg.getOriginalPath(), "/lib/x86_64-linux-gnu/libc.so.6");
  EXPECT_EQ(pkg.getVersion(), "2.41-6ubuntu1.2");
  EXPECT_EQ(pkg.getHashValue(),
            "1b839cf653ce393601ab7cc2910d6b9f8e91d421ebf41c277147cf88f19b0afe");
}
