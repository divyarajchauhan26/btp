#include "simai/runtime/fine_instruction_stream.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace simai {
namespace runtime {
namespace {

using contracts::FineGpuInstruction;
using contracts::FineGpuInstructionKind;
using contracts::FineGpuOperation;
using contracts::FineGpuOperationKind;
using contracts::FineMemoryReference;
using contracts::FineMemorySpace;

std::uint64_t divide_round_up(std::uint64_t value, std::uint64_t divisor) {
  return value / divisor + (value % divisor == 0 ? 0 : 1);
}

FineMemoryReference empty_reference() {
  return FineMemoryReference{
      contracts::DeviceId(),
      FineMemorySpace::RegisterFile,
      contracts::BufferId(),
      0,
      0,
      1};
}

bool operation_has_lines(const FineGpuOperation& operation) {
  return operation.kind == FineGpuOperationKind::Load ||
      operation.kind == FineGpuOperationKind::Store ||
      operation.kind == FineGpuOperationKind::Memcpy;
}

const FineMemoryReference& operation_span(const FineGpuOperation& operation) {
  return operation.kind == FineGpuOperationKind::Store
      ? operation.destination
      : operation.source;
}

std::uint64_t checked_add(
    std::uint64_t lhs, std::uint64_t rhs, const char* message) {
  if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs) {
    throw std::overflow_error(message);
  }
  return lhs + rhs;
}

}  // namespace

contracts::ValidationErrors FineInstructionGeneratorConfig::validate() const {
  contracts::ValidationErrors errors;
  if (cache_line_bytes == 0 ||
      (cache_line_bytes & (cache_line_bytes - 1)) != 0) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "fine cache-line size must be a nonzero power of two");
  }
  if (memcpy_unroll == 0) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "fine Memcpy unroll factor must be nonzero");
  }
  if (!register_buffer.valid()) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidIdentity,
        "fine instruction generator register buffer must be nonzero");
  }
  return errors;
}

void FineInstructionGeneratorConfig::require_valid() const {
  contracts::require_no_errors(validate(), "FineInstructionGeneratorConfig");
}

FineMemoryPartition partition_fine_memory_span(
    const FineMemoryReference& span,
    std::uint32_t wavefront_index,
    std::uint32_t wavefront_count,
    std::uint64_t cache_line_bytes) {
  span.require_valid();
  if (wavefront_count == 0 || wavefront_index >= wavefront_count ||
      cache_line_bytes == 0 ||
      (cache_line_bytes & (cache_line_bytes - 1)) != 0) {
    throw std::invalid_argument("invalid fine wavefront memory partition");
  }
  const std::uint64_t line_count =
      divide_round_up(span.length_bytes, cache_line_bytes);
  const std::uint64_t base = line_count / wavefront_count;
  const std::uint64_t remainder = line_count % wavefront_count;
  const std::uint64_t assigned =
      base + (wavefront_index < remainder ? 1 : 0);
  if (assigned == 0) {
    return FineMemoryPartition{true, empty_reference()};
  }
  const std::uint64_t first_line =
      static_cast<std::uint64_t>(wavefront_index) * base +
      std::min<std::uint64_t>(wavefront_index, remainder);
  const std::uint64_t byte_start = checked_add(
      span.offset_bytes,
      first_line * cache_line_bytes,
      "fine partition offset overflow");
  const std::uint64_t relative_start = first_line * cache_line_bytes;
  const std::uint64_t available = span.length_bytes - relative_start;
  const std::uint64_t byte_length =
      std::min(available, assigned * cache_line_bytes);
  FineMemoryReference result = span;
  result.offset_bytes = byte_start;
  result.length_bytes = byte_length;
  return FineMemoryPartition{false, result};
}

FineGpuOperation partition_fine_operation(
    const FineGpuOperation& operation,
    std::uint32_t wavefront_index,
    std::uint32_t wavefront_count,
    std::uint64_t cache_line_bytes) {
  operation.require_valid();
  if (!operation_has_lines(operation)) return operation;

  FineGpuOperation partitioned = operation;
  const FineMemoryPartition primary = partition_fine_memory_span(
      operation_span(operation),
      wavefront_index,
      wavefront_count,
      cache_line_bytes);
  if (primary.empty) {
    throw std::invalid_argument(
        "wavefront has no cache lines in fine operation partition");
  }
  const std::uint64_t original_offset = operation_span(operation).offset_bytes;
  const std::uint64_t relative_offset =
      primary.reference.offset_bytes - original_offset;
  if (partitioned.has_source) {
    partitioned.source.offset_bytes = checked_add(
        partitioned.source.offset_bytes,
        relative_offset,
        "fine source partition offset overflow");
    partitioned.source.length_bytes = primary.reference.length_bytes;
  }
  if (partitioned.has_destination) {
    partitioned.destination.offset_bytes = checked_add(
        partitioned.destination.offset_bytes,
        relative_offset,
        "fine destination partition offset overflow");
    partitioned.destination.length_bytes = primary.reference.length_bytes;
  }
  partitioned.require_valid();
  return partitioned;
}

FineInstructionStream::FineInstructionStream(
    const FineGpuOperation& operation,
    const FineInstructionGeneratorConfig& config,
    contracts::GpuInstructionId first_instruction)
    : operation_(operation),
      config_(config),
      first_instruction_(first_instruction),
      line_count_(0),
      emitted_(0),
      simple_line_(0),
      group_start_(0),
      group_size_(0),
      group_cursor_(0),
      memcpy_phase_(MemcpyPhase::Load),
      single_emitted_(false) {
  operation_.require_valid();
  config_.require_valid();
  if (!first_instruction_.valid()) {
    throw std::invalid_argument("first fine instruction identity must be nonzero");
  }
  if (operation_has_lines(operation_)) {
    line_count_ = divide_round_up(
        operation_span(operation_).length_bytes, config_.cache_line_bytes);
  }
}

FineMemoryReference FineInstructionStream::line_slice(
    const FineMemoryReference& span, std::uint64_t line_index) const {
  const std::uint64_t relative = line_index * config_.cache_line_bytes;
  FineMemoryReference result = span;
  result.offset_bytes = checked_add(
      span.offset_bytes, relative, "fine instruction line offset overflow");
  result.length_bytes = std::min(
      config_.cache_line_bytes, span.length_bytes - relative);
  return result;
}

FineMemoryReference FineInstructionStream::register_slice(
    std::uint64_t line_index, std::uint64_t length) const {
  const FineMemoryReference& span = operation_span(operation_);
  const contracts::DeviceId issuer =
      span.space == FineMemorySpace::RemoteHbm && operation_.has_destination
      ? operation_.destination.owner
      : span.owner;
  return FineMemoryReference{
      issuer,
      FineMemorySpace::RegisterFile,
      config_.register_buffer,
      line_index * config_.cache_line_bytes,
      length,
      config_.cache_line_bytes};
}

FineGpuInstruction FineInstructionStream::base_instruction(
    FineGpuInstructionKind kind) const {
  return FineGpuInstruction{
      contracts::GpuInstructionId(
          checked_add(
              first_instruction_.value(),
              emitted_,
              "fine instruction identity overflow")),
      operation_.id,
      emitted_,
      kind,
      false,
      empty_reference(),
      false,
      empty_reference(),
      false,
      contracts::DeviceId(),
      contracts::SemaphoreId(),
      0,
      0,
      0,
      0};
}

void FineInstructionStream::advance_instruction() {
  ++emitted_;
}

bool FineInstructionStream::next(FineGpuInstruction& instruction) {
  if (operation_.kind == FineGpuOperationKind::Load) {
    if (simple_line_ == line_count_) return false;
    instruction = base_instruction(FineGpuInstructionKind::Load);
    instruction.has_source = true;
    instruction.source = line_slice(operation_.source, simple_line_);
    instruction.has_destination = true;
    instruction.destination = line_slice(operation_.destination, simple_line_);
    ++simple_line_;
  } else if (operation_.kind == FineGpuOperationKind::Store) {
    if (simple_line_ == line_count_) return false;
    instruction = base_instruction(FineGpuInstructionKind::Store);
    instruction.has_source = true;
    instruction.source = line_slice(operation_.source, simple_line_);
    instruction.has_destination = true;
    instruction.destination = line_slice(operation_.destination, simple_line_);
    ++simple_line_;
  } else if (operation_.kind == FineGpuOperationKind::Memcpy) {
    if (group_start_ == line_count_) return false;
    if (group_size_ == 0) {
      group_size_ = std::min<std::uint64_t>(
          config_.memcpy_unroll, line_count_ - group_start_);
    }
    const std::uint64_t line = group_start_ + group_cursor_;
    if (memcpy_phase_ == MemcpyPhase::Load) {
      instruction = base_instruction(FineGpuInstructionKind::Load);
      instruction.source = line_slice(operation_.source, line);
      instruction.destination = register_slice(
          line, instruction.source.length_bytes);
      instruction.has_source = true;
      instruction.has_destination = true;
      ++group_cursor_;
      if (group_cursor_ == group_size_) {
        group_cursor_ = 0;
        memcpy_phase_ = MemcpyPhase::Wait;
      }
    } else if (memcpy_phase_ == MemcpyPhase::Wait) {
      instruction = base_instruction(FineGpuInstructionKind::Waitcnt);
      instruction.wait_threshold = group_size_ - group_cursor_ - 1;
      memcpy_phase_ = MemcpyPhase::Store;
    } else {
      instruction = base_instruction(FineGpuInstructionKind::Store);
      instruction.destination = line_slice(operation_.destination, line);
      instruction.source = register_slice(
          line, instruction.destination.length_bytes);
      instruction.has_source = true;
      instruction.has_destination = true;
      ++group_cursor_;
      if (group_cursor_ == group_size_) {
        group_start_ += group_size_;
        group_size_ = 0;
        group_cursor_ = 0;
        memcpy_phase_ = MemcpyPhase::Load;
      } else {
        memcpy_phase_ = MemcpyPhase::Wait;
      }
    }
  } else if (operation_.kind == FineGpuOperationKind::SemaphoreAcquire ||
             operation_.kind == FineGpuOperationKind::SemaphoreRelease) {
    if (single_emitted_) return false;
    const bool acquire =
        operation_.kind == FineGpuOperationKind::SemaphoreAcquire;
    instruction = base_instruction(
        acquire ? FineGpuInstructionKind::SemaphoreAcquire
                : FineGpuInstructionKind::SemaphoreRelease);
    instruction.has_source = acquire;
    instruction.source = acquire ? operation_.source : empty_reference();
    instruction.has_destination = !acquire;
    instruction.destination =
        acquire ? empty_reference() : operation_.destination;
    instruction.has_semaphore = true;
    instruction.semaphore_owner = operation_.semaphore_owner;
    instruction.semaphore = operation_.semaphore;
    instruction.semaphore_generation = operation_.semaphore_generation;
    single_emitted_ = true;
  } else if (operation_.kind == FineGpuOperationKind::Reduce) {
    if (single_emitted_) return false;
    instruction = base_instruction(FineGpuInstructionKind::Reduce);
    instruction.reduce_cycles = operation_.reduce_cycles;
    single_emitted_ = true;
  } else if (operation_.kind == FineGpuOperationKind::TraceWaitUntil) {
    if (single_emitted_) return false;
    instruction = base_instruction(FineGpuInstructionKind::TraceWaitUntil);
    instruction.trace_timestamp_ns = operation_.trace_timestamp_ns;
    single_emitted_ = true;
  } else {
    return false;
  }
  instruction.require_valid();
  advance_instruction();
  return true;
}

std::uint64_t FineInstructionStream::instruction_count() const {
  if (operation_.kind == FineGpuOperationKind::Load ||
      operation_.kind == FineGpuOperationKind::Store) {
    return line_count_;
  }
  if (operation_.kind == FineGpuOperationKind::Memcpy) {
    if (line_count_ > std::numeric_limits<std::uint64_t>::max() / 3) {
      throw std::overflow_error("fine Memcpy instruction count overflow");
    }
    return line_count_ * 3;
  }
  if (operation_.kind == FineGpuOperationKind::SemaphoreAcquire ||
      operation_.kind == FineGpuOperationKind::SemaphoreRelease ||
      operation_.kind == FineGpuOperationKind::Reduce ||
      operation_.kind == FineGpuOperationKind::TraceWaitUntil) {
    return 1;
  }
  return 0;
}

}  // namespace runtime
}  // namespace simai
