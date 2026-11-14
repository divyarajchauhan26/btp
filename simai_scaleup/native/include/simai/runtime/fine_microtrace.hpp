#ifndef SIMAI_RUNTIME_FINE_MICROTRACE_HPP
#define SIMAI_RUNTIME_FINE_MICROTRACE_HPP

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

#include "simai/contracts/fine_gpu.hpp"

namespace simai {
namespace runtime {

enum class FineMicrotraceInstructionKind { Load, Store };

enum class FineMicrotraceTimestampSemantics {
  Unspecified,
  KernelElapsedCycles,
  DeltaCycles
};

struct FineMicrotraceInstructionTypeMapping {
  std::uint64_t encoded_type;
  FineMicrotraceInstructionKind kind;
};

struct FineMicrotraceAllocation {
  std::uint64_t base_address;
  std::uint64_t size_bytes;
  contracts::DeviceId owner;
  contracts::BufferId buffer;
  std::uint64_t alignment_bytes;
};

struct FineMicrotraceImportConfig {
  contracts::KernelId kernel;
  contracts::DeviceId device;
  contracts::FineKernelKind kernel_kind;
  std::uint32_t block_dimension_x;
  std::uint32_t block_dimension_y;
  std::uint32_t block_dimension_z;
  std::uint32_t warp_size;
  std::uint32_t workgroup_residency_slots;
  std::uint64_t access_bytes;
  contracts::BufferId register_buffer;
  FineMicrotraceTimestampSemantics timestamp_semantics;
  std::vector<FineMicrotraceInstructionTypeMapping> instruction_types;
  std::vector<FineMicrotraceAllocation> allocations;
};

struct FineMicrotraceRecord {
  std::uint64_t probe;
  std::uint32_t block_x;
  std::uint32_t block_y;
  std::uint32_t block_z;
  std::uint32_t thread_x;
  std::uint32_t thread_y;
  std::uint32_t thread_z;
  std::uint64_t clock;
  std::uint64_t address;
  std::uint64_t encoded_instruction_type;
};

struct FineMicrotraceAccess {
  std::uint64_t probe;
  std::uint32_t block_x;
  std::uint32_t block_y;
  std::uint32_t block_z;
  std::uint64_t warp;
  std::uint64_t clock;
  FineMicrotraceInstructionKind kind;
  std::uint32_t active_lanes;
  contracts::FineMemoryReference memory;
};

struct FineMicrotraceImportResult {
  std::vector<FineMicrotraceRecord> records;
  std::vector<FineMicrotraceAccess> accesses;
  FineMicrotraceTimestampSemantics timestamp_semantics;
  contracts::FineKernelDefinition kernel;
};

std::vector<FineMicrotraceRecord> parse_fine_microtrace_tsv(
    std::istream& input);
std::vector<FineMicrotraceRecord> parse_fine_microtrace_tsv_file(
    const std::string& path);
FineMicrotraceImportResult import_fine_microtrace(
    const std::vector<FineMicrotraceRecord>& records,
    const FineMicrotraceImportConfig& config);
FineMicrotraceImportResult import_fine_microtrace_tsv(
    std::istream& input,
    const FineMicrotraceImportConfig& config);
FineMicrotraceImportResult import_fine_microtrace_tsv_file(
    const std::string& path,
    const FineMicrotraceImportConfig& config);

}  // namespace runtime
}  // namespace simai

#endif
