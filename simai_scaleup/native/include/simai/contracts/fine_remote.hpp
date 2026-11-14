#ifndef SIMAI_CONTRACTS_FINE_REMOTE_HPP
#define SIMAI_CONTRACTS_FINE_REMOTE_HPP

#include "simai/contracts/completion_stage.hpp"
#include "simai/contracts/fine_gpu.hpp"

namespace simai {
namespace contracts {

struct FineRemoteRequest {
  MemoryRequestId id;
  KernelInstanceId kernel_instance;
  WorkgroupInstanceId workgroup_instance;
  WavefrontInstanceId wavefront_instance;
  GpuInstructionId instruction;
  DeviceId issuer;
  FineMemoryRequestKind kind;
  bool has_source;
  FineMemoryReference source;
  bool has_destination;
  FineMemoryReference destination;
  bool has_semaphore;
  DeviceId semaphore_owner;
  SemaphoreId semaphore;
  std::uint64_t semaphore_generation;
  CompletionStage completion_stage;
  SimTimeNs issue_ns;

  ValidationErrors validate() const;
  void require_valid() const;
};

}  // namespace contracts
}  // namespace simai

#endif
