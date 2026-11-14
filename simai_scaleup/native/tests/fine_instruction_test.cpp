#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "simai/contracts/fine_gpu.hpp"
#include "simai/runtime/fine_instruction_stream.hpp"

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
    std::uint64_t offset,
    std::uint64_t length) {
  return FineMemoryReference{
      DeviceId(device), space, BufferId(buffer), offset, length, 128};
}

std::vector<FineGpuInstruction> collect(FineInstructionStream& stream) {
  std::vector<FineGpuInstruction> instructions;
  FineGpuInstruction instruction{};
  while (stream.next(instruction)) instructions.push_back(instruction);
  return instructions;
}

void test_unrolled_memcpy_stream() {
  const FineGpuOperation operation = FineGpuOperation::memcpy(
      GpuOperationId(1),
      memory(1, FineMemorySpace::LocalHbm, 1, 0, 1024),
      memory(2, FineMemorySpace::RemoteHbm, 2, 0, 1024));
  FineInstructionStream stream(
      operation,
      FineInstructionGeneratorConfig{128, 4, BufferId(900)},
      GpuInstructionId(100));
  expect(stream.instruction_count() == 24, "Memcpy instruction count is lazy and exact");
  const std::vector<FineGpuInstruction> instructions = collect(stream);
  expect(instructions.size() == 24, "Memcpy emits three instructions per line");
  expect(stream.emitted_count() == 24, "Memcpy emitted count is exact");

  for (std::size_t index = 0; index < 4; ++index) {
    expect(
        instructions[index].kind == FineGpuInstructionKind::Load,
        "unrolled Memcpy begins with grouped Loads");
  }
  expect(
      instructions[4].kind == FineGpuInstructionKind::Waitcnt &&
          instructions[4].wait_threshold == 3,
      "first unrolled Waitcnt allows three outstanding Loads");
  expect(
      instructions[5].kind == FineGpuInstructionKind::Store,
      "first unrolled Store follows Waitcnt");
  expect(
      instructions[6].kind == FineGpuInstructionKind::Waitcnt &&
          instructions[6].wait_threshold == 2,
      "unrolled Waitcnt threshold decreases");

  std::uint64_t loaded_bytes = 0;
  std::uint64_t stored_bytes = 0;
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    const FineGpuInstruction& instruction = instructions[index];
    expect(
        instruction.id == GpuInstructionId(100 + index),
        "instruction identities are contiguous");
    expect(
        instruction.sequence_index == index,
        "instruction sequence indices are contiguous");
    expect(
        instruction.parent_operation == GpuOperationId(1),
        "instruction provenance retains parent operation");
    if (instruction.kind == FineGpuInstructionKind::Load) {
      loaded_bytes += instruction.source.length_bytes;
    } else if (instruction.kind == FineGpuInstructionKind::Store) {
      stored_bytes += instruction.destination.length_bytes;
    }
  }
  expect(loaded_bytes == 1024, "Memcpy Load bytes are conserved");
  expect(stored_bytes == 1024, "Memcpy Store bytes are conserved");
}

void test_partial_cache_line() {
  const FineGpuOperation operation = FineGpuOperation::memcpy(
      GpuOperationId(2),
      memory(1, FineMemorySpace::LocalHbm, 3, 0, 300),
      memory(2, FineMemorySpace::RemoteHbm, 4, 0, 300));
  FineInstructionStream stream(
      operation,
      FineInstructionGeneratorConfig{128, 1, BufferId(901)},
      GpuInstructionId(200));
  const std::vector<FineGpuInstruction> instructions = collect(stream);
  expect(instructions.size() == 9, "partial-line Memcpy emits three line groups");
  expect(
      instructions[6].kind == FineGpuInstructionKind::Load &&
          instructions[6].source.length_bytes == 44,
      "partial final Load has exact byte count");
  expect(
      instructions[8].kind == FineGpuInstructionKind::Store &&
          instructions[8].destination.length_bytes == 44,
      "partial final Store has exact byte count");
}

void test_wavefront_partitioning() {
  const FineMemoryReference span =
      memory(1, FineMemorySpace::LocalHbm, 10, 0, 1024);
  const FineMemoryPartition first =
      partition_fine_memory_span(span, 0, 3, 128);
  const FineMemoryPartition second =
      partition_fine_memory_span(span, 1, 3, 128);
  const FineMemoryPartition third =
      partition_fine_memory_span(span, 2, 3, 128);
  expect(
      !first.empty && first.reference.offset_bytes == 0 &&
          first.reference.length_bytes == 384,
      "first wavefront receives first three lines");
  expect(
      !second.empty && second.reference.offset_bytes == 384 &&
          second.reference.length_bytes == 384,
      "second wavefront receives next three lines");
  expect(
      !third.empty && third.reference.offset_bytes == 768 &&
          third.reference.length_bytes == 256,
      "third wavefront receives final two lines");

  const FineGpuOperation operation = FineGpuOperation::memcpy(
      GpuOperationId(3),
      span,
      memory(2, FineMemorySpace::RemoteHbm, 11, 2048, 1024));
  const FineGpuOperation partitioned =
      partition_fine_operation(operation, 1, 3, 128);
  expect(
      partitioned.source.offset_bytes == 384 &&
          partitioned.destination.offset_bytes == 2432 &&
          partitioned.source.length_bytes == 384 &&
          partitioned.destination.length_bytes == 384,
      "operation partition preserves matching source and destination range");

  const FineMemoryPartition empty =
      partition_fine_memory_span(span, 9, 10, 128);
  expect(empty.empty, "excess wavefront receives an explicit empty partition");
  expect_invalid(
      [&operation]() {
        partition_fine_operation(operation, 9, 10, 128);
      },
      "empty operation partition is rejected");
}

void test_singleton_and_empty_streams() {
  const FineMemoryReference semaphore =
      memory(2, FineMemorySpace::SemaphoreMemory, 20, 0, 128);
  const FineGpuOperation acquire = FineGpuOperation::semaphore_acquire(
      GpuOperationId(4), DeviceId(2), SemaphoreId(1), 3, semaphore);
  FineInstructionStream acquire_stream(
      acquire,
      FineInstructionGeneratorConfig{128, 1, BufferId(902)},
      GpuInstructionId(300));
  const std::vector<FineGpuInstruction> acquire_instructions =
      collect(acquire_stream);
  expect(
      acquire_instructions.size() == 1 &&
          acquire_instructions.front().kind ==
              FineGpuInstructionKind::SemaphoreAcquire &&
          acquire_instructions.front().semaphore_generation == 3,
      "SemaphoreAcquire emits one provenance-preserving instruction");

  const FineGpuOperation release = FineGpuOperation::semaphore_release(
      GpuOperationId(7), DeviceId(2), SemaphoreId(1), 4, semaphore);
  FineInstructionStream release_stream(
      release,
      FineInstructionGeneratorConfig{128, 1, BufferId(905)},
      GpuInstructionId(303));
  const std::vector<FineGpuInstruction> release_instructions =
      collect(release_stream);
  expect(
      release_instructions.size() == 1 &&
          release_instructions.front().kind ==
              FineGpuInstructionKind::SemaphoreRelease &&
          release_instructions.front().semaphore_generation == 4,
      "SemaphoreRelease emits one provenance-preserving instruction");

  const FineGpuOperation reduce =
      FineGpuOperation::reduce(GpuOperationId(5), 17);
  FineInstructionStream reduce_stream(
      reduce,
      FineInstructionGeneratorConfig{128, 1, BufferId(903)},
      GpuInstructionId(301));
  const std::vector<FineGpuInstruction> reduce_instructions =
      collect(reduce_stream);
  expect(
      reduce_instructions.size() == 1 &&
          reduce_instructions.front().reduce_cycles == 17,
      "Reduce emits one timed instruction");

  const FineGpuOperation barrier = FineGpuOperation::workgroup_barrier(
      GpuOperationId(6), BarrierId(1));
  FineInstructionStream barrier_stream(
      barrier,
      FineInstructionGeneratorConfig{128, 1, BufferId(904)},
      GpuInstructionId(302));
  expect(
      barrier_stream.instruction_count() == 0 &&
          collect(barrier_stream).empty(),
      "barrier remains a zero-instruction execution control operation");

  const FineGpuOperation kernel_barrier = FineGpuOperation::kernel_barrier(
      GpuOperationId(8), BarrierId(2));
  FineInstructionStream kernel_barrier_stream(
      kernel_barrier,
      FineInstructionGeneratorConfig{128, 1, BufferId(906)},
      GpuInstructionId(304));
  expect(
      kernel_barrier_stream.instruction_count() == 0 &&
          collect(kernel_barrier_stream).empty(),
      "kernel barrier remains a zero-instruction execution control operation");
}

void test_load_and_store_streams() {
  const FineMemoryReference local =
      memory(1, FineMemorySpace::LocalHbm, 30, 0, 300);
  const FineMemoryReference registers =
      memory(1, FineMemorySpace::RegisterFile, 31, 0, 300);
  const FineMemoryReference remote =
      memory(2, FineMemorySpace::RemoteHbm, 32, 0, 300);
  FineInstructionStream load_stream(
      FineGpuOperation::load(GpuOperationId(9), local, registers),
      FineInstructionGeneratorConfig{128, 2, BufferId(907)},
      GpuInstructionId(400));
  const std::vector<FineGpuInstruction> loads = collect(load_stream);
  expect(
      loads.size() == 3 &&
          loads[0].kind == FineGpuInstructionKind::Load &&
          loads[2].source.length_bytes == 44,
      "LoadOp emits one Load per cache line including partial line");

  FineInstructionStream store_stream(
      FineGpuOperation::store(GpuOperationId(10), registers, remote),
      FineInstructionGeneratorConfig{128, 2, BufferId(908)},
      GpuInstructionId(500));
  const std::vector<FineGpuInstruction> stores = collect(store_stream);
  expect(
      stores.size() == 3 &&
          stores[0].kind == FineGpuInstructionKind::Store &&
          stores[2].destination.length_bytes == 44,
      "StoreOp emits one Store per cache line including partial line");
}

}  // namespace

int main() {
  test_unrolled_memcpy_stream();
  test_partial_cache_line();
  test_wavefront_partitioning();
  test_singleton_and_empty_streams();
  test_load_and_store_streams();
  if (failures != 0) {
    std::cerr << failures << " fine instruction test(s) failed\n";
    return 1;
  }
  std::cout << "All fine instruction tests passed\n";
  return 0;
}
