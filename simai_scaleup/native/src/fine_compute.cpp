#include "simai/runtime/fine_compute.hpp"

#include <limits>
#include <stdexcept>

namespace simai {
namespace runtime {
namespace {

bool is_compute_operation(contracts::FineGpuOperationKind kind) {
  return kind == contracts::FineGpuOperationKind::Load ||
      kind == contracts::FineGpuOperationKind::Store ||
      kind == contracts::FineGpuOperationKind::Memcpy ||
      kind == contracts::FineGpuOperationKind::Reduce;
}

bool is_local_compute_space(contracts::FineMemorySpace space) {
  return space == contracts::FineMemorySpace::RegisterFile ||
      space == contracts::FineMemorySpace::L1 ||
      space == contracts::FineMemorySpace::L2 ||
      space == contracts::FineMemorySpace::LocalHbm;
}

std::uint64_t checked_offset(
    std::uint64_t base,
    std::uint64_t workgroup,
    std::uint64_t workgroup_stride,
    std::uint64_t wavefront,
    std::uint64_t wavefront_stride) {
  if (workgroup != 0 &&
      workgroup_stride >
          (std::numeric_limits<std::uint64_t>::max() - base) / workgroup) {
    throw std::overflow_error("fine compute workgroup offset overflow");
  }
  const std::uint64_t workgroup_offset = base + workgroup * workgroup_stride;
  if (wavefront != 0 &&
      wavefront_stride >
          (std::numeric_limits<std::uint64_t>::max() - workgroup_offset) /
              wavefront) {
    throw std::overflow_error("fine compute wavefront offset overflow");
  }
  return workgroup_offset + wavefront * wavefront_stride;
}

void shift_reference(
    contracts::FineMemoryReference& reference,
    std::uint64_t workgroup,
    std::uint64_t workgroup_stride,
    std::uint64_t wavefront,
    std::uint64_t wavefront_stride) {
  reference.offset_bytes = checked_offset(
      reference.offset_bytes,
      workgroup,
      workgroup_stride,
      wavefront,
      wavefront_stride);
}

}  // namespace

contracts::ValidationErrors FineComputeKernelTemplate::validate() const {
  contracts::ValidationErrors errors;
  if (!kernel.valid() || !device.valid()) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidIdentity,
        "fine compute template identities must be nonzero");
  }
  if (workgroup_count == 0 || wavefronts_per_workgroup == 0 ||
      workgroup_residency_slots == 0 || operations.empty()) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidResourceRequirement,
        "fine compute template dimensions and operations must be nonzero");
  }
  for (const FineComputeOperationTemplate& entry : operations) {
    const contracts::ValidationErrors operation_errors =
        entry.operation.validate();
    errors.insert(
        errors.end(), operation_errors.begin(), operation_errors.end());
    if (!is_compute_operation(entry.operation.kind)) {
      contracts::append_error(
          errors,
          contracts::ValidationCode::InvalidValue,
          "fine compute template supports Load, Store, Memcpy, and Reduce only");
    }
    if (entry.operation.has_source &&
        (!is_local_compute_space(entry.operation.source.space) ||
         entry.operation.source.owner != device)) {
      contracts::append_error(
          errors,
          contracts::ValidationCode::InvalidBuffer,
          "fine compute source must use issuer-local memory");
    }
    if (entry.operation.has_destination &&
        (!is_local_compute_space(entry.operation.destination.space) ||
         entry.operation.destination.owner != device)) {
      contracts::append_error(
          errors,
          contracts::ValidationCode::InvalidBuffer,
          "fine compute destination must use issuer-local memory");
    }
  }
  return errors;
}

void FineComputeKernelTemplate::require_valid() const {
  contracts::require_no_errors(validate(), "FineComputeKernelTemplate");
}

contracts::FineKernelDefinition lower_compute_kernel(
    const FineComputeKernelTemplate& description) {
  description.require_valid();
  std::vector<contracts::FineWorkgroupDefinition> workgroups;
  std::uint64_t next_wavefront = 1;
  std::uint64_t next_operation = 1;
  for (std::uint32_t workgroup_index = 0;
       workgroup_index < description.workgroup_count;
       ++workgroup_index) {
    std::vector<contracts::FineWavefrontProgram> wavefronts;
    for (std::uint32_t wavefront_index = 0;
         wavefront_index < description.wavefronts_per_workgroup;
         ++wavefront_index) {
      std::vector<contracts::FineGpuOperation> operations;
      for (const FineComputeOperationTemplate& entry : description.operations) {
        contracts::FineGpuOperation operation = entry.operation;
        operation.id = contracts::GpuOperationId(next_operation++);
        if (operation.has_source) {
          shift_reference(
              operation.source,
              workgroup_index,
              entry.workgroup_byte_stride,
              wavefront_index,
              entry.wavefront_byte_stride);
        }
        if (operation.has_destination) {
          shift_reference(
              operation.destination,
              workgroup_index,
              entry.workgroup_byte_stride,
              wavefront_index,
              entry.wavefront_byte_stride);
        }
        operation.require_valid();
        operations.push_back(operation);
      }
      wavefronts.push_back(contracts::FineWavefrontProgram{
          contracts::WavefrontId(next_wavefront++), operations});
    }
    workgroups.push_back(contracts::FineWorkgroupDefinition{
        contracts::WorkgroupId(workgroup_index + 1),
        contracts::ThreadBlockId(workgroup_index + 1),
        contracts::FineWorkgroupResources{
            description.workgroup_residency_slots},
        wavefronts});
  }
  contracts::FineKernelDefinition kernel{
      description.kernel,
      description.device,
      contracts::FineKernelKind::Compute,
      workgroups};
  kernel.require_valid();
  return kernel;
}

}  // namespace runtime
}  // namespace simai
