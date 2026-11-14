#ifndef SIMAI_CONTRACTS_FINE_MEMORY_HPP
#define SIMAI_CONTRACTS_FINE_MEMORY_HPP

#include "simai/contracts/fine_gpu.hpp"

namespace simai {
namespace contracts {

struct FineMemoryRequest {
  MemoryRequestId id;
  KernelInstanceId kernel_instance;
  WorkgroupInstanceId workgroup_instance;
  WavefrontInstanceId wavefront_instance;
  GpuInstructionId instruction;
  DeviceId issuer;
  FineMemoryRequestKind kind;
  FineMemoryReference target;
  SimTimeNs issue_ns;

  ValidationErrors validate() const;
  void require_valid() const;
};

struct FineMemoryCompletion {
  FineMemoryRequest request;
  SimTimeNs service_start_ns;
  SimTimeNs service_end_ns;
  SimTimeNs completion_ns;

  ValidationErrors validate() const;
  void require_valid() const;
};

}  // namespace contracts
}  // namespace simai

#endif
