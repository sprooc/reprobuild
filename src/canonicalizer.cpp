#include "canonicalizer.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace {

static inline void ltrim_inplace(std::string& s) {
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
            return !std::isspace(ch);
          }));
}

static inline void rtrim_inplace(std::string& s) {
  s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
          }).base(),
          s.end());
}

static inline std::string trim_copy(std::string s) {
  ltrim_inplace(s);
  rtrim_inplace(s);
  return s;
}

// Find the matching ')' for a Make expression starting at "$(".
// Returns npos if no match.
static size_t find_matching_paren(const std::string& s, size_t dollar_lparen_pos) {
  // Expect s[dollar_lparen_pos..] starts with "$(".
  if (dollar_lparen_pos + 1 >= s.size() || s[dollar_lparen_pos] != '$' ||
      s[dollar_lparen_pos + 1] != '(') {
    return std::string::npos;
  }

  size_t i = dollar_lparen_pos + 2;
  int depth = 1;
  while (i < s.size()) {
    if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '(') {
      depth++;
      i += 2;
      continue;
    }
    if (s[i] == ')') {
      depth--;
      if (depth == 0) {
        return i;
      }
    }
    i++;
  }
  return std::string::npos;
}

static std::string canonicalize_make_text(const std::string& text, const std::string& parent_fn);

static void split_make_func(const std::string& inner, std::string& fn, std::string& args) {
  // inner: "wildcard ..." or "shell ls ..." etc.
  // function name runs until first whitespace (or end).
  size_t pos = 0;
  while (pos < inner.size() && std::isspace(static_cast<unsigned char>(inner[pos]))) {
    pos++;
  }
  size_t start = pos;
  while (pos < inner.size() && !std::isspace(static_cast<unsigned char>(inner[pos]))) {
    pos++;
  }
  fn = inner.substr(start, pos - start);
  args = (pos < inner.size()) ? inner.substr(pos) : std::string{};
  // Preserve original spacing in args as much as possible.
}

static std::string maybe_wrap_wildcard_with_sort(const std::string& args_processed,
                                                 const std::string& parent_fn) {
  // If wildcard is already inside a sort(), the parent sort will sort its output.
  // Avoid redundant nesting: $(sort $(sort $(wildcard ...)))
  if (parent_fn == "sort") {
    return "$(wildcard" + args_processed + ")";
  }
  return "$(sort $(wildcard" + args_processed + "))";
}

static std::string maybe_sort_shell_ls(const std::string& args_processed) {
  // Handle: $(shell ls ...)
  // Do a conservative check: only transform if it starts with "ls" after trimming,
  // and doesn't already include a pipe to sort.
  std::string trimmed = trim_copy(args_processed);
  if (trimmed.rfind("ls", 0) != 0) {
    return "$(shell" + args_processed + ")";
  }
  if (args_processed.find("| sort") != std::string::npos) {
    return "$(shell" + args_processed + ")";
  }
  return "$(shell" + args_processed + " | sort)";
}

static std::string canonicalize_make_expr(const std::string& expr_inner,
                                          const std::string& parent_fn) {
  std::string fn;
  std::string args;
  split_make_func(expr_inner, fn, args);

  // Recursively canonicalize nested $(...) in args.
  std::string args_processed = canonicalize_make_text(args, fn);

  if (fn == "wildcard") {
    return maybe_wrap_wildcard_with_sort(args_processed, parent_fn);
  }
  if (fn == "shell") {
    return maybe_sort_shell_ls(args_processed);
  }

  // Default: rebuild expression with canonicalized args.
  return "$(" + fn + args_processed + ")";
}

static std::string canonicalize_make_text(const std::string& text, const std::string& parent_fn) {
  std::string out;
  out.reserve(text.size());

  size_t i = 0;
  while (i < text.size()) {
    size_t pos = text.find("$(", i);
    if (pos == std::string::npos) {
      out.append(text, i, std::string::npos);
      break;
    }

    out.append(text, i, pos - i);

    size_t end = find_matching_paren(text, pos);
    if (end == std::string::npos) {
      // Unbalanced: leave the rest unchanged.
      out.append(text, pos, std::string::npos);
      break;
    }

    // inner content without outer "$(" and ")".
    const std::string inner = text.substr(pos + 2, end - (pos + 2));
    out += canonicalize_make_expr(inner, parent_fn);
    i = end + 1;
  }

  return out;
}

}  // namespace

void Canonicalizer::add_rule(const std::string& pattern,
                             const std::string& replacement,
                             std::regex::flag_type flags) {
  Rule r;
  r.kind = Rule::Kind::RegexReplace;
  r.pattern = std::regex(pattern, flags);
  r.replacement = replacement;
  rules_.push_back(std::move(r));
}

std::string Canonicalizer::apply(const std::string& input) const {
  std::string result = input;
  for (const auto& rule : rules_) {
    switch (rule.kind) {
      case Rule::Kind::RegexReplace:
        result = std::regex_replace(result, rule.pattern, rule.replacement);
        break;
      case Rule::Kind::MakefileDefaults:
        // Parent function name is empty at top-level.
        result = canonicalize_make_text(result, "");
        break;
    }
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
  // Parenthesis-aware canonicalization for Makefile-style expressions.
  Rule r;
  r.kind = Rule::Kind::MakefileDefaults;
  rules_.push_back(std::move(r));
}