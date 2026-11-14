#ifndef SIMAI_RUNTIME_FINE_COMPUTE_HPP
#define SIMAI_RUNTIME_FINE_COMPUTE_HPP

#include <cstdint>
#include <vector>

#include "simai/contracts/fine_gpu.hpp"

namespace simai {
namespace runtime {

struct FineComputeOperationTemplate {
  contracts::FineGpuOperation operation;
  std::uint64_t workgroup_byte_stride;
  std::uint64_t wavefront_byte_stride;
};

struct FineComputeKernelTemplate {
  contracts::KernelId kernel;
  contracts::DeviceId device;
  std::uint32_t workgroup_count;
  std::uint32_t wavefronts_per_workgroup;
  std::uint32_t workgroup_residency_slots;
  std::vector<FineComputeOperationTemplate> operations;

  contracts::ValidationErrors validate() const;
  void require_valid() const;
};

contracts::FineKernelDefinition lower_compute_kernel(
    const FineComputeKernelTemplate& description);

}  // namespace runtime
}  // namespace simai

#endif
