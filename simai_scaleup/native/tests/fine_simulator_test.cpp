#include <iostream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "simai/runtime/fine_simulator.hpp"
#include "simai/runtime/mscclpp_plan.hpp"

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

FineSimulatorConfig config(std::uint32_t endpoints) {
  const LinkConfig link{
      400, 10, 32, 128, 16, 2, 0, 64, 0, 0, 0, 0, 0, 0, 0};
  return FineSimulatorConfig{
      FineGpuSchedulerConfig{
          endpoints, 2, 2, FineSchedulingPolicy::RoundRobin, 1},
      FineWavefrontEngineConfig{
          endpoints,
          2,
          2,
          1,
          1,
          FineInstructionGeneratorConfig{64, 2, BufferId(900)}},
      FineLocalMemorySystemConfig{
          endpoints,
          {FineLocalMemoryTierConfig{FineMemorySpace::L1, 1, 64, 64, 128},
           FineLocalMemoryTierConfig{FineMemorySpace::L2, 2, 32, 64, 128},
           FineLocalMemoryTierConfig{
               FineMemorySpace::LocalHbm, 5, 16, 64, 128}}},
      link,
      BufferId(901)};
}

void test_two_gpu_fine_vertical_runtime() {
  const MscclppPlan plan =
      parse_mscclpp_plan_file(data_file("mscclpp_two_gpu.json"));
  const std::vector<FineKernelDefinition> kernels =
      lower_mscclpp_fine_kernels(
          plan, 256, MscclppFineLoweringConfig{1, 64, 10});
  const FineSimulationReport report =
      FineScaleUpSimulator().run(kernels, config(2));
  expect(report.end_to_end_ns > 0, "fine vertical runtime advances simulated time");
  expect(
      report.issued_instructions == report.completed_instructions &&
          report.issued_instructions > 0,
      "fine vertical runtime conserves external instruction completions");
  expect(
      report.local_memory_requests == 4,
      "two local PUT-source cache lines are modeled per GPU");
  expect(
      report.local_memory_bytes == 256 && report.remote_memory_bytes == 256,
      "fine report conserves logical local and remote data bytes");
  expect(
      report.remote_requests == 6,
      "four remote Stores and two remote semaphore releases enter transport");
  expect(
      report.remote_command_pdus > 0 && report.remote_response_pdus == 0,
      "PUT/signal fixture produces one-way command traffic");
  expect(report.dispatches.size() == 2, "both GPU workgroups are reported");
  expect(report.diagnostics.empty(), "completed run has no blocked-state diagnostics");
  expect(
      format_fine_report(report).find("fine_schema_version=1") == 0,
      "fine report has a versioned text representation");
  expect(
      format_fine_report(report) == format_fine_report(
          FineScaleUpSimulator().run(kernels, config(2))),
      "fine vertical runtime is deterministic across repeated execution");
}

void test_four_gpu_conservation() {
  const MscclppPlan plan = parse_mscclpp_plan_file(
      data_file("mscclpp_four_gpu_allgather.json"));
  const std::vector<FineKernelDefinition> kernels =
      lower_mscclpp_fine_kernels(
          plan, 4096, MscclppFineLoweringConfig{1, 64, 1});
  const FineSimulationReport report =
      FineScaleUpSimulator().run(kernels, config(4));
  expect(
      report.local_memory_requests == 192 &&
          report.remote_requests == 192,
      "four-GPU runtime conserves 12 PUTs expanded into 192 cache lines");
  expect(
      report.local_memory_bytes == 12288 &&
          report.remote_memory_bytes == 12288,
      "four-GPU runtime conserves local and remote logical bytes");
  expect(
      report.issued_instructions == 384 &&
          report.completed_instructions == 384,
      "four-GPU cache-line Load/Store instructions complete exactly once");
  expect(
      report.dispatches.size() == 4 && report.remote_command_pdus == 192,
      "four-GPU runtime reports every workgroup and remote Store PDU");
}

void test_memory_owner_must_fit_simulator_domain() {
  const FineMemoryReference registers{
      DeviceId(1), FineMemorySpace::RegisterFile, BufferId(1), 0, 64, 1};
  const FineMemoryReference outside{
      DeviceId(3), FineMemorySpace::RemoteHbm, BufferId(2), 0, 64, 1};
  const FineKernelDefinition kernel{
      KernelId(20),
      DeviceId(1),
      FineKernelKind::Communication,
      {FineWorkgroupDefinition{
          WorkgroupId(1),
          ThreadBlockId(1),
          FineWorkgroupResources{1},
          {FineWavefrontProgram{
              WavefrontId(1),
              {FineGpuOperation::store(
                  GpuOperationId(1), registers, outside)}}}}}};
  expect_invalid(
      [&kernel]() { FineScaleUpSimulator().run({kernel}, config(2)); },
      "memory owner outside the simulator endpoint domain is rejected");
}

}  // namespace

int main() {
  test_two_gpu_fine_vertical_runtime();
  test_four_gpu_conservation();
  test_memory_owner_must_fit_simulator_domain();
  if (failures != 0) {
    std::cerr << failures << " fine simulator test(s) failed\n";
    return 1;
  }
  std::cout << "All fine simulator tests passed\n";
  return 0;
}
