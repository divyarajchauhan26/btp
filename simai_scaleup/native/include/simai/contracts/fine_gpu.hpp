#ifndef SIMAI_CONTRACTS_FINE_GPU_HPP
#define SIMAI_CONTRACTS_FINE_GPU_HPP

#include <cstdint>
#include <vector>

#include "simai/contracts/ids.hpp"
#include "simai/contracts/validation.hpp"

namespace simai {
namespace contracts {

enum class FineMemorySpace {
  RegisterFile,
  L1,
  L2,
  LocalHbm,
  RemoteHbm,
  SemaphoreMemory
};

enum class FineGpuInstructionKind {
  Load,
  Store,
  SemaphoreAcquire,
  SemaphoreRelease,
  Reduce,
  Waitcnt,
  TraceWaitUntil
};

enum class FineMemoryRequestKind {
  LocalLoad,
  LocalStore,
  RemoteLoadRequest,
  RemoteLoadResponse,
  RemoteStore,
  SemaphoreAcquire,
  SemaphoreRelease
};

enum class FineStallReason {
  None,
  CuResidency,
  InstructionIssue,
  MemoryBackpressure,
  Waitcnt,
  Semaphore,
  TraceReplay,
  WorkgroupBarrier,
  KernelBarrier
};

enum class FineOrderingScope { Wavefront, Workgroup, Kernel, Device, System };

enum class FineSchedulingPolicy { RoundRobin, LeastOccupied, SeededRandom };

struct FineMemoryReference {
  DeviceId owner;
  FineMemorySpace space;
  BufferId buffer;
  std::uint64_t offset_bytes;
  std::uint64_t length_bytes;
  std::uint64_t alignment_bytes;

  ValidationErrors validate() const;
  void require_valid() const;
};

struct FineGpuInstruction {
  GpuInstructionId id;
  GpuOperationId parent_operation;
  std::uint64_t sequence_index;
  FineGpuInstructionKind kind;
  bool has_source;
  FineMemoryReference source;
  bool has_destination;
  FineMemoryReference destination;
  bool has_semaphore;
  DeviceId semaphore_owner;
  SemaphoreId semaphore;
  std::uint64_t semaphore_generation;
  std::uint64_t wait_threshold;
  std::uint64_t reduce_cycles;
  SimTimeNs trace_timestamp_ns;

  ValidationErrors validate() const;
  void require_valid() const;
};

enum class FineGpuOperationKind {
  Load,
  Store,
  Memcpy,
  SemaphoreAcquire,
  SemaphoreRelease,
  Reduce,
  TraceWaitUntil,
  WorkgroupBarrier,
  KernelBarrier
};

struct FineGpuOperation {
  GpuOperationId id;
  FineGpuOperationKind kind;
  bool has_source;
  FineMemoryReference source;
  bool has_destination;
  FineMemoryReference destination;
  bool has_semaphore;
  DeviceId semaphore_owner;
  SemaphoreId semaphore;
  std::uint64_t semaphore_generation;
  bool has_barrier;
  BarrierId barrier;
  std::uint64_t reduce_cycles;
  SimTimeNs trace_timestamp_ns;

  static FineGpuOperation load(
      GpuOperationId id,
      const FineMemoryReference& source,
      const FineMemoryReference& register_destination);
  static FineGpuOperation store(
      GpuOperationId id,
      const FineMemoryReference& register_source,
      const FineMemoryReference& destination);
  static FineGpuOperation memcpy(
      GpuOperationId id,
      const FineMemoryReference& source,
      const FineMemoryReference& destination);
  static FineGpuOperation semaphore_acquire(
      GpuOperationId id,
      DeviceId semaphore_owner,
      SemaphoreId semaphore,
      std::uint64_t generation,
      const FineMemoryReference& location);
  static FineGpuOperation semaphore_release(
      GpuOperationId id,
      DeviceId semaphore_owner,
      SemaphoreId semaphore,
      std::uint64_t generation,
      const FineMemoryReference& location);
  static FineGpuOperation reduce(
      GpuOperationId id, std::uint64_t cycles);
  static FineGpuOperation trace_wait_until(
      GpuOperationId id, SimTimeNs timestamp_ns);
  static FineGpuOperation workgroup_barrier(
      GpuOperationId id, BarrierId barrier);
  static FineGpuOperation kernel_barrier(
      GpuOperationId id, BarrierId barrier);

  ValidationErrors validate() const;
  void require_valid() const;
};

struct FineWavefrontProgram {
  WavefrontId id;
  std::vector<FineGpuOperation> operations;

  ValidationErrors validate() const;
  void require_valid() const;
};

struct FineWorkgroupResources {
  std::uint32_t residency_slots;

  ValidationErrors validate() const;
  void require_valid() const;
};

struct FineWorkgroupDefinition {
  WorkgroupId id;
  ThreadBlockId source_thread_block;
  FineWorkgroupResources resources;
  std::vector<FineWavefrontProgram> wavefronts;

  ValidationErrors validate() const;
  void require_valid() const;
};

enum class FineKernelKind { Compute, Communication };

struct FineKernelDefinition {
  KernelId id;
  DeviceId device;
  FineKernelKind kind;
  std::vector<FineWorkgroupDefinition> workgroups;

  ValidationErrors validate() const;
  void require_valid() const;
};

}  // namespace contracts
}  // namespace simai

#endif
