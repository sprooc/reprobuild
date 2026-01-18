#include <getopt.h>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "build_info.h"
#include "bundle.h"
#include "logger.h"
#include "postprocessor.h"
#include "preprocessor.h"
#include "tracker.h"
#include "utils.h"

void printUsage(const char* program_name) {
  std::cerr << (std::string("Usage: ") + program_name +
               " [OPTIONS] <command...>") << std::endl;
  std::cerr << "Options:" << std::endl;
  std::cerr <<
      "  -o, --output <file>    Output file for build record (default: "
      "build_record.yaml)" << std::endl;
  std::cerr <<
      "  -l, --logdir <dir>     Log directory for tracking files (default: "
      "./logs)" << std::endl;
  std::cerr <<
      "  -b, --bundle           Create a bundle from existing build record" << std::endl;
  std::cerr << "  -h, --help             Show this help message" << std::endl;
  std::cerr << std::string("Example: ") + program_name +
               " -o my_build.yaml -l /tmp/logs make clean all" << std::endl;
}

bool handleBundle(const std::string& record_path,
                  const std::string& bundle_path) {
  BuildRecord record;
  try {
    record = BuildRecord::loadFromFile(record_path);
  } catch (const std::exception& e) {
    Logger::error("Failed to load build record: " + std::string(e.what()));
    return false;
  }

  try {
    createBundle(record, bundle_path);
  } catch (const std::exception& e) {
    Logger::error("Failed to create bundle: " + std::string(e.what()));
    return false;
  }

  return true;
}

int main(int argc, char* argv[]) {
  std::string output_file = "build_record.yaml";
  std::string log_dir = "/tmp";
  bool bundle = false;

  // Parse command line options
  static struct option long_options[] = {{"output", required_argument, 0, 'o'},
                                         {"logdir", required_argument, 0, 'l'},
                                         {"bundle", no_argument, 0, 'b'},
                                         {"help", no_argument, 0, 'h'},
                                         {0, 0, 0, 0}};

  int c;
  int option_index = 0;
  while ((c = getopt_long(argc, argv, "o:l:bh", long_options, &option_index)) !=
         -1) {
    switch (c) {
      case 'o':
        output_file = optarg;
        break;
      case 'l':
        log_dir = optarg;
        break;
      case 'b':
        bundle = true;
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

  if (bundle) {
    std::string record_path = argv[optind];   // Input build record file
    std::string bundle_output = output_file;  // Output bundle file
    if (!handleBundle(record_path, bundle_output)) {
      return 1;
    }
    return 0;
  }

  // Collect build command from remaining arguments
  std::vector<std::string> build_command;
  for (int i = optind; i < argc; ++i) {
    build_command.push_back(argv[i]);
  }

  std::shared_ptr<BuildInfo> build_info = std::make_shared<BuildInfo>(
      Utils::joinCommand(build_command), output_file, log_dir);

  Logger::setLevel(LogLevel::INFO);
  Logger::setLevel();

  Preprocessor preprocessor(build_info);
  preprocessor.prepareBuildEnvironment();
  preprocessor.fixMakefile();

  build_info->fillBuildRecordMetadata();

  Tracker tracker(build_info);
  try {
    tracker.trackBuild();

  } catch (const std::exception& e) {
    Logger::error("Error: " + std::string(e.what()));
    return 1;
  }

  Postprocessor postprocessor(build_info);
  postprocessor.postprocess();

  build_info->build_record_.saveToFile(output_file);

  Logger::info("Build completed.");
  return 0;
}
