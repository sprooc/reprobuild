#include <getopt.h>

#include <iostream>
#include <string>
#include <vector>

#include "logger.h"
#include "tracker.h"

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
  while ((c = getopt_long(argc, argv, "o:l:h", long_options, &option_index)) !=
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

  try {
    Logger::setLevel(LogLevel::INFO);
    Tracker tracker("demo_project");

    // Configure tracker with user-specified options
    tracker.setOutputFile(output_file);
    tracker.setLogDirectory(log_dir);


    BuildRecord record = tracker.trackBuild(build_command);

  } catch (const std::exception& e) {
    Logger::error("Error: " + std::string(e.what()));
    return 1;
  }

  Logger::info("Build completed.");
  return 0;
}