#ifndef CANONICALIZER_H
#define CANONICALIZER_H

#include <regex>
#include <string>
#include <vector>

class Canonicalizer {
 public:

  void add_rule(const std::string& pattern, const std::string& replacement,
                std::regex::flag_type flags = std::regex::ECMAScript);

  std::string apply(const std::string& input) const;
  void apply_to_file(const std::string& filename) const;
  void add_default_rules();

 private:
  struct Rule {
    std::regex pattern;
    std::string replacement;
  };

  std::vector<Rule> rules_;
};

#endif