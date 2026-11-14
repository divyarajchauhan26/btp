#include "simai/contracts/fine_gpu.hpp"

#include <limits>
#include <unordered_set>

namespace simai {
namespace contracts {
namespace {

FineMemoryReference empty_reference() {
  return FineMemoryReference{
      DeviceId(), FineMemorySpace::RegisterFile, BufferId(), 0, 0, 1};
}

bool is_power_of_two(std::uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

void append_all(ValidationErrors& destination, const ValidationErrors& source) {
  destination.insert(destination.end(), source.begin(), source.end());
}

FineGpuOperation base_operation(
    GpuOperationId id, FineGpuOperationKind kind) {
  return FineGpuOperation{
      id,
      kind,
      false,
      empty_reference(),
      false,
      empty_reference(),
      false,
      DeviceId(),
      SemaphoreId(),
      0,
      false,
      BarrierId(),
      0,
      0};
}

bool is_data_memory(FineMemorySpace space) {
  return space == FineMemorySpace::L1 || space == FineMemorySpace::L2 ||
      space == FineMemorySpace::LocalHbm ||
      space == FineMemorySpace::RemoteHbm;
}

bool reference_matches_kernel_device(
    const FineMemoryReference& reference, DeviceId device) {
  if (reference.space == FineMemorySpace::RemoteHbm) {
    return reference.owner != device;
  }
  if (reference.space == FineMemorySpace::SemaphoreMemory) {
    return true;
  }
  return reference.owner == device;
}

}  // namespace

ValidationErrors FineMemoryReference::validate() const {
  ValidationErrors errors;
  if (!owner.valid() || !buffer.valid()) {
    append_error(
        errors,
        ValidationCode::InvalidIdentity,
        "fine memory owner and buffer identity must be nonzero");
  }
  if (length_bytes == 0 ||
      offset_bytes > std::numeric_limits<std::uint64_t>::max() - length_bytes) {
    append_error(
        errors,
        ValidationCode::InvalidMemoryReference,
        "fine memory byte range must be nonzero and non-overflowing");
  }
  if (!is_power_of_two(alignment_bytes) ||
      offset_bytes % alignment_bytes != 0) {
    append_error(
        errors,
        ValidationCode::InvalidMemoryReference,
        "fine memory alignment must be a power of two and divide the offset");
  }
  return errors;
}

void FineMemoryReference::require_valid() const {
  require_no_errors(validate(), "FineMemoryReference");
}

ValidationErrors FineGpuInstruction::validate() const {
  ValidationErrors errors;
  if (!id.valid() || !parent_operation.valid()) {
    append_error(
        errors,
        ValidationCode::InvalidIdentity,
        "fine instruction and parent operation identities must be nonzero");
  }
  if (has_source) append_all(errors, source.validate());
  if (has_destination) append_all(errors, destination.validate());

  const bool memory_instruction =
      kind == FineGpuInstructionKind::Load ||
      kind == FineGpuInstructionKind::Store;
  if (memory_instruction && (!has_source || !has_destination ||
                             source.length_bytes != destination.length_bytes)) {
    append_error(
        errors,
        ValidationCode::InvalidGpuOperation,
        "fine Load/Store instruction requires equal-sized source and destination");
  }
  const bool semaphore_instruction =
      kind == FineGpuInstructionKind::SemaphoreAcquire ||
      kind == FineGpuInstructionKind::SemaphoreRelease;
  if (semaphore_instruction &&
      (!has_semaphore || !semaphore_owner.valid() || !semaphore.valid() ||
       semaphore_generation == 0)) {
    append_error(
        errors,
        ValidationCode::InvalidGpuOperation,
        "fine semaphore instruction requires valid semaphore state");
  } else if (!semaphore_instruction &&
             (has_semaphore || semaphore_owner.valid() || semaphore.valid() ||
              semaphore_generation != 0)) {
    append_error(
        errors,
        ValidationCode::InvalidGpuOperation,
        "non-semaphore instruction contains semaphore state");
  }
  if (kind == FineGpuInstructionKind::Reduce && reduce_cycles == 0) {
    append_error(
        errors,
        ValidationCode::InvalidGpuOperation,
        "fine Reduce instruction requires nonzero cycles");
  } else if (kind != FineGpuInstructionKind::Reduce && reduce_cycles != 0) {
    append_error(
        errors,
        ValidationCode::InvalidGpuOperation,
        "non-Reduce instruction contains reduction cycles");
  }
  if (kind != FineGpuInstructionKind::Waitcnt && wait_threshold != 0) {
    append_error(
        errors,
        ValidationCode::InvalidGpuOperation,
        "non-Waitcnt instruction contains a wait threshold");
  }
  if (kind != FineGpuInstructionKind::TraceWaitUntil &&
      trace_timestamp_ns != 0) {
    append_error(
        errors,
        ValidationCode::InvalidGpuOperation,
        "non-trace-wait instruction contains a trace timestamp");
  }
  return errors;
}

void FineGpuInstruction::require_valid() const {
  require_no_errors(validate(), "FineGpuInstruction");
}

FineGpuOperation FineGpuOperation::load(
    GpuOperationId id,
    const FineMemoryReference& source,
    const FineMemoryReference& register_destination) {
  FineGpuOperation operation = base_operation(id, FineGpuOperationKind::Load);
  operation.has_source = true;
  operation.source = source;
  operation.has_destination = true;
  operation.destination = register_destination;
  return operation;
}

FineGpuOperation FineGpuOperation::store(
    GpuOperationId id,
    const FineMemoryReference& register_source,
    const FineMemoryReference& destination) {
  FineGpuOperation operation = base_operation(id, FineGpuOperationKind::Store);
  operation.has_source = true;
  operation.source = register_source;
  operation.has_destination = true;
  operation.destination = destination;
  return operation;
}

FineGpuOperation FineGpuOperation::memcpy(
    GpuOperationId id,
    const FineMemoryReference& source,
    const FineMemoryReference& destination) {
  FineGpuOperation operation = base_operation(id, FineGpuOperationKind::Memcpy);
  operation.has_source = true;
  operation.source = source;
  operation.has_destination = true;
  operation.destination = destination;
  return operation;
}

FineGpuOperation FineGpuOperation::semaphore_acquire(
    GpuOperationId id,
    DeviceId semaphore_owner,
    SemaphoreId semaphore,
    std::uint64_t generation,
    const FineMemoryReference& location) {
  FineGpuOperation operation =
      base_operation(id, FineGpuOperationKind::SemaphoreAcquire);
  operation.has_source = true;
  operation.source = location;
  operation.has_semaphore = true;
  operation.semaphore_owner = semaphore_owner;
  operation.semaphore = semaphore;
  operation.semaphore_generation = generation;
  return operation;
}

FineGpuOperation FineGpuOperation::semaphore_release(
    GpuOperationId id,
    DeviceId semaphore_owner,
    SemaphoreId semaphore,
    std::uint64_t generation,
    const FineMemoryReference& location) {
  FineGpuOperation operation =
      base_operation(id, FineGpuOperationKind::SemaphoreRelease);
  operation.has_destination = true;
  operation.destination = location;
  operation.has_semaphore = true;
  operation.semaphore_owner = semaphore_owner;
  operation.semaphore = semaphore;
  operation.semaphore_generation = generation;
  return operation;
}

FineGpuOperation FineGpuOperation::reduce(
    GpuOperationId id, std::uint64_t cycles) {
  FineGpuOperation operation = base_operation(id, FineGpuOperationKind::Reduce);
  operation.reduce_cycles = cycles;
  return operation;
}

FineGpuOperation FineGpuOperation::trace_wait_until(
    GpuOperationId id, SimTimeNs timestamp_ns) {
  FineGpuOperation operation =
      base_operation(id, FineGpuOperationKind::TraceWaitUntil);
  operation.trace_timestamp_ns = timestamp_ns;
  return operation;
}

FineGpuOperation FineGpuOperation::workgroup_barrier(
    GpuOperationId id, BarrierId barrier) {
  FineGpuOperation operation =
      base_operation(id, FineGpuOperationKind::WorkgroupBarrier);
  operation.has_barrier = true;
  operation.barrier = barrier;
  return operation;
}

FineGpuOperation FineGpuOperation::kernel_barrier(
    GpuOperationId id, BarrierId barrier) {
  FineGpuOperation operation =
      base_operation(id, FineGpuOperationKind::KernelBarrier);
  operation.has_barrier = true;
  operation.barrier = barrier;
  return operation;
}

ValidationErrors FineGpuOperation::validate() const {
  ValidationErrors errors;
  if (!id.valid()) {
    append_error(
        errors,
        ValidationCode::InvalidIdentity,
        "fine GPU operation identity must be nonzero");
  }
  if (has_source) append_all(errors, source.validate());
  if (has_destination) append_all(errors, destination.validate());

  const bool semaphore_kind =
      kind == FineGpuOperationKind::SemaphoreAcquire ||
      kind == FineGpuOperationKind::SemaphoreRelease;
  const bool barrier_kind =
      kind == FineGpuOperationKind::WorkgroupBarrier ||
      kind == FineGpuOperationKind::KernelBarrier;

  if (kind == FineGpuOperationKind::Load &&
      (!has_source || !has_destination ||
       !is_data_memory(source.space) ||
       destination.space != FineMemorySpace::RegisterFile ||
       source.length_bytes != destination.length_bytes)) {
    append_error(
        errors,
        ValidationCode::InvalidGpuOperation,
        "Load requires equal-sized data-memory source and register destination");
  } else if (kind == FineGpuOperationKind::Store &&
             (!has_source || !has_destination ||
              source.space != FineMemorySpace::RegisterFile ||
              !is_data_memory(destination.space) ||
              source.length_bytes != destination.length_bytes)) {
    append_error(
        errors,
        ValidationCode::InvalidGpuOperation,
        "Store requires equal-sized register source and data-memory destination");
  } else if (kind == FineGpuOperationKind::Memcpy &&
             (!has_source || !has_destination ||
              !is_data_memory(source.space) ||
              !is_data_memory(destination.space) ||
              source.length_bytes != destination.length_bytes)) {
    append_error(
        errors,
        ValidationCode::InvalidGpuOperation,
        "Memcpy requires equal-sized data-memory source and destination");
  }

  if (semaphore_kind) {
    const FineMemoryReference& location =
        kind == FineGpuOperationKind::SemaphoreAcquire ? source : destination;
    if (!has_semaphore || !semaphore_owner.valid() || !semaphore.valid() ||
        semaphore_generation == 0 ||
        location.space != FineMemorySpace::SemaphoreMemory ||
        location.owner != semaphore_owner) {
      append_error(
          errors,
          ValidationCode::InvalidGpuOperation,
          "semaphore operation requires a valid generation and owner location");
    }
  } else if (has_semaphore || semaphore_owner.valid() || semaphore.valid() ||
             semaphore_generation != 0) {
    append_error(
        errors,
        ValidationCode::InvalidGpuOperation,
        "non-semaphore operation contains semaphore state");
  }

  if (kind == FineGpuOperationKind::Reduce && reduce_cycles == 0) {
    append_error(
        errors,
        ValidationCode::InvalidGpuOperation,
        "Reduce requires nonzero modeled cycles");
  } else if (kind != FineGpuOperationKind::Reduce && reduce_cycles != 0) {
    append_error(
        errors,
        ValidationCode::InvalidGpuOperation,
        "non-Reduce operation contains reduction cycles");
  }
  if (kind != FineGpuOperationKind::TraceWaitUntil &&
      trace_timestamp_ns != 0) {
    append_error(
        errors,
        ValidationCode::InvalidGpuOperation,
        "non-trace-wait operation contains a trace timestamp");
  }

  if (barrier_kind && (!has_barrier || !barrier.valid())) {
    append_error(
        errors,
        ValidationCode::InvalidGpuOperation,
        "barrier operation requires a valid barrier identity");
  } else if (!barrier_kind && (has_barrier || barrier.valid())) {
    append_error(
        errors,
        ValidationCode::InvalidGpuOperation,
        "non-barrier operation contains barrier state");
  }
  return errors;
}

void FineGpuOperation::require_valid() const {
  require_no_errors(validate(), "FineGpuOperation");
}

ValidationErrors FineWavefrontProgram::validate() const {
  ValidationErrors errors;
  if (!id.valid()) {
    append_error(
        errors,
        ValidationCode::InvalidIdentity,
        "fine wavefront identity must be nonzero");
  }
  if (operations.empty()) {
    append_error(
        errors,
        ValidationCode::InvalidGpuKernel,
        "fine wavefront program must contain operations");
  }
  std::unordered_set<GpuOperationId> seen;
  for (const FineGpuOperation& operation : operations) {
    append_all(errors, operation.validate());
    if (!seen.insert(operation.id).second) {
      append_error(
          errors,
          ValidationCode::DuplicateIdentity,
          "fine wavefront contains a duplicate operation identity");
    }
  }
  return errors;
}

void FineWavefrontProgram::require_valid() const {
  require_no_errors(validate(), "FineWavefrontProgram");
}

ValidationErrors FineWorkgroupResources::validate() const {
  ValidationErrors errors;
  if (residency_slots == 0) {
    append_error(
        errors,
        ValidationCode::InvalidResourceRequirement,
        "fine workgroup requires at least one residency slot");
  }
  return errors;
}

void FineWorkgroupResources::require_valid() const {
  require_no_errors(validate(), "FineWorkgroupResources");
}

ValidationErrors FineWorkgroupDefinition::validate() const {
  ValidationErrors errors;
  if (!id.valid() || !source_thread_block.valid()) {
    append_error(
        errors,
        ValidationCode::InvalidIdentity,
        "fine workgroup and source threadblock identities must be nonzero");
  }
  append_all(errors, resources.validate());
  if (wavefronts.empty()) {
    append_error(
        errors,
        ValidationCode::InvalidGpuKernel,
        "fine workgroup must contain wavefronts");
  }
  std::unordered_set<WavefrontId> seen;
  for (const FineWavefrontProgram& wavefront : wavefronts) {
    append_all(errors, wavefront.validate());
    if (!seen.insert(wavefront.id).second) {
      append_error(
          errors,
          ValidationCode::DuplicateIdentity,
          "fine workgroup contains a duplicate wavefront identity");
    }
  }
  return errors;
}

void FineWorkgroupDefinition::require_valid() const {
  require_no_errors(validate(), "FineWorkgroupDefinition");
}

ValidationErrors FineKernelDefinition::validate() const {
  ValidationErrors errors;
  if (!id.valid() || !device.valid()) {
    append_error(
        errors,
        ValidationCode::InvalidIdentity,
        "fine kernel and device identities must be nonzero");
  }
  if (workgroups.empty()) {
    append_error(
        errors,
        ValidationCode::InvalidGpuKernel,
        "fine kernel must contain workgroups");
  }
  std::unordered_set<WorkgroupId> workgroup_ids;
  std::unordered_set<WavefrontId> wavefront_ids;
  std::unordered_set<GpuOperationId> operation_ids;
  for (const FineWorkgroupDefinition& workgroup : workgroups) {
    append_all(errors, workgroup.validate());
    if (!workgroup_ids.insert(workgroup.id).second) {
      append_error(
          errors,
          ValidationCode::DuplicateIdentity,
          "fine kernel contains a duplicate workgroup identity");
    }
    for (const FineWavefrontProgram& wavefront : workgroup.wavefronts) {
      if (!wavefront_ids.insert(wavefront.id).second) {
        append_error(
            errors,
            ValidationCode::DuplicateIdentity,
            "fine kernel contains a globally duplicate wavefront identity");
      }
      for (const FineGpuOperation& operation : wavefront.operations) {
        if (!operation_ids.insert(operation.id).second) {
          append_error(
              errors,
              ValidationCode::DuplicateIdentity,
              "fine kernel contains a globally duplicate operation identity");
        }
        if ((operation.has_source &&
             !reference_matches_kernel_device(operation.source, device)) ||
            (operation.has_destination &&
             !reference_matches_kernel_device(operation.destination, device))) {
          append_error(
              errors,
              ValidationCode::InvalidMemoryReference,
              "fine operation memory space does not match its kernel device");
        }
      }
    }
  }
  return errors;
}

void FineKernelDefinition::require_valid() const {
  require_no_errors(validate(), "FineKernelDefinition");
}

}  // namespace contracts
}  // namespace simai
