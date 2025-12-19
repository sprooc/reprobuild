#ifndef LOGGER_H
#define LOGGER_H

#include <iostream>
#include <string>

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

class Logger {
public:
    static void setLevel(LogLevel level);
    static void setLevel();
    static LogLevel getLevel();
    
    static void debug(const std::string& message);
    static void info(const std::string& message);
    static void warn(const std::string& message);
    static void error(const std::string& message);
    
private:
    static LogLevel current_level_;
    static void log(LogLevel level, const std::string& prefix, const std::string& message);
};

#endif // LOGGER_H
