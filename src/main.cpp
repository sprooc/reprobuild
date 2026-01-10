#include <getopt.h>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "build_info.h"
#include "logger.h"
#include "preprocessor.h"
#include "tracker.h"
#include "utils.h"

void printUsage(const char* program_name) {
  Logger::warn(std::string("Usage: ") + program_name +
               " [OPTIONS] <command...>");
  Logger::warn("Options:");
  Logger::warn(
      "  -o, --output <file>    Output file for build record (default: "
      "build_record.yaml)");
  Logger::warn(
      "  -l, --logdir <dir>     Log directory for tracking files (default: "
      "./logs)");
  Logger::warn("  -h, --help             Show this help message");
  Logger::warn(std::string("Example: ") + program_name +
               " -o my_build.yaml -l /tmp/logs make clean all");
}

int main(int argc, char* argv[]) {
  std::string output_file = "build_record.yaml";
  std::string log_dir = "/tmp";

  // Parse command line options
  static struct option long_options[] = {{"output", required_argument, 0, 'o'},
                                         {"logdir", required_argument, 0, 'l'},
                                         {"help", no_argument, 0, 'h'},
                                         {0, 0, 0, 0}};

  int c;
  int option_index = 0;
  while ((c = getopt_long(argc, argv, "+o:l:h", long_options, &option_index)) !=
         -1) {
    switch (c) {
      case 'o':
        output_file = optarg;
        break;
      case 'l':
        log_dir = optarg;
        break;
      case 'h':
        printUsage(argv[0]);
        return 0;
      case '?':
        printUsage(argv[0]);
        return 1;
      default:
        break;
    }
  }

  // Check if we have build command arguments
  if (optind >= argc) {
    printUsage(argv[0]);
    return 1;
  }

  // Collect build command from remaining arguments
  std::vector<std::string> build_command;
  for (int i = optind; i < argc; ++i) {
    build_command.push_back(argv[i]);
  }

  std::shared_ptr<BuildInfo> build_info =
      std::make_shared<BuildInfo>(Utils::joinCommand(build_command), output_file, log_dir);

  Logger::setLevel(LogLevel::INFO);
  Logger::setLevel();

  Preprocessor preprocessor(build_info);
  preprocessor.prepareBuildEnvironment();

  build_info->fillBuildRecordMetadata();

  Tracker tracker(build_info);
  try {
    tracker.trackBuild();

  } catch (const std::exception& e) {
    Logger::error("Error: " + std::string(e.what()));
    return 1;
  }

  build_info->build_record_.saveToFile(output_file);

  Logger::info("Build completed.");
  return 0;
}
