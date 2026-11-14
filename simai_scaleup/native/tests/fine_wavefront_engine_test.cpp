#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "simai/contracts/fine_gpu.hpp"
#include "simai/runtime/fine_gpu_scheduler.hpp"
#include "simai/runtime/fine_wavefront_engine.hpp"

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

FineKernelDefinition make_kernel() {
  std::vector<FineWorkgroupDefinition> workgroups;
  for (std::uint64_t workgroup_index = 0; workgroup_index < 2;
       ++workgroup_index) {
    std::vector<FineWavefrontProgram> wavefronts;
    for (std::uint64_t wavefront_index = 0; wavefront_index < 2;
         ++wavefront_index) {
      const std::uint64_t identity =
          100 + workgroup_index * 10 + wavefront_index;
      wavefronts.push_back(FineWavefrontProgram{
          WavefrontId(identity),
          {FineGpuOperation::reduce(GpuOperationId(identity), 4)}});
    }
    workgroups.push_back(FineWorkgroupDefinition{
        WorkgroupId(workgroup_index + 1),
        ThreadBlockId(workgroup_index + 1),
        FineWorkgroupResources{1},
        wavefronts});
  }
  return FineKernelDefinition{
      KernelId(1), DeviceId(1), FineKernelKind::Compute, workgroups};
}

FineMemoryReference memory(
    std::uint64_t device,
    FineMemorySpace space,
    std::uint64_t buffer,
    std::uint64_t length) {
  return FineMemoryReference{
      DeviceId(device), space, BufferId(buffer), 0, length, 64};
}

const FineWorkgroupDefinition& find_workgroup(
    const FineKernelDefinition& kernel, WorkgroupId id) {
  for (const FineWorkgroupDefinition& workgroup : kernel.workgroups) {
    if (workgroup.id == id) return workgroup;
  }
  throw std::logic_error("test workgroup definition is missing");
}

void finish_dispatch(
    FineWavefrontEngine& engine,
    const FineWorkgroupDispatch& dispatch,
    const FineWorkgroupDefinition& definition,
    SimTimeNs admit_ns) {
  engine.admit(dispatch, definition, admit_ns);
  const std::vector<FineInstructionIssue> first = engine.issue_ready(admit_ns);
  expect(first.size() == 1, "first wavefront issues at admission time");
  const std::vector<FineInstructionIssue> second =
      engine.issue_ready(admit_ns + 2);
  expect(second.size() == 1, "second wavefront issues after CU latency");
  if (first.size() == 1) {
    engine.complete_instruction(
        first[0].wavefront_instance,
        first[0].instruction.id,
        first[0].modeled_completion_ns);
  }
  if (second.size() == 1) {
    engine.complete_instruction(
        second[0].wavefront_instance,
        second[0].instruction.id,
        second[0].modeled_completion_ns);
  }
}

void test_issue_and_hierarchical_retirement() {
  FineGpuScheduler scheduler(FineGpuSchedulerConfig{
      1, 1, 1, FineSchedulingPolicy::RoundRobin, 0});
  FineWavefrontEngine engine(FineWavefrontEngineConfig{
      1, 1, 1, 2, 1, FineInstructionGeneratorConfig{64, 2, BufferId(900)}});
  const FineKernelDefinition kernel = make_kernel();
  const std::vector<FineWorkgroupDispatch> initial = scheduler.submit(kernel);
  expect(initial.size() == 1, "scheduler admits one resident workgroup");
  expect(
      scheduler.waiting_workgroups(DeviceId(1)) == 1,
      "second workgroup waits for CU residency");

  engine.admit(initial[0], find_workgroup(kernel, initial[0].workgroup), 10);
  expect(engine.issue_ready(9).empty(), "wavefront cannot issue before ready time");
  const std::vector<FineInstructionIssue> first = engine.issue_ready(10);
  expect(first.size() == 1, "first wavefront issues one instruction");
  expect(
      first.size() == 1 && first[0].instruction.kind == FineGpuInstructionKind::Reduce,
      "issued instruction preserves operation semantics");
  expect(
      first.size() == 1 && first[0].has_modeled_completion &&
          first[0].modeled_completion_ns == 14,
      "Reduce cycles produce a deterministic completion timestamp");
  expect(engine.issue_ready(11).empty(), "CU issue latency is enforced");
  const std::vector<FineInstructionIssue> second = engine.issue_ready(12);
  expect(
      second.size() == 1 && first.size() == 1 &&
          second[0].wavefront_instance != first[0].wavefront_instance,
      "round-robin issue skips the pending wavefront");

  if (first.size() == 1) {
    const FineWavefrontSnapshot pending =
        engine.snapshot(first[0].wavefront_instance);
    expect(
        pending.instruction_pending &&
            pending.stall_reason == FineStallReason::InstructionIssue,
        "issued wavefront records its pending stall state");
    expect_invalid(
        [&engine, &first]() {
          engine.complete_instruction(
              first[0].wavefront_instance, GpuInstructionId(999), 13);
        },
        "mismatched instruction completion is rejected");
    expect_invalid(
        [&engine, &first]() {
          engine.complete_instruction(
              first[0].wavefront_instance, first[0].instruction.id, 13);
        },
        "Reduce cannot complete before its modeled duration");
    engine.complete_instruction(
        first[0].wavefront_instance,
        first[0].instruction.id,
        first[0].modeled_completion_ns);
  }
  expect(
      engine.take_completed_workgroups().empty(),
      "workgroup waits for every wavefront to retire");
  if (second.size() == 1) {
    engine.complete_instruction(
        second[0].wavefront_instance,
        second[0].instruction.id,
        second[0].modeled_completion_ns);
  }

  const std::vector<FineWorkgroupCompletion> completed =
      engine.take_completed_workgroups();
  expect(completed.size() == 1, "completed workgroup retires from the engine");
  expect(
      engine.active_workgroups() == 0 && engine.active_wavefronts() == 0,
      "retirement erases runtime wavefront state");
  const std::vector<FineWorkgroupDispatch> next =
      completed.empty()
          ? std::vector<FineWorkgroupDispatch>()
          : scheduler.complete(completed[0].workgroup_instance);
  expect(
      next.size() == 1 && next[0].waited_for_residency,
      "retirement releases the CU slot to queued work");

  if (next.size() == 1) {
    finish_dispatch(
        engine, next[0], find_workgroup(kernel, next[0].workgroup), 20);
  }
  const std::vector<FineWorkgroupCompletion> final_completion =
      engine.take_completed_workgroups();
  if (final_completion.size() == 1) {
    scheduler.complete(final_completion[0].workgroup_instance);
  }
  expect(
      final_completion.size() == 1,
      "queued workgroup completes through the same execution path");
  expect(engine.drained(), "wavefront engine drains completely");
  expect(scheduler.drained(), "GPU scheduler drains completely");
}

void test_outstanding_memory_and_waitcnt() {
  FineGpuScheduler scheduler(FineGpuSchedulerConfig{
      1, 1, 1, FineSchedulingPolicy::RoundRobin, 0});
  FineWavefrontEngine engine(FineWavefrontEngineConfig{
      1, 1, 4, 1, 1, FineInstructionGeneratorConfig{64, 4, BufferId(901)}});
  const FineGpuOperation copy = FineGpuOperation::memcpy(
      GpuOperationId(500),
      memory(1, FineMemorySpace::LocalHbm, 10, 256),
      memory(2, FineMemorySpace::RemoteHbm, 11, 256));
  const FineKernelDefinition kernel{
      KernelId(2),
      DeviceId(1),
      FineKernelKind::Communication,
      {FineWorkgroupDefinition{
          WorkgroupId(10),
          ThreadBlockId(10),
          FineWorkgroupResources{1},
          {FineWavefrontProgram{WavefrontId(10), {copy}}}}}};
  const std::vector<FineWorkgroupDispatch> dispatches = scheduler.submit(kernel);
  expect(dispatches.size() == 1, "Memcpy workgroup dispatches");
  if (dispatches.empty()) return;
  engine.admit(dispatches[0], kernel.workgroups[0], 0);

  std::vector<FineInstructionIssue> pending = engine.issue_ready(0);
  expect(pending.size() == 4, "unroll width issues four independent Loads");
  for (const FineInstructionIssue& issue : pending) {
    expect(
        issue.instruction.kind == FineGpuInstructionKind::Load,
        "initial unrolled instructions are Loads");
  }
  const WavefrontInstanceId instance = pending.front().wavefront_instance;
  expect(
      engine.snapshot(instance).outstanding_instructions == 4,
      "issued Loads remain independently outstanding");
  expect(
      engine.snapshot(instance).outstanding_loads == 4 &&
          engine.snapshot(instance).outstanding_stores == 0,
      "request-class counters distinguish Loads from Stores");
  expect(
      engine.issue_ready(1).empty(),
      "Waitcnt is consumed internally rather than sent to memory");
  FineWavefrontSnapshot waiting = engine.snapshot(instance);
  expect(
      waiting.waitcnt_pending && waiting.wait_threshold == 3 &&
          waiting.stall_reason == FineStallReason::Waitcnt,
      "Waitcnt stalls above its outstanding-request threshold");

  engine.complete_instruction(
      pending.back().wavefront_instance, pending.back().instruction.id, 1);
  pending.pop_back();
  waiting = engine.snapshot(instance);
  expect(
      !waiting.waitcnt_pending && waiting.outstanding_instructions == 3,
      "memory completion releases a satisfied Waitcnt");
  const std::vector<FineInstructionIssue> first_store = engine.issue_ready(2);
  expect(
      first_store.size() == 1 &&
          first_store[0].instruction.kind == FineGpuInstructionKind::Store,
      "released Waitcnt allows the paired Store to issue");
  pending.insert(pending.end(), first_store.begin(), first_store.end());
  waiting = engine.snapshot(instance);
  expect(
      waiting.waitcnt_pending && waiting.wait_threshold == 2,
      "next Waitcnt observes Loads and Stores together");
  expect(
      waiting.outstanding_loads == 3 && waiting.outstanding_stores == 1,
      "out-of-order Load completion and Store issue preserve class counts");

  std::size_t load_count = 4;
  std::size_t store_count = first_store.size();
  std::vector<FineWorkgroupCompletion> completed;
  for (SimTimeNs now = 3; now < 100 && completed.empty(); ++now) {
    if (!pending.empty()) {
      const FineInstructionIssue done = pending.front();
      pending.erase(pending.begin());
      engine.complete_instruction(
          done.wavefront_instance, done.instruction.id, now);
    }
    const std::vector<FineInstructionIssue> issued = engine.issue_ready(now);
    for (const FineInstructionIssue& issue : issued) {
      if (issue.instruction.kind == FineGpuInstructionKind::Load) {
        ++load_count;
      } else if (issue.instruction.kind == FineGpuInstructionKind::Store) {
        ++store_count;
      }
      pending.push_back(issue);
    }
    completed = engine.take_completed_workgroups();
  }
  expect(load_count == 4, "Memcpy issues exactly one Load per cache line");
  expect(store_count == 4, "Memcpy issues exactly one Store per cache line");
  expect(completed.size() == 1, "Memcpy retires after its final Store completes");
  if (completed.size() == 1) {
    scheduler.complete(completed[0].workgroup_instance);
  }
  expect(engine.drained(), "memory execution state drains completely");
  expect(scheduler.drained(), "memory scheduler state drains completely");
}

void test_trace_wait_uses_kernel_admission_origin() {
  FineGpuScheduler scheduler(FineGpuSchedulerConfig{
      1, 1, 1, FineSchedulingPolicy::RoundRobin, 0});
  FineWavefrontEngine engine(FineWavefrontEngineConfig{
      1, 1, 1, 2, 1, FineInstructionGeneratorConfig{64, 2, BufferId(902)}});
  const FineKernelDefinition kernel{
      KernelId(3),
      DeviceId(1),
      FineKernelKind::Compute,
      {FineWorkgroupDefinition{
          WorkgroupId(20),
          ThreadBlockId(20),
          FineWorkgroupResources{1},
          {FineWavefrontProgram{
              WavefrontId(20),
              {FineGpuOperation::trace_wait_until(GpuOperationId(600), 20),
               FineGpuOperation::load(
                   GpuOperationId(601),
                   memory(1, FineMemorySpace::LocalHbm, 20, 64),
                   memory(1, FineMemorySpace::RegisterFile, 21, 64))}}}}}};
  const std::vector<FineWorkgroupDispatch> dispatches = scheduler.submit(kernel);
  expect(dispatches.size() == 1, "trace replay workgroup dispatches");
  if (dispatches.empty()) return;
  engine.admit(dispatches[0], kernel.workgroups[0], 100);
  const std::vector<FineInstructionIssue> wait = engine.issue_ready(100);
  expect(
      wait.size() == 1 &&
          wait[0].instruction.kind == FineGpuInstructionKind::TraceWaitUntil &&
          wait[0].modeled_completion_ns == 120,
      "trace timestamp is relative to kernel admission");
  expect(
      wait.size() == 1 &&
          engine.snapshot(wait[0].wavefront_instance).stall_reason ==
              FineStallReason::TraceReplay,
      "trace replay wait has an explicit stall reason");
  if (wait.empty()) return;
  expect_invalid(
      [&engine, &wait]() {
        engine.complete_instruction(
            wait[0].wavefront_instance, wait[0].instruction.id, 119);
      },
      "trace wait cannot complete before its timestamp");
  engine.complete_instruction(
      wait[0].wavefront_instance, wait[0].instruction.id, 120);
  const std::vector<FineInstructionIssue> load = engine.issue_ready(120);
  expect(
      load.size() == 1 &&
          load[0].instruction.kind == FineGpuInstructionKind::Load,
      "memory operation issues at the recorded earliest timestamp");

  FineGpuScheduler delayed_scheduler(FineGpuSchedulerConfig{
      1, 1, 1, FineSchedulingPolicy::RoundRobin, 0});
  FineWavefrontEngine delayed_engine(FineWavefrontEngineConfig{
      1, 1, 1, 2, 1, FineInstructionGeneratorConfig{64, 2, BufferId(903)}});
  const std::vector<FineWorkgroupDispatch> delayed_dispatches =
      delayed_scheduler.submit(kernel);
  expect(
      delayed_dispatches.size() == 1,
      "delayed trace replay workgroup dispatches");
  if (delayed_dispatches.empty()) return;
  delayed_engine.admit(
      delayed_dispatches[0], kernel.workgroups[0], 100, 0);
  const std::vector<FineInstructionIssue> expired_wait =
      delayed_engine.issue_ready(100);
  expect(
      expired_wait.size() == 1 &&
          expired_wait[0].modeled_completion_ns == 100,
      "queued workgroup keeps the original kernel timestamp origin");
}

}  // namespace

int main() {
  test_issue_and_hierarchical_retirement();
  test_outstanding_memory_and_waitcnt();
  test_trace_wait_uses_kernel_admission_origin();
  if (failures != 0) {
    std::cerr << failures << " fine wavefront engine test(s) failed\n";
    return 1;
  }
  std::cout << "All fine wavefront engine tests passed\n";
  return 0;
}
