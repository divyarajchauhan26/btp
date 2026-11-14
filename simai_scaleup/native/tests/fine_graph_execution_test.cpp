#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "simai/runtime/fine_graph_execution.hpp"
#include "simai/runtime/fine_simulator.hpp"

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

std::string data_file(const std::string& name) {
  return std::string(SIMAI_TEST_DATA_DIR) + "/" + name;
}

FineKernelGraph graph() {
  std::istringstream input(
      "schema_version=1\n"
      "node=1|rank0_pre|compute|1|compute.pre|-|0|-\n"
      "node=2|rank1_pre|compute|2|compute.pre|-|0|-\n"
      "node=3|rank0_allreduce|collective|1|collective.ar|allreduce|256|1\n"
      "node=4|rank1_allreduce|collective|2|collective.ar|allreduce|256|2\n"
      "node=5|rank0_post|compute|1|compute.post|-|0|3\n"
      "node=6|rank1_post|compute|2|compute.post|-|0|4\n");
  return parse_fine_kernel_graph_sidecar(input);
}

FineComputeKernelTemplate compute_template(std::uint64_t cycles) {
  return FineComputeKernelTemplate{
      KernelId(1),
      DeviceId(1),
      1,
      1,
      1,
      {FineComputeOperationTemplate{
          FineGpuOperation::reduce(GpuOperationId(1), cycles), 0, 0}}};
}

FineSimulatorConfig config() {
  const LinkConfig link{
      400, 10, 32, 128, 16, 2, 0, 64, 0, 0, 0, 0, 0, 0, 0};
  return FineSimulatorConfig{
      FineGpuSchedulerConfig{
          2, 2, 2, FineSchedulingPolicy::RoundRobin, 1},
      FineWavefrontEngineConfig{
          2,
          2,
          2,
          1,
          1,
          FineInstructionGeneratorConfig{64, 2, BufferId(900)}},
      FineLocalMemorySystemConfig{
          2,
          {FineLocalMemoryTierConfig{FineMemorySpace::L1, 1, 64, 64, 128},
           FineLocalMemoryTierConfig{FineMemorySpace::L2, 2, 32, 64, 128},
           FineLocalMemoryTierConfig{
               FineMemorySpace::LocalHbm, 5, 16, 64, 128}}},
      link,
      BufferId(901)};
}

std::vector<FineGraphBinding> bindings() {
  const MscclppPlan plan =
      parse_mscclpp_plan_file(data_file("mscclpp_two_gpu.json"));
  return {
      FineGraphBinding::compute_kernel(
          "rank0_pre", WorkloadEventId(1), compute_template(10)),
      FineGraphBinding::compute_kernel(
          "rank1_pre", WorkloadEventId(2), compute_template(10)),
      FineGraphBinding::collective(
          "allreduce",
          {WorkloadEventId(3), WorkloadEventId(4)},
          plan,
          256,
          MscclppFineLoweringConfig{1, 64, 4}),
      FineGraphBinding::compute_kernel(
          "rank0_post", WorkloadEventId(5), compute_template(5)),
      FineGraphBinding::compute_kernel(
          "rank1_post", WorkloadEventId(6), compute_template(5))};
}

void test_graph_lowering_and_dependency_execution() {
  const FineGraphExecutionPlan plan =
      lower_fine_kernel_graph(graph(), bindings());
  expect(plan.nodes.size() == 5, "rank collective nodes collapse into one launch");
  expect(
      plan.nodes[2].source_nodes.size() == 2 &&
          plan.nodes[2].kernels.size() == 2 &&
          plan.nodes[2].dependencies.size() == 2,
      "collective launch preserves participants, kernels, and dependencies");
  expect(
      plan.nodes[3].dependencies.size() == 1 &&
          plan.nodes[3].dependencies[0] == WorkloadEventId(3),
      "post-compute dependency targets collapsed collective launch");

  const FineSimulationReport report = FineScaleUpSimulator().run(plan, config());
  expect(report.graph_nodes.size() == 5, "every execution node is reported");
  expect(
      report.graph_nodes[2].start_ns >= report.graph_nodes[0].completion_ns &&
          report.graph_nodes[2].start_ns >= report.graph_nodes[1].completion_ns,
      "collective starts only after both rank-local predecessors complete");
  expect(
      report.graph_nodes[3].ready_ns == report.graph_nodes[2].completion_ns &&
          report.graph_nodes[4].ready_ns == report.graph_nodes[2].completion_ns,
      "collective completion releases both rank-local successors");
  expect(
      report.graph_nodes[3].start_ns >= report.graph_nodes[3].ready_ns &&
          report.graph_nodes[4].start_ns >= report.graph_nodes[4].ready_ns,
      "successor kernels never start before graph readiness");
  expect(report.dispatches.size() == 6, "all compute and collective workgroups run");
  expect(
      report.issued_instructions == report.completed_instructions,
      "graph-driven runtime conserves instruction completions");
  expect(
      report.remote_requests == 6,
      "graph-driven collective reaches the semantic remote transport");
}

void test_missing_and_mismatched_bindings_fail() {
  std::vector<FineGraphBinding> incomplete = bindings();
  incomplete.pop_back();
  expect_invalid(
      [&incomplete]() { lower_fine_kernel_graph(graph(), incomplete); },
      "unbound executable graph node is rejected");

  std::vector<FineGraphBinding> mismatch = bindings();
  mismatch[2].source_nodes.pop_back();
  expect_invalid(
      [&mismatch]() { lower_fine_kernel_graph(graph(), mismatch); },
      "collective source participants must match the plan");
}

void test_prelowered_trace_kernel_binding() {
  std::vector<FineGraphBinding> traced_bindings = bindings();
  const FineKernelDefinition traced = lower_compute_kernel(compute_template(7));
  traced_bindings[0] = FineGraphBinding::traced(
      "rank0_pre_trace", WorkloadEventId(1), traced);
  const FineGraphExecutionPlan plan =
      lower_fine_kernel_graph(graph(), traced_bindings);
  expect(
      plan.nodes[0].kernels.size() == 1 &&
          plan.nodes[0].kernels[0].workgroups[0].wavefronts[0]
                  .operations[0]
                  .reduce_cycles == 7,
      "pre-lowered trace kernel binds without template reconstruction");

  FineKernelDefinition wrong_device = traced;
  wrong_device.device = DeviceId(2);
  traced_bindings[0] = FineGraphBinding::traced(
      "wrong_device", WorkloadEventId(1), wrong_device);
  expect_invalid(
      [&traced_bindings]() {
        lower_fine_kernel_graph(graph(), traced_bindings);
      },
      "traced kernel must match its Chakra node device");
}

}  // namespace

int main() {
  test_graph_lowering_and_dependency_execution();
  test_missing_and_mismatched_bindings_fail();
  test_prelowered_trace_kernel_binding();
  if (failures != 0) {
    std::cerr << failures << " fine graph execution test(s) failed\n";
    return 1;
  }
  std::cout << "All fine graph execution tests passed\n";
  return 0;
}
