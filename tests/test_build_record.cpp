#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>

#include "build_record.h"

class BuildRecordTest : public ::testing::Test {
 protected:
  void SetUp() override {
    openssl_pkg =
        DependencyPackage("openssl", DependencyOrigin::APT,
                          "/usr/lib/libssl.so", "1.1.1w", "sha256:abc123");
    zlib_pkg = DependencyPackage("zlib", DependencyOrigin::APT,
                                 "/usr/lib/libz.so", "1.3", "sha256:def456");
    curl_pkg =
        DependencyPackage("curl", DependencyOrigin::APT, "/usr/lib/libcurl.so",
                          "7.81.0", "sha256:ghi789");

    record = BuildRecord("");
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
  // std::remove(filename.c_str());
}

TEST_F(BuildRecordTest, SaveAndLoadPreservesMetadataArtifactsAndGitCommits) {
  record.setProjectName("reprobuild");
  record.addDependency(openssl_pkg);
  record.addArtifact(
      BuildArtifact("build/reprobuild", "sha256:artifact", "executable"));
  record.setArchitecture("x86_64");
  record.setDistribution("Ubuntu 25.04");
  record.setBuildPath("/tmp/project");
  record.setBuildTimestamp("2026-03-21T00:00:00+00:00");
  record.setHostname("builder");
  record.setLocale("C.UTF-8");
  record.setUmask("0022");
  record.setRandomSeed("123");
  record.setBuildCommand("make -j4");
  record.addGitCommitId("https://example.com/repo.git", "abc123");

  const std::string filename = "/tmp/test_build_record_round_trip.yaml";
  record.saveToFile(filename);

  BuildRecord loaded_record = BuildRecord::loadFromFile(filename);

  EXPECT_EQ(loaded_record.getProjectName(), "reprobuild");
  EXPECT_EQ(loaded_record.getDependencyCount(), 1U);
  EXPECT_TRUE(loaded_record.hasDependency("openssl"));
  EXPECT_EQ(loaded_record.getArtifacts().size(), 1U);
  EXPECT_EQ(loaded_record.getArtifacts()[0].path, "build/reprobuild");
  EXPECT_EQ(loaded_record.getArtifacts()[0].hash, "sha256:artifact");
  EXPECT_EQ(loaded_record.getArtifacts()[0].type, "executable");
  EXPECT_EQ(loaded_record.getArchitecture(), "x86_64");
  EXPECT_EQ(loaded_record.getDistribution(), "Ubuntu 25.04");
  EXPECT_EQ(loaded_record.getBuildPath(), "/tmp/project");
  EXPECT_EQ(loaded_record.getBuildTimestamp(), "2026-03-21T00:00:00+00:00");
  EXPECT_EQ(loaded_record.getHostname(), "builder");
  EXPECT_EQ(loaded_record.getLocale(), "C.UTF-8");
  EXPECT_EQ(loaded_record.getUmask(), "0022");
  EXPECT_EQ(loaded_record.getRandomSeed(), "123");
  EXPECT_EQ(loaded_record.getBuildCommand(), "make -j4");
  ASSERT_EQ(loaded_record.getGitCommitIds().size(), 1U);
  EXPECT_EQ(loaded_record.getGitCommitIds()[0].first,
            "https://example.com/repo.git");
  EXPECT_EQ(loaded_record.getGitCommitIds()[0].second, "abc123");

  std::remove(filename.c_str());
}

TEST_F(BuildRecordTest, RemoveDependencyAndClearArtifactsUpdateState) {
  record.addDependency(openssl_pkg);
  record.addDependency(zlib_pkg);
  record.addArtifact(
      BuildArtifact("build/libdemo.so", "sha256:demo", "shared_library"));

  record.removeDependency("openssl");
  record.clearArtifacts();

  EXPECT_FALSE(record.hasDependency("openssl"));
  EXPECT_TRUE(record.hasDependency("zlib"));
  EXPECT_EQ(record.getDependencyCount(), 1U);
  EXPECT_TRUE(record.getArtifacts().empty());
}
