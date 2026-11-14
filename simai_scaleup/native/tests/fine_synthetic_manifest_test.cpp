#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#include "simai/runtime/fine_synthetic_manifest.hpp"

namespace {

using namespace simai::runtime;

int failures = 0;

void expect(bool condition, const std::string& name) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << name << '\n';
  }
}

std::string data_file(const std::string& name) {
  return std::string(SIMAI_TEST_DATA_DIR) + "/" + name;
}

void test_multigpu_manifest_loads() {
  const FineSyntheticExecutionProgram program =
      load_fine_synthetic_execution_manifest(
          data_file("synthetic_two_gpu.manifest"));
  expect(program.traces.size() == 2, "manifest loads two synthetic traces");
  expect(
      program.execution.nodes.size() == 2 &&
          program.execution.nodes[0].dependencies.empty() &&
          program.execution.nodes[1].dependencies.empty(),
      "independent synthetic kernels become concurrent graph roots");
  expect(
      program.execution.nodes[0].kernels[0].device.value() == 1 &&
          program.execution.nodes[1].kernels[0].device.value() == 2,
      "manifest preserves per-kernel issuing devices");
}

}  // namespace

int main() {
  test_multigpu_manifest_loads();
  if (failures != 0) {
    std::cerr << failures << " synthetic manifest test(s) failed\n";
    return 1;
  }
  std::cout << "All fine synthetic manifest tests passed\n";
  return 0;
}
