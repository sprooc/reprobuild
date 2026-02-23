#include "build_graph.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

void BuildGraph::addNode(const BuildNode& node) {
  nodes_.emplace(node.path, node);
}

void BuildGraph::addEdge(const BuildEdge& edge) { edges_.push_back(edge); }

bool BuildGraph::hasNode(const std::string& path) const {
  return nodes_.find(path) != nodes_.end();
}

namespace {

std::string nodeTypeToString(BuildNodeType type) {
  switch (type) {
    case BuildNodeType::SOURCE:
      return "source";
    case BuildNodeType::INTERMEDIATE:
      return "intermediate";
    case BuildNodeType::ARTIFACT:
      return "artifact";
    default:
      return "unknown";
  }
}

}  // namespace

void BuildGraph::pruneGraph(const std::unordered_set<std::string>& roots) {
  static const std::unordered_map<std::string, int> kToolPriority = {
      {"gcc", 30},     {"g++", 30},     {"cc", 30},     {"c++", 30},
      {"clang", 30},   {"clang++", 30}, {"ar", 20},     {"ranlib", 20},
      {"libtool", 20}, {"ld", 10},      {"ld.bfd", 10}, {"ld.gold", 10},
      {"ld.lld", 10},  {"as", 5},       {"objcopy", 5}, {"strip", 5},
  };
  auto priority = [&](const std::string& cmd) -> int {
    auto it = kToolPriority.find(cmd);
    return it != kToolPriority.end() ? it->second : 0;
  };

  // per output, keep only the highest-priority edge
  std::unordered_map<std::string, size_t> best;
  for (size_t i = 0; i < edges_.size(); ++i) {
    const std::string& out = edges_[i].output;
    if (out.empty()) continue;
    auto it = best.find(out);
    if (it == best.end() ||
        priority(edges_[i].command) > priority(edges_[it->second].command)) {
      best[out] = i;
    }
  }
  std::unordered_set<size_t> kept;
  for (const auto& [out, idx] : best) kept.insert(idx);

  std::vector<BuildEdge> deduped;
  deduped.reserve(kept.size());
  for (size_t i = 0; i < edges_.size(); ++i)
    if (kept.count(i)) deduped.push_back(std::move(edges_[i]));
  edges_ = std::move(deduped);

  // reverse-BFS from roots (roots contains basenames)
  if (!roots.empty()) {
    std::unordered_map<std::string, std::vector<size_t>> out_to_edges;
    for (size_t i = 0; i < edges_.size(); ++i)
      if (!edges_[i].output.empty())
        out_to_edges[edges_[i].output].push_back(i);

    // Expand basenames to the full output paths actually stored in edges.
    std::unordered_set<std::string> seed_paths;
    for (const auto& [out, _] : out_to_edges) {
      if (roots.count(std::filesystem::path(out).filename().string()))
        seed_paths.insert(out);
    }

    std::unordered_set<size_t> reachable;
    std::unordered_set<std::string> visited(seed_paths.begin(),
                                            seed_paths.end());
    std::vector<std::string> frontier(seed_paths.begin(), seed_paths.end());

    while (!frontier.empty()) {
      std::vector<std::string> next;
      for (const auto& node : frontier) {
        auto it = out_to_edges.find(node);
        if (it == out_to_edges.end()) continue;
        for (size_t idx : it->second) {
          if (reachable.insert(idx).second) {
            for (const auto& inp : edges_[idx].inputs)
              if (visited.insert(inp).second) next.push_back(inp);
          }
        }
      }
      frontier = std::move(next);
    }

    std::vector<BuildEdge> pruned;
    pruned.reserve(reachable.size());
    for (size_t i = 0; i < edges_.size(); ++i)
      if (reachable.count(i)) pruned.push_back(std::move(edges_[i]));
    edges_ = std::move(pruned);
  }

  // remove unreferenced nodes
  std::unordered_set<std::string> referenced;
  for (const auto& e : edges_) {
    for (const auto& inp : e.inputs) referenced.insert(inp);
    if (!e.output.empty()) referenced.insert(e.output);
  }
  for (auto it = nodes_.begin(); it != nodes_.end();)
    it = referenced.count(it->first) ? std::next(it) : nodes_.erase(it);
}

void BuildGraph::saveToFile(const std::string& filepath) const {
  YAML::Node root;

  // --- nodes ---
  YAML::Node nodes_node;
  for (const auto& [path, node] : nodes_) {
    YAML::Node n;
    n["path"] = node.path;
    n["type"] = nodeTypeToString(node.type);
    n["hash"] = node.hash;
    nodes_node.push_back(n);
  }
  root["nodes"] = nodes_node;

  // --- edges ---
  YAML::Node edges_node;
  for (const auto& edge : edges_) {
    YAML::Node e;
    e["command"] = edge.command;
    e["command_path"] = edge.command_path;
    e["pid"] = edge.pid;

    YAML::Node inputs_node;
    for (const auto& inp : edge.inputs) {
      inputs_node.push_back(inp);
    }
    e["inputs"] = inputs_node;

    e["output"] = edge.output;

    std::string args_concat;
    for (const auto& arg : edge.args) {
      args_concat += arg + " ";
    }
    e["args"] = args_concat;

    edges_node.push_back(e);
  }
  root["edges"] = edges_node;

  std::ofstream out(filepath);
  if (!out.is_open()) {
    throw std::runtime_error("Cannot open build graph output file: " +
                             filepath);
  }
  out << root;
}
