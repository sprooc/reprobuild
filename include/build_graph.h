#ifndef BUILD_GRAPH_H
#define BUILD_GRAPH_H

#include <map>
#include <string>
#include <unordered_set>
#include <vector>

enum class BuildNodeType {
  SOURCE,
  INTERMEDIATE,
  ARTIFACT,
  UNKNOWN,
};

struct BuildNode {
  std::string path;
  std::string hash;
  BuildNodeType type = BuildNodeType::UNKNOWN;
};

struct BuildEdge {
  std::string command;
  std::string command_path;
  std::vector<std::string> inputs;
  std::string output;
  std::vector<std::string> args;
  int pid = -1;
};

class BuildGraph {
 public:
  void addNode(const BuildNode& node);
  void addNode(BuildNode&& node);
  void addEdge(const BuildEdge& edge);
  void addEdge(BuildEdge&& edge);
  bool hasNode(const std::string& path) const;

  const std::map<std::string, BuildNode>& getNodes() const { return nodes_; }
  const std::vector<BuildEdge>& getEdges() const { return edges_; }

  size_t nodeCount() const { return nodes_.size(); }
  size_t edgeCount() const { return edges_.size(); }

  void pruneGraph(const std::unordered_set<std::string>& roots);

  void saveToFile(const std::string& filepath) const;

 private:
  std::map<std::string, BuildNode> nodes_;
  std::vector<BuildEdge> edges_;
};

#endif  // BUILD_GRAPH_H
