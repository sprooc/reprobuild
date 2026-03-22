#include <gtest/gtest.h>

#include <unordered_set>

#include "build_graph.h"

TEST(BuildGraphTest, PruneGraphKeepsReachableHighestPriorityEdges) {
  BuildGraph graph;

  graph.addNode({"src/a.cpp", "hash-src", BuildNodeType::SOURCE});
  graph.addNode({"build/a.o", "hash-obj", BuildNodeType::INTERMEDIATE});
  graph.addNode({"libfoo.a", "hash-lib", BuildNodeType::INTERMEDIATE});
  graph.addNode({"foo", "hash-bin", BuildNodeType::ARTIFACT});
  graph.addNode({"src/unused.cpp", "hash-unused-src", BuildNodeType::SOURCE});
  graph.addNode({"unused.o", "hash-unused-obj", BuildNodeType::INTERMEDIATE});

  graph.addEdge({"c++", "/usr/bin/c++", {"src/a.cpp"}, "build/a.o",
                 {"-c", "src/a.cpp", "-o", "build/a.o"}, 101});
  graph.addEdge({"ar", "/usr/bin/ar", {"build/a.o"}, "libfoo.a",
                 {"rcs", "libfoo.a", "build/a.o"}, 102});
  graph.addEdge({"ld", "/usr/bin/ld", {"libfoo.a"}, "foo",
                 {"libfoo.a", "-o", "foo"}, 103});
  graph.addEdge({"c++", "/usr/bin/c++", {"libfoo.a"}, "foo",
                 {"libfoo.a", "-o", "foo"}, 104});
  graph.addEdge({"c++", "/usr/bin/c++", {"src/unused.cpp"}, "unused.o",
                 {"-c", "src/unused.cpp", "-o", "unused.o"}, 105});

  graph.pruneGraph(std::unordered_set<std::string>{"foo"});

  ASSERT_EQ(graph.edgeCount(), 3U);
  ASSERT_EQ(graph.nodeCount(), 4U);
  EXPECT_TRUE(graph.hasNode("src/a.cpp"));
  EXPECT_TRUE(graph.hasNode("build/a.o"));
  EXPECT_TRUE(graph.hasNode("libfoo.a"));
  EXPECT_TRUE(graph.hasNode("foo"));
  EXPECT_FALSE(graph.hasNode("src/unused.cpp"));
  EXPECT_FALSE(graph.hasNode("unused.o"));

  int foo_edge_count = 0;
  for (const auto& edge : graph.getEdges()) {
    if (edge.output == "foo") {
      ++foo_edge_count;
      EXPECT_EQ(edge.command, "c++");
    }
  }
  EXPECT_EQ(foo_edge_count, 1);
}
