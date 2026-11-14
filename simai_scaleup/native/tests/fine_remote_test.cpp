#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "simai/runtime/fine_instruction_stream.hpp"
#include "simai/runtime/fine_remote.hpp"

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
    std::uint64_t length,
    std::uint64_t alignment = 64) {
  return FineMemoryReference{
      DeviceId(device), space, BufferId(buffer), 0, length, alignment};
}

FineInstructionIssue issue(
    const FineGpuOperation& operation,
    DeviceId issuer,
    std::uint64_t instruction_id) {
  FineInstructionStream stream(
      operation,
      FineInstructionGeneratorConfig{64, 1, BufferId(900)},
      GpuInstructionId(instruction_id));
  FineGpuInstruction instruction{};
  if (!stream.next(instruction)) {
    throw std::logic_error("test operation did not generate an instruction");
  }
  return FineInstructionIssue{
      KernelInstanceId(1),
      WorkgroupInstanceId(2),
      WavefrontInstanceId(3),
      WavefrontId(4),
      issuer,
      ComputeUnitId(1),
      5,
      false,
      0,
      instruction};
}

LinkConfig link_config() {
  return LinkConfig{
      400, 20, 32, 64, 16, 1, 0, 64, 5, 64, 5, 0, 0, 0, 5};
}

std::uint64_t payload(const std::vector<SuePdu>& pdus) {
  std::uint64_t bytes = 0;
  for (const SuePdu& pdu : pdus) bytes += pdu.payload_bytes;
  return bytes;
}

void test_remote_load_and_store() {
  FineRemotePacketizer packetizer(link_config(), BufferId(999));
  std::uint64_t next_transaction = 1;
  std::uint64_t next_pdu = 1;
  const FineInstructionIssue store_issue = issue(
      FineGpuOperation::store(
          GpuOperationId(1),
          memory(1, FineMemorySpace::RegisterFile, 10, 64),
          memory(2, FineMemorySpace::RemoteHbm, 11, 64)),
      DeviceId(1),
      100);
  const FineRemoteRequest store =
      lower_remote_request(MemoryRequestId(1), store_issue);
  const FineRemotePacketBatch store_batch = packetizer.packetize(
      store, next_transaction, next_pdu);
  expect(
      store.kind == FineMemoryRequestKind::RemoteStore &&
          store.completion_stage == CompletionStage::RemoteVisible,
      "remote Store requests remote visibility completion");
  expect(
      payload(store_batch.command_pdus) == 64 &&
          store_batch.response_pdus.empty(),
      "remote Store emits one-way payload-conserving PUT traffic");
  expect(
      store_batch.request.instruction == store_issue.instruction.id &&
          store_batch.request.wavefront_instance ==
              store_issue.wavefront_instance,
      "remote Store batch preserves fine execution provenance");

  const FineInstructionIssue load_issue = issue(
      FineGpuOperation::load(
          GpuOperationId(2),
          memory(2, FineMemorySpace::RemoteHbm, 12, 64),
          memory(1, FineMemorySpace::RegisterFile, 13, 64)),
      DeviceId(1),
      101);
  const FineRemoteRequest load =
      lower_remote_request(MemoryRequestId(2), load_issue);
  const FineRemotePacketBatch load_batch = packetizer.packetize(
      load, next_transaction, next_pdu);
  expect(
      load.kind == FineMemoryRequestKind::RemoteLoadRequest &&
          load.completion_stage == CompletionStage::Finalized,
      "remote Load completes only after response delivery");
  expect(
      payload(load_batch.command_pdus) == 16 &&
          payload(load_batch.response_pdus) == 64,
      "remote Load emits chunk requests and exact data response");
  expect(
      load_batch.command_pdus.front().source_endpoint == DeviceId(1) &&
          load_batch.response_pdus.front().source_endpoint == DeviceId(2),
      "remote Load request and response reverse transport direction");
}

void test_remote_semaphores() {
  FineRemotePacketizer packetizer(link_config(), BufferId(998));
  std::uint64_t next_transaction = 20;
  std::uint64_t next_pdu = 20;
  const FineMemoryReference location =
      memory(2, FineMemorySpace::SemaphoreMemory, 20, 4, 4);
  const FineInstructionIssue release_issue = issue(
      FineGpuOperation::semaphore_release(
          GpuOperationId(3), DeviceId(2), SemaphoreId(1), 1, location),
      DeviceId(1),
      102);
  const FineRemotePacketBatch release = packetizer.packetize(
      lower_remote_request(MemoryRequestId(3), release_issue),
      next_transaction,
      next_pdu);
  expect(
      release.command_pdus.size() == 1 && release.response_pdus.empty() &&
          release.command_pdus[0].transactions[0].service == SueService::Atomic,
      "remote semaphore release emits one atomic command");

  const FineInstructionIssue acquire_issue = issue(
      FineGpuOperation::semaphore_acquire(
          GpuOperationId(4), DeviceId(2), SemaphoreId(1), 1, location),
      DeviceId(1),
      103);
  const FineRemotePacketBatch acquire = packetizer.packetize(
      lower_remote_request(MemoryRequestId(4), acquire_issue),
      next_transaction,
      next_pdu);
  expect(
      payload(acquire.command_pdus) == 8 &&
          payload(acquire.response_pdus) == 4,
      "remote semaphore acquire uses read request and value response");
}

void test_strict_remote_routing() {
  const FineInstructionIssue local = issue(
      FineGpuOperation::load(
          GpuOperationId(5),
          memory(1, FineMemorySpace::LocalHbm, 30, 64),
          memory(1, FineMemorySpace::RegisterFile, 31, 64)),
      DeviceId(1),
      104);
  expect_invalid(
      [&local]() {
        lower_remote_request(MemoryRequestId(5), local);
      },
      "local Load cannot silently enter remote transport");
}

}  // namespace

int main() {
  test_remote_load_and_store();
  test_remote_semaphores();
  test_strict_remote_routing();
  if (failures != 0) {
    std::cerr << failures << " fine remote test(s) failed\n";
    return 1;
  }
  std::cout << "All fine remote tests passed\n";
  return 0;
}
