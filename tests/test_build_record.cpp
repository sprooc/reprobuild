#include <gtest/gtest.h>

#include <fstream>

#include "build_record.h"

class BuildRecordTest : public ::testing::Test {
 protected:
  void SetUp() override {
    openssl_pkg = DependencyPackage("openssl", "/usr/lib/libssl.so", "1.1.1w",
                                    "sha256:abc123");
    zlib_pkg =
        DependencyPackage("zlib", "/usr/lib/libz.so", "1.3", "sha256:def456");
    curl_pkg = DependencyPackage("curl", "/usr/lib/libcurl.so", "7.81.0",
                                 "sha256:ghi789");

    record = BuildRecord("test_project");
  }

  DependencyPackage openssl_pkg;
  DependencyPackage zlib_pkg;
  DependencyPackage curl_pkg;
  BuildRecord record;
};

TEST_F(BuildRecordTest, SaveAndLoadFile) {
  record.addDependency(openssl_pkg);
  record.addDependency(zlib_pkg);

  std::string filename = "/tmp/test_build_record.yaml";
  record.saveToFile(filename);

  BuildRecord loaded_record = BuildRecord::loadFromFile(filename);

  EXPECT_TRUE(record.matches(loaded_record));

  // Clean up
  std::remove(filename.c_str());
}
