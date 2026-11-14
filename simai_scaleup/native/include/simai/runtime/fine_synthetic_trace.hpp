#ifndef SIMAI_RUNTIME_FINE_SYNTHETIC_TRACE_HPP
#define SIMAI_RUNTIME_FINE_SYNTHETIC_TRACE_HPP

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

#include "simai/contracts/fine_gpu.hpp"

namespace simai {
namespace runtime {

enum class FineSyntheticLocality { Local, Remote };

enum class FineSyntheticTimestampSemantics { EarliestIssue };

struct FineSyntheticBufferDeclaration {
  contracts::BufferId buffer;
  contracts::DeviceId owner;
  std::uint64_t size_bytes;
};

struct FineSyntheticTraceRecord {
  std::uint64_t sequence;
  contracts::SimTimeNs timestamp_ns;
  contracts::WorkgroupId workgroup;
  std::uint64_t wavefront;
  contracts::FineGpuOperationKind operation;
  std::uint64_t address;
  FineSyntheticLocality locality;
  contracts::DeviceId owner;
  contracts::BufferId buffer;
  std::uint64_t size_bytes;
};

struct FineSyntheticTrace {
  std::uint64_t schema_version;
  std::string kernel_name;
  std::string trace_producer;
  std::string launch_id;
  std::string hardware_id;
  FineSyntheticTimestampSemantics timestamp_semantics;
  contracts::DeviceId device;
  contracts::FineKernelKind kernel_kind;
  std::uint32_t workgroup_residency_slots;
  contracts::BufferId register_buffer;
  std::vector<FineSyntheticBufferDeclaration> buffers;
  std::vector<FineSyntheticTraceRecord> records;
  contracts::FineKernelDefinition kernel;
};

FineSyntheticTrace parse_fine_synthetic_trace(std::istream& input);
FineSyntheticTrace parse_fine_synthetic_trace_file(const std::string& path);
std::string format_fine_synthetic_trace_summary(
    const FineSyntheticTrace& trace);

}  // namespace runtime
}  // namespace simai

#endif
