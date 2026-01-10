#include <gtest/gtest.h>

#include <fstream>

#include "canonicalizer.h"

class CanonicalizerTest : public ::testing::Test {
 protected:
  void SetUp() override {}

  Canonicalizer canon;
};

TEST_F(CanonicalizerTest, ApplyRule) {

  canon.add_default_rules();

  auto input_expected = std::vector<std::pair<std::string, std::string>>{
      {"$(shell ls src bld include)", "$(shell ls src bld include | sort)"},
      {"$(wildcard *.c)", "$(sort $(wildcard *.c))"},
      {"$(wildcard src bld include)", "$(sort $(wildcard src bld include))"},
  };

  for (const auto& [input, expected] : input_expected) {
    EXPECT_EQ(canon.apply(input), expected);
  }
}

TEST_F(CanonicalizerTest, ApplyToFile) {
  // Create a temporary file
  std::string filename = "/tmp/test_canonicalizer.txt";
  {
    std::ofstream ofs(filename);
    ofs << "Sources: $(wildcard src/*.c src/*.cpp)\n";
    ofs << "Files: $(shell ls include bld)\n";
    ofs.close();
  }

  // Add rules
  canon.add_default_rules();

  // Apply to file
  canon.apply_to_file(filename);

  // Verify the content
  {
    std::ifstream ifs(filename);
    std::string line1, line2;
    std::getline(ifs, line1);
    std::getline(ifs, line2);
    ifs.close();

    EXPECT_EQ(line1, "Sources: $(sort $(wildcard src/*.c src/*.cpp))");
    EXPECT_EQ(line2, "Files: $(shell ls include bld | sort)");
  }

  // Clean up
  std::remove(filename.c_str());
}