// #include <gtest/gtest.h>

// #include "logger.h"
// #include "tracker.h"

// class TrackerTest : public ::testing::Test {
//  protected:
//   void SetUp() override {
//     tracker = Tracker("test_project");
//     Logger::setLevel(LogLevel::ERROR);
//     tracker.setOutputFile("/home/sprooc/reprobuild/logs/build_record.yaml");
//     tracker.setLogDirectory("/home/sprooc/reprobuild/logs");
//   }

//   Tracker tracker;
// };

// TEST_F(TrackerTest, TrackBuild) {
//   tracker.trackBuild({"ls", "/usr/lib/"});
//   auto record = tracker.getLastBuildRecord();
//   EXPECT_EQ(record.getProjectName(), "test_project");
// }
