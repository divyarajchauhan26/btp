#ifndef SIMAI_RUNTIME_FINE_INSTRUCTION_STREAM_HPP
#define SIMAI_RUNTIME_FINE_INSTRUCTION_STREAM_HPP

#include <cstdint>

#include "simai/contracts/fine_gpu.hpp"

namespace simai {
namespace runtime {

struct FineInstructionGeneratorConfig {
  std::uint64_t cache_line_bytes;
  std::uint32_t memcpy_unroll;
  contracts::BufferId register_buffer;

  contracts::ValidationErrors validate() const;
  void require_valid() const;
};

struct FineMemoryPartition {
  bool empty;
  contracts::FineMemoryReference reference;
};

FineMemoryPartition partition_fine_memory_span(
    const contracts::FineMemoryReference& span,
    std::uint32_t wavefront_index,
    std::uint32_t wavefront_count,
    std::uint64_t cache_line_bytes);

contracts::FineGpuOperation partition_fine_operation(
    const contracts::FineGpuOperation& operation,
    std::uint32_t wavefront_index,
    std::uint32_t wavefront_count,
    std::uint64_t cache_line_bytes);

class FineInstructionStream {
 public:
  FineInstructionStream(
      const contracts::FineGpuOperation& operation,
      const FineInstructionGeneratorConfig& config,
      contracts::GpuInstructionId first_instruction);

  bool next(contracts::FineGpuInstruction& instruction);
  std::uint64_t instruction_count() const;
  std::uint64_t emitted_count() const { return emitted_; }

 private:
  enum class MemcpyPhase { Load, Wait, Store };

  contracts::FineMemoryReference line_slice(
      const contracts::FineMemoryReference& span,
      std::uint64_t line_index) const;
  contracts::FineMemoryReference register_slice(
      std::uint64_t line_index, std::uint64_t length) const;
  contracts::FineGpuInstruction base_instruction(
      contracts::FineGpuInstructionKind kind) const;
  void advance_instruction();

  contracts::FineGpuOperation operation_;
  FineInstructionGeneratorConfig config_;
  contracts::GpuInstructionId first_instruction_;
  std::uint64_t line_count_;
  std::uint64_t emitted_;
  std::uint64_t simple_line_;
  std::uint64_t group_start_;
  std::uint64_t group_size_;
  std::uint64_t group_cursor_;
  MemcpyPhase memcpy_phase_;
  bool single_emitted_;
};

}  // namespace runtime
}  // namespace simai

#endif
