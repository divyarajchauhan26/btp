#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "simai/runtime/fine_kernel_graph.hpp"

namespace {

using namespace simai::contracts;
using namespace simai::runtime;

int failures = 0;

void expect(bool condition, const std::string& name) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << name << '\n';
  }
}

void expect_invalid(
    const std::function<void()>& action, const std::string& name) {
  try {
    action();
    expect(false, name);
  } catch (const std::invalid_argument&) {
    expect(true, name);
  }
}

FineKernelGraph parse(const std::string& text) {
  std::istringstream input(text);
  return parse_fine_kernel_graph_sidecar(input);
}

void test_versioned_sidecar_and_topology() {
  const FineKernelGraph graph = parse(
      "schema_version=1\n"
      "node=1|forward|compute|1|compute.forward|-|0|-\n"
      "node=2|allreduce|collective|1|plan.allreduce|allreduce|256|1\n"
      "node=3|update|compute|2|compute.update|-|0|2\n");
  expect(graph.nodes.size() == 3, "sidecar imports all kernel nodes");
  const std::vector<WorkloadEventId> order = graph.topological_order();
  expect(
      order.size() == 3 && order[0] == WorkloadEventId(1) &&
          order[1] == WorkloadEventId(2) &&
          order[2] == WorkloadEventId(3),
      "kernel graph returns deterministic topological order");
  expect(
      graph.nodes[1].has_collective && graph.nodes[1].payload_bytes == 256,
      "collective node preserves kind and payload");
  expect(
      graph.nodes[2].device == DeviceId(2),
      "sidecar preserves per-node device/rank ownership");
}

void test_graph_errors() {
  expect_invalid(
      []() {
        parse(
            "schema_version=1\n"
            "node=1|a|compute|1|a|-|0|2\n");
      },
      "missing dependency is rejected");
  expect_invalid(
      []() {
        parse(
            "schema_version=1\n"
            "node=1|a|compute|1|a|-|0|2\n"
            "node=2|b|compute|1|b|-|0|1\n");
      },
      "dependency cycle is rejected");
  expect_invalid(
      []() {
        parse(
            "schema_version=2\n"
            "node=1|a|compute|1|a|-|0|-\n");
      },
      "unknown sidecar schema version is rejected");
}

}  // namespace

int main() {
  test_versioned_sidecar_and_topology();
  test_graph_errors();
  if (failures != 0) {
    std::cerr << failures << " fine kernel graph test(s) failed\n";
    return 1;
  }
  std::cout << "All fine kernel graph tests passed\n";
  return 0;
}
