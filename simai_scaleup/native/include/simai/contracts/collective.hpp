#ifndef SIMAI_CONTRACTS_COLLECTIVE_HPP
#define SIMAI_CONTRACTS_COLLECTIVE_HPP

#include <cstdint>
#include <vector>

#include "simai/contracts/buffer.hpp"
#include "simai/contracts/completion_stage.hpp"
#include "simai/contracts/parallel_group.hpp"
#include "simai/contracts/validation.hpp"

namespace simai {
namespace contracts {

enum class CollectiveKind {
  AllReduce,
  ReduceScatter,
  AllGather,
  AllToAll,
  Broadcast
};

enum class MicroOpKind { Put, Get, Signal, Wait, Reduce };
enum class MemoryOrder { Relaxed, Ordered };
enum class ReductionKind { None, Sum, Minimum, Maximum };

struct MicroOp {
  OperationId id;
  MicroOpKind kind;
  DeviceId issuer;
  ThreadBlockId thread_block;
  std::uint32_t program_order;
  std::vector<OperationId> dependencies;
  MemoryOrder memory_order;
  OrderingDomainId ordering_domain;
  CompletionStage release_stage;

  bool has_source;
  BufferSlice source;
  bool has_destination;
  BufferSlice destination;

  bool has_semaphore;
  DeviceId semaphore_owner;
  SemaphoreId semaphore;
  std::uint64_t semaphore_value;

  ReductionKind reduction;

  static MicroOp put(
      OperationId id,
      DeviceId issuer,
      ThreadBlockId thread_block,
      std::uint32_t program_order,
      const std::vector<OperationId>& dependencies,
      const BufferSlice& source,
      const BufferSlice& destination,
      MemoryOrder memory_order,
      OrderingDomainId ordering_domain,
      CompletionStage release_stage);

  static MicroOp get(
      OperationId id,
      DeviceId issuer,
      ThreadBlockId thread_block,
      std::uint32_t program_order,
      const std::vector<OperationId>& dependencies,
      const BufferSlice& source,
      const BufferSlice& destination,
      MemoryOrder memory_order,
      OrderingDomainId ordering_domain,
      CompletionStage release_stage);

  static MicroOp signal(
      OperationId id,
      DeviceId issuer,
      ThreadBlockId thread_block,
      std::uint32_t program_order,
      const std::vector<OperationId>& dependencies,
      DeviceId semaphore_owner,
      SemaphoreId semaphore,
      std::uint64_t value,
      MemoryOrder memory_order,
      OrderingDomainId ordering_domain,
      CompletionStage release_stage);

  static MicroOp wait(
      OperationId id,
      DeviceId issuer,
      ThreadBlockId thread_block,
      std::uint32_t program_order,
      const std::vector<OperationId>& dependencies,
      DeviceId semaphore_owner,
      SemaphoreId semaphore,
      std::uint64_t value,
      MemoryOrder memory_order,
      OrderingDomainId ordering_domain,
      CompletionStage release_stage);

  static MicroOp reduce(
      OperationId id,
      DeviceId issuer,
      ThreadBlockId thread_block,
      std::uint32_t program_order,
      const std::vector<OperationId>& dependencies,
      const BufferSlice& source,
      const BufferSlice& destination,
      ReductionKind reduction,
      MemoryOrder memory_order,
      OrderingDomainId ordering_domain,
      CompletionStage release_stage);

  ValidationErrors validate() const;
  void require_valid() const;
};

struct CollectivePlan {
  CollectivePlanId id;
  CollectiveKind kind;
  ParallelGroup group;
  std::uint64_t payload_bytes;
  std::vector<MicroOp> operations;

  ValidationErrors validate() const;
  void require_valid() const;
};

}  // namespace contracts
}  // namespace simai

#endif
