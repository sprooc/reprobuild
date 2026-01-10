#include "canonicalizer.h"

#include <fstream>

void Canonicalizer::add_rule(const std::string& pattern,
                             const std::string& replacement,
                             std::regex::flag_type flags) {
  rules_.push_back(Rule{std::regex(pattern, flags), replacement});
}

std::string Canonicalizer::apply(const std::string& input) const {
  std::string result = input;
  for (const auto& rule : rules_) {
    result = std::regex_replace(result, rule.pattern, rule.replacement);
  }
  return result;
}

void Canonicalizer::apply_to_file(const std::string& filename) const {
  // Read file content
  std::ifstream file_in(filename);
  if (!file_in.is_open()) {
    throw std::runtime_error("Failed to open file: " + filename);
  }

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(file_in, line)) {
    lines.push_back(apply(line));
  }
  file_in.close();

  // Write back modified content
  std::ofstream file_out(filename);
  if (!file_out.is_open()) {
    throw std::runtime_error("Failed to open file for writing: " + filename);
  }
  for (const auto& modified_line : lines) {
    file_out << modified_line << "\n";
  }
  file_out.close();
}

void Canonicalizer::add_default_rules() {
  // Example default rules
  add_rule(R"(\$\(wildcard\s+([^)]*)\))", R"($(sort $(wildcard $1)))");
  add_rule(R"(\$\(shell\s+ls([^)]*)\))", R"($(shell ls$1 | sort))");
}