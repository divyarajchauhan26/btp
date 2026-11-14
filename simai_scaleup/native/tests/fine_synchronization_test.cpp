#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "simai/contracts/fine_gpu.hpp"
#include "simai/runtime/fine_gpu_scheduler.hpp"
#include "simai/runtime/fine_synchronization.hpp"

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

FineMemoryReference semaphore_memory() {
  return FineMemoryReference{
      DeviceId(1), FineMemorySpace::SemaphoreMemory, BufferId(50), 0, 4, 4};
}

FineInstructionIssue semaphore_issue(
    FineGpuInstructionKind kind,
    std::uint64_t generation,
    std::uint64_t wavefront,
    std::uint64_t instruction) {
  FineGpuInstruction value{
      GpuInstructionId(instruction),
      GpuOperationId(instruction),
      0,
      kind,
      kind == FineGpuInstructionKind::SemaphoreAcquire,
      kind == FineGpuInstructionKind::SemaphoreAcquire
          ? semaphore_memory()
          : FineMemoryReference{},
      kind == FineGpuInstructionKind::SemaphoreRelease,
      kind == FineGpuInstructionKind::SemaphoreRelease
          ? semaphore_memory()
          : FineMemoryReference{},
      true,
      DeviceId(1),
      SemaphoreId(1),
      generation,
      0,
      0,
      0};
  value.require_valid();
  return FineInstructionIssue{
      KernelInstanceId(1),
      WorkgroupInstanceId(1),
      WavefrontInstanceId(wavefront),
      WavefrontId(wavefront),
      DeviceId(1),
      ComputeUnitId(1),
      0,
      false,
      0,
      value};
}

void test_semaphore_generations_and_waiters() {
  FineSynchronizationEngine synchronization;
  const FineInstructionIssue generation_two = semaphore_issue(
      FineGpuInstructionKind::SemaphoreAcquire, 2, 1, 1);
  const FineInstructionIssue generation_one = semaphore_issue(
      FineGpuInstructionKind::SemaphoreAcquire, 1, 2, 2);
  expect(
      synchronization.process_semaphore(generation_two).empty(),
      "future semaphore generation waits");
  expect(
      synchronization.process_semaphore(generation_one).empty(),
      "first semaphore generation waits");
  expect(
      synchronization.blocked_state_diagnostics().size() == 2,
      "blocked semaphore waiters are diagnosed");

  const std::vector<FineSynchronizationCompletion> first_release =
      synchronization.process_semaphore(semaphore_issue(
          FineGpuInstructionKind::SemaphoreRelease, 1, 3, 3));
  expect(
      first_release.size() == 2,
      "generation one release completes itself and matching waiter");
  expect(
      synchronization.blocked_state_diagnostics().size() == 1,
      "later generation remains blocked");
  const std::vector<FineSynchronizationCompletion> second_release =
      synchronization.process_semaphore(semaphore_issue(
          FineGpuInstructionKind::SemaphoreRelease, 2, 4, 4));
  expect(
      second_release.size() == 2 && synchronization.drained(),
      "generation two release wakes remaining waiter");
  const std::vector<FineSynchronizationCompletion> immediate =
      synchronization.process_semaphore(semaphore_issue(
          FineGpuInstructionKind::SemaphoreAcquire, 1, 5, 5));
  expect(immediate.size() == 1, "published generation satisfies later acquire");
  expect_invalid(
      [&synchronization]() {
        synchronization.process_semaphore(semaphore_issue(
            FineGpuInstructionKind::SemaphoreRelease, 2, 6, 6));
      },
      "release generation cannot regress or repeat");
}

void test_reusable_barriers_and_diagnostics() {
  FineSynchronizationEngine synchronization;
  const std::vector<WavefrontInstanceId> wavefronts{
      WavefrontInstanceId(10), WavefrontInstanceId(11)};
  synchronization.register_workgroup_barrier(
      WorkgroupInstanceId(7), BarrierId(1), 1, wavefronts);
  expect(
      synchronization.arrive_workgroup_barrier(
          WorkgroupInstanceId(7), BarrierId(1), 1, wavefronts[0]).empty(),
      "workgroup barrier waits for all wavefronts");
  expect(
      synchronization.blocked_state_diagnostics().size() == 1,
      "partial workgroup barrier reports missing participants");
  expect_invalid(
      [&synchronization, &wavefronts]() {
        synchronization.arrive_workgroup_barrier(
            WorkgroupInstanceId(7), BarrierId(1), 1, wavefronts[0]);
      },
      "duplicate workgroup barrier arrival is rejected");
  expect(
      synchronization.arrive_workgroup_barrier(
          WorkgroupInstanceId(7), BarrierId(1), 1, wavefronts[1]).size() == 2,
      "final wavefront releases complete participant set");
  synchronization.register_workgroup_barrier(
      WorkgroupInstanceId(7), BarrierId(1), 2, wavefronts);
  synchronization.arrive_workgroup_barrier(
      WorkgroupInstanceId(7), BarrierId(1), 2, wavefronts[0]);
  expect(
      synchronization.arrive_workgroup_barrier(
          WorkgroupInstanceId(7), BarrierId(1), 2, wavefronts[1]).size() == 2,
      "workgroup barrier can be reused with a new generation");

  const std::vector<WorkgroupInstanceId> workgroups{
      WorkgroupInstanceId(20), WorkgroupInstanceId(21)};
  synchronization.register_kernel_barrier(
      KernelInstanceId(8), BarrierId(2), 1, workgroups);
  synchronization.arrive_kernel_barrier(
      KernelInstanceId(8), BarrierId(2), 1, workgroups[0]);
  expect(
      synchronization.arrive_kernel_barrier(
          KernelInstanceId(8), BarrierId(2), 1, workgroups[1]).size() == 2,
      "kernel barrier releases explicit workgroup participants");
  expect(synchronization.drained(), "all reusable barriers drain");
}

void test_wavefront_engine_semaphore_handoff() {
  const FineMemoryReference location = semaphore_memory();
  const FineKernelDefinition kernel{
      KernelId(9),
      DeviceId(1),
      FineKernelKind::Communication,
      {FineWorkgroupDefinition{
          WorkgroupId(9),
          ThreadBlockId(9),
          FineWorkgroupResources{1},
          {FineWavefrontProgram{
               WavefrontId(90),
               {FineGpuOperation::semaphore_acquire(
                   GpuOperationId(90),
                   DeviceId(1),
                   SemaphoreId(1),
                   1,
                   location)}},
           FineWavefrontProgram{
               WavefrontId(91),
               {FineGpuOperation::semaphore_release(
                   GpuOperationId(91),
                   DeviceId(1),
                   SemaphoreId(1),
                   1,
                   location)}}}}}};
  FineGpuScheduler scheduler(FineGpuSchedulerConfig{
      1, 1, 1, FineSchedulingPolicy::RoundRobin, 0});
  FineWavefrontEngine engine(FineWavefrontEngineConfig{
      1, 1, 2, 1, 1, FineInstructionGeneratorConfig{64, 1, BufferId(900)}});
  FineSynchronizationEngine synchronization;
  const std::vector<FineWorkgroupDispatch> dispatches = scheduler.submit(kernel);
  engine.admit(dispatches[0], kernel.workgroups[0], 0);
  const std::vector<FineInstructionIssue> issues = engine.issue_ready(0);
  expect(issues.size() == 2, "semaphore wavefronts issue independently");
  std::vector<FineSynchronizationCompletion> completions;
  for (const FineInstructionIssue& issue : issues) {
    const std::vector<FineSynchronizationCompletion> current =
        synchronization.process_semaphore(issue);
    completions.insert(completions.end(), current.begin(), current.end());
  }
  expect(
      completions.size() == 2,
      "release visibility completes releaser and blocked acquire");
  for (const FineSynchronizationCompletion& completion : completions) {
    engine.complete_instruction(
        completion.wavefront_instance, completion.instruction, 0);
  }
  const std::vector<FineWorkgroupCompletion> workgroups =
      engine.take_completed_workgroups();
  expect(workgroups.size() == 1, "synchronized wavefronts retire their workgroup");
  if (!workgroups.empty()) scheduler.complete(workgroups[0].workgroup_instance);
  expect(
      engine.drained() && scheduler.drained() && synchronization.drained(),
      "synchronized execution state drains across all engines");
}

}  // namespace

int main() {
  test_semaphore_generations_and_waiters();
  test_reusable_barriers_and_diagnostics();
  test_wavefront_engine_semaphore_handoff();
  if (failures != 0) {
    std::cerr << failures << " fine synchronization test(s) failed\n";
    return 1;
  }
  std::cout << "All fine synchronization tests passed\n";
  return 0;
}
