#include "logger.h"

LogLevel Logger::current_level_ = LogLevel::INFO;

void Logger::setLevel(LogLevel level) { current_level_ = level; }

// From environment variable LOG_LEVEL
void Logger::setLevel() {
  const char* env_level = std::getenv("LOG_LEVEL");
  if (env_level) {
    std::string level_str(env_level);
    if (level_str == "DEBUG") {
      current_level_ = LogLevel::DEBUG;
    } else if (level_str == "INFO") {
      current_level_ = LogLevel::INFO;
    } else if (level_str == "WARN") {
      current_level_ = LogLevel::WARN;
    } else if (level_str == "ERROR") {
      current_level_ = LogLevel::ERROR;
    }
  }
}

LogLevel Logger::getLevel() { return current_level_; }

void Logger::debug(const std::string& message) {
  log(LogLevel::DEBUG, "[DEBUG]", message);
}

void Logger::info(const std::string& message) {
  log(LogLevel::INFO, "[INFO]", message);
}

void Logger::warn(const std::string& message) {
  log(LogLevel::WARN, "[WARN]", message);
}

void Logger::error(const std::string& message) {
  log(LogLevel::ERROR, "[ERROR]", message);
}

void Logger::log(LogLevel level, const std::string& prefix,
                 const std::string& message) {
  if (level >= current_level_) {
    if (level == LogLevel::ERROR || level == LogLevel::WARN) {
      std::cerr << prefix << " " << message << std::endl;
    } else {
      std::cout << prefix << " " << message << std::endl;
    }
  }
}
