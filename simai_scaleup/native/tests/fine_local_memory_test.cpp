#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "simai/contracts/fine_memory.hpp"
#include "simai/runtime/fine_instruction_stream.hpp"
#include "simai/runtime/fine_local_memory.hpp"

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
    std::uint64_t device,
    FineMemorySpace space,
    std::uint64_t buffer,
    std::uint64_t length) {
  return FineMemoryReference{
      DeviceId(device), space, BufferId(buffer), 0, length, 64};
}

FineMemoryRequest request(
    std::uint64_t id,
    std::uint64_t device,
    FineMemorySpace space,
    SimTimeNs issue_ns,
    std::uint64_t length = 64) {
  return FineMemoryRequest{
      MemoryRequestId(id),
      KernelInstanceId(1),
      WorkgroupInstanceId(1),
      WavefrontInstanceId(1),
      GpuInstructionId(id),
      DeviceId(device),
      FineMemoryRequestKind::LocalLoad,
      memory(device, space, id, length),
      issue_ns};
}

FineInstructionIssue instruction_issue(
    const FineGpuOperation& operation,
    FineGpuInstructionKind expected_kind,
    DeviceId issuer) {
  FineInstructionStream stream(
      operation,
      FineInstructionGeneratorConfig{64, 1, BufferId(900)},
      GpuInstructionId(100));
  FineGpuInstruction instruction{};
  if (!stream.next(instruction) || instruction.kind != expected_kind) {
    throw std::logic_error("test instruction generation failed");
  }
  return FineInstructionIssue{
      KernelInstanceId(2),
      WorkgroupInstanceId(3),
      WavefrontInstanceId(4),
      WavefrontId(5),
      issuer,
      ComputeUnitId(1),
      7,
      false,
      0,
      instruction};
}

void test_timing_queue_and_retry() {
  FineLocalMemorySystem memory_system(FineLocalMemorySystemConfig{
      1,
      {FineLocalMemoryTierConfig{
          FineMemorySpace::LocalHbm, 10, 16, 1, 3}}});
  const FineMemorySubmitResult first = memory_system.submit(
      request(1, 1, FineMemorySpace::LocalHbm, 0));
  const FineMemorySubmitResult second = memory_system.submit(
      request(2, 1, FineMemorySpace::LocalHbm, 0));
  const FineMemorySubmitResult blocked = memory_system.submit(
      request(3, 1, FineMemorySpace::LocalHbm, 0));

  expect(
      first.accepted && first.completion.service_start_ns == 0 &&
          first.completion.service_end_ns == 4 &&
          first.completion.completion_ns == 14,
      "first request includes service and base latency");
  expect(
      second.accepted && second.completion.service_start_ns == 4 &&
          second.completion.service_end_ns == 8 &&
          second.completion.completion_ns == 18,
      "second request serializes on tier bandwidth");
  expect(
      !blocked.accepted &&
          blocked.stall_reason == FineStallReason::MemoryBackpressure,
      "finite queue rejects excess waiting work without mutation");
  expect(
      memory_system.active_requests(DeviceId(1), FineMemorySpace::LocalHbm) == 2,
      "rejected request does not consume outstanding capacity");
  expect_invalid(
      [&memory_system]() {
        memory_system.complete(MemoryRequestId(1), 13);
      },
      "completion before analytical latency is rejected");
  memory_system.complete(MemoryRequestId(1), 14);

  const FineMemorySubmitResult retry = memory_system.submit(
      request(3, 1, FineMemorySpace::LocalHbm, 14));
  expect(
      retry.accepted && retry.completion.service_start_ns == 14 &&
          retry.completion.completion_ns == 28,
      "backpressured request can retry after capacity is released");
  memory_system.complete(MemoryRequestId(2), 18);
  memory_system.complete(MemoryRequestId(3), 28);
  expect(memory_system.drained(), "local-memory requests drain completely");
}

void test_outstanding_limit_and_device_isolation() {
  FineLocalMemorySystem limited(FineLocalMemorySystemConfig{
      2,
      {FineLocalMemoryTierConfig{
          FineMemorySpace::L2, 3, 64, 4, 1}}});
  const FineMemorySubmitResult device_one =
      limited.submit(request(10, 1, FineMemorySpace::L2, 0));
  const FineMemorySubmitResult same_device =
      limited.submit(request(11, 1, FineMemorySpace::L2, 0));
  const FineMemorySubmitResult device_two =
      limited.submit(request(12, 2, FineMemorySpace::L2, 0));
  expect(device_one.accepted, "device 1 accepts its first request");
  expect(!same_device.accepted, "per-tier outstanding limit is enforced");
  expect(device_two.accepted, "device 2 has independent tier capacity");
  limited.complete(MemoryRequestId(10), device_one.completion.completion_ns);
  limited.complete(MemoryRequestId(12), device_two.completion.completion_ns);
  expect(limited.drained(), "per-device tier state drains independently");
}

void test_instruction_routing() {
  const FineMemoryReference registers =
      memory(1, FineMemorySpace::RegisterFile, 20, 64);
  const FineInstructionIssue load_issue = instruction_issue(
      FineGpuOperation::load(
          GpuOperationId(20),
          memory(1, FineMemorySpace::LocalHbm, 21, 64),
          registers),
      FineGpuInstructionKind::Load,
      DeviceId(1));
  const FineMemoryRequest load =
      lower_local_memory_request(MemoryRequestId(20), load_issue);
  expect(
      load.kind == FineMemoryRequestKind::LocalLoad &&
          load.target.space == FineMemorySpace::LocalHbm &&
          load.instruction == load_issue.instruction.id,
      "Load lowering preserves target and instruction provenance");

  const FineInstructionIssue store_issue = instruction_issue(
      FineGpuOperation::store(
          GpuOperationId(21),
          registers,
          memory(1, FineMemorySpace::L2, 22, 64)),
      FineGpuInstructionKind::Store,
      DeviceId(1));
  const FineMemoryRequest store =
      lower_local_memory_request(MemoryRequestId(21), store_issue);
  expect(
      store.kind == FineMemoryRequestKind::LocalStore &&
          store.target.space == FineMemorySpace::L2,
      "Store lowering routes by destination memory tier");

  const FineInstructionIssue remote_issue = instruction_issue(
      FineGpuOperation::load(
          GpuOperationId(22),
          memory(2, FineMemorySpace::RemoteHbm, 23, 64),
          registers),
      FineGpuInstructionKind::Load,
      DeviceId(1));
  expect_invalid(
      [&remote_issue]() {
        lower_local_memory_request(MemoryRequestId(22), remote_issue);
      },
      "remote Load cannot silently enter local memory");

  FineLocalMemorySystem local_only(FineLocalMemorySystemConfig{
      1,
      {FineLocalMemoryTierConfig{
          FineMemorySpace::L1, 1, 64, 1, 2}}});
  FineMemoryRequest remote_kind = request(30, 1, FineMemorySpace::L1, 0);
  remote_kind.kind = FineMemoryRequestKind::RemoteStore;
  expect_invalid(
      [&local_only, &remote_kind]() {
        local_only.submit(remote_kind);
      },
      "local backend rejects remote request kinds explicitly");
}

void test_invalid_configuration() {
  expect_invalid(
      []() {
        FineLocalMemorySystem(FineLocalMemorySystemConfig{
            1,
            {FineLocalMemoryTierConfig{
                 FineMemorySpace::L1, 1, 64, 1, 2},
             FineLocalMemoryTierConfig{
                 FineMemorySpace::L1, 2, 32, 1, 2}}});
      },
      "duplicate memory tiers are rejected");
  expect_invalid(
      []() {
        FineLocalMemorySystem(FineLocalMemorySystemConfig{
            1,
            {FineLocalMemoryTierConfig{
                FineMemorySpace::RemoteHbm, 1, 64, 1, 2}}});
      },
      "remote HBM cannot be configured as a local tier");
}

}  // namespace

int main() {
  test_timing_queue_and_retry();
  test_outstanding_limit_and_device_isolation();
  test_instruction_routing();
  test_invalid_configuration();
  if (failures != 0) {
    std::cerr << failures << " fine local-memory test(s) failed\n";
    return 1;
  }
  std::cout << "All fine local-memory tests passed\n";
  return 0;
}
