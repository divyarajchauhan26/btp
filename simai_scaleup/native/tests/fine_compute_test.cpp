#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "simai/runtime/fine_compute.hpp"
#include "simai/runtime/fine_gpu_scheduler.hpp"

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

FineMemoryReference memory(
    FineMemorySpace space,
    std::uint64_t buffer,
    std::uint64_t offset,
    std::uint64_t length) {
  return FineMemoryReference{
      DeviceId(1), space, BufferId(buffer), offset, length, 64};
}

FineComputeKernelTemplate compute_template() {
  const FineMemoryReference hbm =
      memory(FineMemorySpace::LocalHbm, 1, 0, 64);
  const FineMemoryReference registers =
      memory(FineMemorySpace::RegisterFile, 2, 0, 64);
  const FineMemoryReference l2 = memory(FineMemorySpace::L2, 3, 0, 64);
  return FineComputeKernelTemplate{
      KernelId(1),
      DeviceId(1),
      2,
      2,
      1,
      {FineComputeOperationTemplate{
           FineGpuOperation::load(GpuOperationId(100), hbm, registers),
           1024,
           256},
       FineComputeOperationTemplate{
           FineGpuOperation::reduce(GpuOperationId(101), 20), 0, 0},
       FineComputeOperationTemplate{
           FineGpuOperation::store(GpuOperationId(102), registers, l2),
           1024,
           256}}};
}

void test_deterministic_template_expansion() {
  const FineKernelDefinition first = lower_compute_kernel(compute_template());
  const FineKernelDefinition second = lower_compute_kernel(compute_template());
  expect(
      first.kind == FineKernelKind::Compute && first.workgroups.size() == 2,
      "compute template creates requested workgroups");
  std::uint64_t expected_wavefront = 1;
  std::uint64_t expected_operation = 1;
  for (std::size_t workgroup_index = 0;
       workgroup_index < first.workgroups.size();
       ++workgroup_index) {
    const FineWorkgroupDefinition& workgroup = first.workgroups[workgroup_index];
    expect(
        workgroup.wavefronts.size() == 2,
        "compute template creates requested wavefronts");
    for (std::size_t wavefront_index = 0;
         wavefront_index < workgroup.wavefronts.size();
         ++wavefront_index) {
      const FineWavefrontProgram& wavefront =
          workgroup.wavefronts[wavefront_index];
      expect(
          wavefront.id == WavefrontId(expected_wavefront++),
          "dynamic template wavefront IDs are deterministic");
      expect(
          wavefront.operations.size() == 3,
          "operation template is copied into each wavefront");
      const std::uint64_t expected_offset =
          workgroup_index * 1024 + wavefront_index * 256;
      expect(
          wavefront.operations[0].source.offset_bytes == expected_offset &&
              wavefront.operations[2].destination.offset_bytes == expected_offset,
          "workgroup and wavefront byte strides are applied exactly");
      for (const FineGpuOperation& operation : wavefront.operations) {
        expect(
            operation.id == GpuOperationId(expected_operation++),
            "expanded operation IDs are globally deterministic");
      }
    }
  }
  expect(
      second.workgroups[1].wavefronts[1].operations[0].source.offset_bytes ==
          first.workgroups[1].wavefronts[1].operations[0].source.offset_bytes,
      "repeated lowering produces identical offsets");
}

void test_compute_and_communication_share_cus() {
  FineGpuScheduler scheduler(FineGpuSchedulerConfig{
      1, 1, 1, FineSchedulingPolicy::RoundRobin, 0});
  FineComputeKernelTemplate description = compute_template();
  description.workgroup_count = 1;
  description.wavefronts_per_workgroup = 1;
  const FineKernelDefinition compute = lower_compute_kernel(description);
  const FineKernelDefinition communication{
      KernelId(2),
      DeviceId(1),
      FineKernelKind::Communication,
      {FineWorkgroupDefinition{
          WorkgroupId(10),
          ThreadBlockId(10),
          FineWorkgroupResources{1},
          {FineWavefrontProgram{
              WavefrontId(10),
              {FineGpuOperation::reduce(GpuOperationId(10), 1)}}}}}};
  const std::vector<FineWorkgroupDispatch> compute_dispatch =
      scheduler.submit(compute);
  const std::vector<FineWorkgroupDispatch> communication_dispatch =
      scheduler.submit(communication);
  expect(compute_dispatch.size() == 1, "compute kernel occupies the finite CU");
  expect(
      communication_dispatch.empty() &&
          scheduler.waiting_workgroups(DeviceId(1)) == 1,
      "communication kernel waits on the same CU residency capacity");
  const std::vector<FineWorkgroupDispatch> released =
      scheduler.complete(compute_dispatch[0].workgroup_instance);
  expect(
      released.size() == 1 && released[0].kernel == communication.id,
      "compute retirement admits waiting communication work");
  scheduler.complete(released[0].workgroup_instance);
  expect(scheduler.drained(), "shared compute/communication scheduling drains");
}

void test_remote_compute_template_rejected() {
  FineComputeKernelTemplate description = compute_template();
  description.operations[0].operation.source = FineMemoryReference{
      DeviceId(2), FineMemorySpace::RemoteHbm, BufferId(9), 0, 64, 64};
  expect_invalid(
      [&description]() {
        lower_compute_kernel(description);
      },
      "compute template cannot smuggle remote communication traffic");
}

}  // namespace

int main() {
  test_deterministic_template_expansion();
  test_compute_and_communication_share_cus();
  test_remote_compute_template_rejected();
  if (failures != 0) {
    std::cerr << failures << " fine compute test(s) failed\n";
    return 1;
  }
  std::cout << "All fine compute tests passed\n";
  return 0;
}
