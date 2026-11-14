#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "simai/contracts/fine_gpu.hpp"
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

FineKernelDefinition make_kernel(
    std::uint64_t kernel_id,
    std::uint64_t device,
    std::uint64_t first_workgroup,
    std::uint32_t workgroup_count,
    std::uint32_t residency_slots) {
  std::vector<FineWorkgroupDefinition> workgroups;
  for (std::uint32_t index = 0; index < workgroup_count; ++index) {
    const std::uint64_t workgroup_id = first_workgroup + index;
    workgroups.push_back(FineWorkgroupDefinition{
        WorkgroupId(workgroup_id),
        ThreadBlockId(workgroup_id),
        FineWorkgroupResources{residency_slots},
        {FineWavefrontProgram{
            WavefrontId(workgroup_id),
            {FineGpuOperation::reduce(
                GpuOperationId(workgroup_id), 1)}}}});
  }
  return FineKernelDefinition{
      KernelId(kernel_id),
      DeviceId(device),
      FineKernelKind::Compute,
      workgroups};
}

void test_round_robin_and_waiting() {
  FineGpuScheduler scheduler(FineGpuSchedulerConfig{
      2, 2, 1, FineSchedulingPolicy::RoundRobin, 0});
  const FineKernelDefinition kernel = make_kernel(1, 1, 1, 4, 1);
  const std::vector<FineWorkgroupDispatch> initial = scheduler.submit(kernel);
  expect(initial.size() == 2, "round-robin dispatch fills both CUs");
  expect(
      initial[0].workgroup == WorkgroupId(1) &&
          initial[0].compute_unit == ComputeUnitId(1),
      "first workgroup dispatches to CU 1");
  expect(
      initial[1].workgroup == WorkgroupId(2) &&
          initial[1].compute_unit == ComputeUnitId(2),
      "second workgroup dispatches to CU 2");
  expect(
      scheduler.active_workgroups(DeviceId(1)) == 2 &&
          scheduler.waiting_workgroups(DeviceId(1)) == 2,
      "excess workgroups wait for residency");

  const std::vector<FineWorkgroupDispatch> after_first =
      scheduler.complete(initial[0].workgroup_instance);
  expect(
      after_first.size() == 1 &&
          after_first[0].workgroup == WorkgroupId(3) &&
          after_first[0].compute_unit == ComputeUnitId(1) &&
          after_first[0].waited_for_residency,
      "first queued workgroup takes released CU slot");
  const std::vector<FineWorkgroupDispatch> after_second =
      scheduler.complete(initial[1].workgroup_instance);
  expect(
      after_second.size() == 1 &&
          after_second[0].workgroup == WorkgroupId(4) &&
          after_second[0].compute_unit == ComputeUnitId(2),
      "second queued workgroup preserves FIFO order");

  scheduler.complete(after_first[0].workgroup_instance);
  scheduler.complete(after_second[0].workgroup_instance);
  expect(scheduler.drained(), "round-robin scheduler drains completely");
}

void test_residency_capacity_and_least_occupied() {
  FineGpuScheduler scheduler(FineGpuSchedulerConfig{
      1, 2, 2, FineSchedulingPolicy::LeastOccupied, 0});
  const std::vector<FineWorkgroupDispatch> dispatches =
      scheduler.submit(make_kernel(2, 1, 10, 3, 1));
  expect(dispatches.size() == 3, "two-slot CUs admit three workgroups");
  expect(
      dispatches[0].compute_unit == ComputeUnitId(1) &&
          dispatches[1].compute_unit == ComputeUnitId(2) &&
          dispatches[2].compute_unit == ComputeUnitId(1),
      "least-occupied policy balances then breaks ties by CU ID");
  expect(
      scheduler.resident_slots(DeviceId(1), ComputeUnitId(1)) == 2 &&
          scheduler.resident_slots(DeviceId(1), ComputeUnitId(2)) == 1,
      "resident-slot accounting is exact");

  expect_invalid(
      [&scheduler]() {
        scheduler.submit(make_kernel(3, 1, 20, 1, 3));
      },
      "workgroup larger than CU capacity is rejected");

  scheduler.complete(dispatches[0].workgroup_instance);
  scheduler.complete(dispatches[1].workgroup_instance);
  scheduler.complete(dispatches[2].workgroup_instance);
  expect(scheduler.drained(), "least-occupied scheduler drains completely");

  FineGpuScheduler transactional(FineGpuSchedulerConfig{
      1, 1, 2, FineSchedulingPolicy::RoundRobin, 0});
  FineKernelDefinition mixed = make_kernel(7, 1, 60, 2, 1);
  mixed.workgroups[1].resources.residency_slots = 3;
  expect_invalid(
      [&transactional, &mixed]() {
        transactional.submit(mixed);
      },
      "kernel with an impossible later workgroup is rejected atomically");
  expect(
      transactional.drained(),
      "failed kernel submission leaves no partially admitted workgroups");
}

void test_seeded_random_reproducibility() {
  const FineGpuSchedulerConfig config{
      1, 4, 1, FineSchedulingPolicy::SeededRandom, 12345};
  FineGpuScheduler first(config);
  FineGpuScheduler second(config);
  const FineKernelDefinition kernel = make_kernel(4, 1, 30, 4, 1);
  const std::vector<FineWorkgroupDispatch> first_dispatches =
      first.submit(kernel);
  const std::vector<FineWorkgroupDispatch> second_dispatches =
      second.submit(kernel);
  expect(
      first_dispatches.size() == second_dispatches.size(),
      "seeded schedulers dispatch equal workgroup counts");
  for (std::size_t index = 0; index < first_dispatches.size(); ++index) {
    expect(
        first_dispatches[index].compute_unit ==
            second_dispatches[index].compute_unit,
        "fixed seed reproduces CU assignments");
  }
}

void test_multi_device_isolation_and_errors() {
  FineGpuScheduler scheduler(FineGpuSchedulerConfig{
      2, 1, 1, FineSchedulingPolicy::RoundRobin, 0});
  const FineKernelDefinition device_one = make_kernel(5, 1, 40, 1, 1);
  const FineKernelDefinition device_two = make_kernel(6, 2, 50, 1, 1);
  const std::vector<FineWorkgroupDispatch> first_device_dispatch =
      scheduler.submit(device_one);
  const std::vector<FineWorkgroupDispatch> second_device_dispatch =
      scheduler.submit(device_two);
  expect(first_device_dispatch.size() == 1, "device 1 dispatches work");
  expect(second_device_dispatch.size() == 1, "device 2 dispatches independently");
  expect(
      scheduler.active_workgroups(DeviceId(1)) == 1 &&
          scheduler.active_workgroups(DeviceId(2)) == 1,
      "active workgroup accounting is device-local");
  const std::vector<FineWorkgroupDispatch> repeated = scheduler.submit(device_one);
  expect(
      repeated.empty() && scheduler.waiting_workgroups(DeviceId(1)) == 1,
      "repeated static kernel creates a distinct waiting runtime instance");
  expect_invalid(
      [&scheduler]() {
        scheduler.complete(WorkgroupInstanceId(999));
      },
      "completion of unknown runtime workgroup instance is rejected");
  const std::vector<FineWorkgroupDispatch> repeated_dispatch =
      scheduler.complete(first_device_dispatch[0].workgroup_instance);
  expect(
      repeated_dispatch.size() == 1 &&
          repeated_dispatch[0].workgroup == WorkgroupId(40) &&
          repeated_dispatch[0].workgroup_instance !=
              first_device_dispatch[0].workgroup_instance,
      "repeated static workgroup receives a distinct runtime identity");
  scheduler.complete(repeated_dispatch[0].workgroup_instance);
  scheduler.complete(second_device_dispatch[0].workgroup_instance);
  expect(scheduler.drained(), "multi-device scheduler drains completely");
}

}  // namespace

int main() {
  test_round_robin_and_waiting();
  test_residency_capacity_and_least_occupied();
  test_seeded_random_reproducibility();
  test_multi_device_isolation_and_errors();
  if (failures != 0) {
    std::cerr << failures << " fine GPU scheduler test(s) failed\n";
    return 1;
  }
  std::cout << "All fine GPU scheduler tests passed\n";
  return 0;
}
