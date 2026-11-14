#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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

void test_two_gpu_semantic_lowering() {
  const MscclppPlan plan =
      parse_mscclpp_plan_file(data_file("mscclpp_two_gpu.json"));
  const std::vector<FineKernelDefinition> kernels =
      lower_mscclpp_fine_kernels(
          plan, 2048, MscclppFineLoweringConfig{1, 128, 50});
  expect(kernels.size() == 2, "two-GPU plan creates one kernel per GPU");
  for (std::size_t index = 0; index < kernels.size(); ++index) {
    const FineKernelDefinition& kernel = kernels[index];
    expect(
        kernel.device == DeviceId(index + 1) &&
            kernel.kind == FineKernelKind::Communication,
        "fine communication kernel preserves GPU ownership");
    expect(
        kernel.workgroups.size() == 1 &&
            kernel.workgroups[0].wavefronts.size() == 1,
        "MSCCL++ threadblock lowers to one ordered workgroup/wavefront");
    const std::vector<FineGpuOperation>& operations =
        kernel.workgroups[0].wavefronts[0].operations;
    expect(operations.size() == 4, "threadblock operation count is preserved");
    if (operations.size() != 4) continue;
    expect(
        operations[0].kind == FineGpuOperationKind::Memcpy &&
            operations[0].source.space == FineMemorySpace::LocalHbm &&
            operations[0].destination.space == FineMemorySpace::RemoteHbm &&
            operations[0].source.length_bytes == 1024,
        "PUT lowers to cache-line-expandable local-to-remote Memcpy");
    expect(
        operations[1].kind == FineGpuOperationKind::SemaphoreRelease &&
            operations[1].semaphore_owner != kernel.device,
        "SIGNAL lowers to remote semaphore release");
    expect(
        operations[2].kind == FineGpuOperationKind::SemaphoreAcquire &&
            operations[2].semaphore_owner == kernel.device,
        "WAIT lowers to local-owner semaphore acquire");
    expect(
        operations[1].semaphore_generation == 1 &&
            operations[2].semaphore_generation == 1,
        "matching SIGNAL/WAIT preserve semaphore generation");
    expect(
        operations[3].kind == FineGpuOperationKind::Reduce &&
            operations[3].reduce_cycles == 50,
        "REDUCE lowers to configured arithmetic duration");
  }
}

void test_four_gpu_put_counts_and_bytes() {
  const MscclppPlan plan = parse_mscclpp_plan_file(
      data_file("mscclpp_four_gpu_allgather.json"));
  const std::vector<FineKernelDefinition> kernels =
      lower_mscclpp_fine_kernels(
          plan, 4096, MscclppFineLoweringConfig{1, 128, 25});
  expect(kernels.size() == 4, "four-GPU plan creates four kernels");
  std::size_t operations = 0;
  std::uint64_t bytes = 0;
  for (const FineKernelDefinition& kernel : kernels) {
    kernel.require_valid();
    for (const FineWorkgroupDefinition& workgroup : kernel.workgroups) {
      for (const FineWavefrontProgram& wavefront : workgroup.wavefronts) {
        for (const FineGpuOperation& operation : wavefront.operations) {
          ++operations;
          bytes += operation.source.length_bytes;
          expect(
              operation.kind == FineGpuOperationKind::Memcpy &&
                  operation.destination.owner != kernel.device,
              "four-GPU PUT remains remote after lowering");
        }
      }
    }
  }
  expect(operations == 12, "four-GPU AllGather preserves twelve PUTs");
  expect(bytes == 12288, "four-GPU AllGather preserves logical PUT bytes");
}

void test_unsupported_operation_rejected() {
  MscclppPlan plan =
      parse_mscclpp_plan_file(data_file("mscclpp_two_gpu.json"));
  plan.gpus[0].threadblocks[0].operations[0].name = "unsupported";
  expect_invalid(
      [&plan]() {
        lower_mscclpp_fine_kernels(
            plan, 2048, MscclppFineLoweringConfig{1, 128, 50});
      },
      "unsupported MSCCL++ operation is rejected explicitly");
}

void test_plan_registry_selection() {
  const MscclppPlan two =
      parse_mscclpp_plan_file(data_file("mscclpp_two_gpu.json"));
  const MscclppPlan four = parse_mscclpp_plan_file(
      data_file("mscclpp_four_gpu_allgather.json"));
  MscclppPlanRegistry registry;
  registry.add(MscclppPlanRegistryEntry{two, 1024, 65536, 1024});
  registry.add(MscclppPlanRegistryEntry{four, 4096, 65536, 4096});
  expect(registry.size() == 2, "plan registry retains registered plans");
  expect(
      registry.select(
          CollectiveKind::AllReduce, 2, "Simple", 2048).name == two.name,
      "plan registry selects by collective, participants, protocol, and payload");
  expect(
      registry.select(
          CollectiveKind::AllGather, 4, "Simple", 4096).name == four.name,
      "plan registry selects independent topology-sized plan");
  expect_invalid(
      [&registry]() {
        registry.select(CollectiveKind::AllReduce, 4, "Simple", 2048);
      },
      "missing plan key is rejected instead of synthesized implicitly");
  registry.add(MscclppPlanRegistryEntry{two, 1024, 65536, 1024});
  expect_invalid(
      [&registry]() {
        registry.select(CollectiveKind::AllReduce, 2, "Simple", 2048);
      },
      "ambiguous plan registry selection is rejected");
}

}  // namespace

int main() {
  test_two_gpu_semantic_lowering();
  test_four_gpu_put_counts_and_bytes();
  test_unsupported_operation_rejected();
  test_plan_registry_selection();
  if (failures != 0) {
    std::cerr << failures << " fine MSCCL++ lowering test(s) failed\n";
    return 1;
  }
  std::cout << "All fine MSCCL++ lowering tests passed\n";
  return 0;
}
