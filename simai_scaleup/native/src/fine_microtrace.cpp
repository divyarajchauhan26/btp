#include "simai/runtime/fine_microtrace.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace simai {
namespace runtime {
namespace {

struct BlockKey {
  std::uint32_t x;
  std::uint32_t y;
  std::uint32_t z;

  bool operator<(const BlockKey& other) const {
    return std::tie(z, y, x) < std::tie(other.z, other.y, other.x);
  }
};

struct InstructionKey {
  BlockKey block;
  std::uint64_t warp;
  std::uint64_t clock;
  std::uint64_t probe;
  std::uint64_t encoded_type;

  bool operator<(const InstructionKey& other) const {
    return std::tie(
               block, warp, clock, probe, encoded_type) <
        std::tie(
               other.block,
               other.warp,
               other.clock,
               other.probe,
               other.encoded_type);
  }
};

struct AddressInterval {
  std::uint64_t begin;
  std::uint64_t end;
};

struct CoalescedInterval {
  AddressInterval address;
  std::uint32_t active_lanes;
};

std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> fields;
  std::stringstream stream(line);
  std::string field;
  while (std::getline(stream, field, '\t')) fields.push_back(field);
  return fields;
}

std::uint64_t unsigned_number(
    const std::string& value, int base, const char* field) {
  if (value.empty() || value[0] == '-') {
    throw std::invalid_argument(std::string(field) + " must be unsigned");
  }
  std::size_t consumed = 0;
  const std::uint64_t parsed = std::stoull(value, &consumed, base);
  if (consumed != value.size()) {
    throw std::invalid_argument(
        std::string(field) + " contains trailing characters");
  }
  return parsed;
}

std::uint32_t coordinate(const std::string& value, const char* field) {
  const std::uint64_t parsed = unsigned_number(value, 10, field);
  if (parsed > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument(std::string(field) + " is out of range");
  }
  return static_cast<std::uint32_t>(parsed);
}

bool power_of_two(std::uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

std::uint64_t checked_end(
    std::uint64_t begin, std::uint64_t length, const char* field) {
  if (length == 0 || begin > std::numeric_limits<std::uint64_t>::max() - length) {
    throw std::invalid_argument(std::string(field) + " range overflows");
  }
  return begin + length;
}

void validate_config(const FineMicrotraceImportConfig& config) {
  if (!config.kernel.valid() || !config.device.valid() ||
      !config.register_buffer.valid()) {
    throw std::invalid_argument(
        "microtrace kernel, device, and register buffer must be valid");
  }
  if (config.block_dimension_x == 0 || config.block_dimension_y == 0 ||
      config.block_dimension_z == 0 || config.warp_size == 0 ||
      config.workgroup_residency_slots == 0 || config.access_bytes == 0) {
    throw std::invalid_argument(
        "microtrace dimensions, residency, and access width must be nonzero");
  }
  if (config.instruction_types.empty() || config.allocations.empty()) {
    throw std::invalid_argument(
        "microtrace requires instruction mappings and allocations");
  }
  if (config.timestamp_semantics !=
      FineMicrotraceTimestampSemantics::KernelElapsedCycles) {
    throw std::invalid_argument(
        "microtrace lowering currently requires kernel-elapsed cycle timestamps");
  }
  std::set<std::uint64_t> encoded_types;
  for (const FineMicrotraceInstructionTypeMapping& mapping :
       config.instruction_types) {
    if (!encoded_types.insert(mapping.encoded_type).second) {
      throw std::invalid_argument(
          "microtrace instruction type mappings must be unique");
    }
  }
  std::vector<AddressInterval> allocations;
  for (const FineMicrotraceAllocation& allocation : config.allocations) {
    if (!allocation.owner.valid() || !allocation.buffer.valid() ||
        !power_of_two(allocation.alignment_bytes)) {
      throw std::invalid_argument(
          "microtrace allocation owner, buffer, and alignment must be valid");
    }
    allocations.push_back(AddressInterval{
        allocation.base_address,
        checked_end(
            allocation.base_address,
            allocation.size_bytes,
            "microtrace allocation")});
  }
  std::sort(
      allocations.begin(),
      allocations.end(),
      [](const AddressInterval& lhs, const AddressInterval& rhs) {
        return lhs.begin < rhs.begin;
      });
  for (std::size_t index = 1; index < allocations.size(); ++index) {
    if (allocations[index].begin < allocations[index - 1].end) {
      throw std::invalid_argument("microtrace allocations must not overlap");
    }
  }
}

FineMicrotraceInstructionKind instruction_kind(
    std::uint64_t encoded,
    const FineMicrotraceImportConfig& config) {
  for (const FineMicrotraceInstructionTypeMapping& mapping :
       config.instruction_types) {
    if (mapping.encoded_type == encoded) return mapping.kind;
  }
  throw std::invalid_argument("microtrace contains an unmapped instruction type");
}

std::uint64_t linear_thread(
    const FineMicrotraceRecord& record,
    const FineMicrotraceImportConfig& config) {
  if (record.thread_x >= config.block_dimension_x ||
      record.thread_y >= config.block_dimension_y ||
      record.thread_z >= config.block_dimension_z) {
    throw std::invalid_argument(
        "microtrace thread coordinate exceeds configured block dimensions");
  }
  const std::uint64_t plane =
      static_cast<std::uint64_t>(config.block_dimension_x) *
      config.block_dimension_y;
  return record.thread_x +
      static_cast<std::uint64_t>(config.block_dimension_x) * record.thread_y +
      plane * record.thread_z;
}

const FineMicrotraceAllocation& allocation_for(
    const AddressInterval& interval,
    const FineMicrotraceImportConfig& config) {
  for (const FineMicrotraceAllocation& allocation : config.allocations) {
    const std::uint64_t allocation_end = checked_end(
        allocation.base_address,
        allocation.size_bytes,
        "microtrace allocation");
    if (interval.begin >= allocation.base_address &&
        interval.end <= allocation_end) {
      return allocation;
    }
  }
  throw std::invalid_argument(
      "microtrace memory access is unmapped or crosses an allocation");
}

std::vector<CoalescedInterval> coalesced_intervals(
    const std::vector<const FineMicrotraceRecord*>& records,
    std::uint64_t access_bytes) {
  std::vector<AddressInterval> intervals;
  for (const FineMicrotraceRecord* record : records) {
    intervals.push_back(AddressInterval{
        record->address,
        checked_end(record->address, access_bytes, "microtrace access")});
  }
  std::sort(
      intervals.begin(),
      intervals.end(),
      [](const AddressInterval& lhs, const AddressInterval& rhs) {
        return std::tie(lhs.begin, lhs.end) < std::tie(rhs.begin, rhs.end);
      });
  std::vector<CoalescedInterval> result;
  for (const AddressInterval& interval : intervals) {
    if (result.empty() || interval.begin > result.back().address.end) {
      result.push_back(CoalescedInterval{interval, 1});
    } else {
      result.back().address.end =
          std::max(result.back().address.end, interval.end);
      ++result.back().active_lanes;
    }
  }
  return result;
}

contracts::FineMemoryReference memory_reference(
    const AddressInterval& interval,
    const FineMicrotraceImportConfig& config) {
  const FineMicrotraceAllocation& allocation = allocation_for(interval, config);
  const std::uint64_t offset = interval.begin - allocation.base_address;
  if (offset % allocation.alignment_bytes != 0) {
    throw std::invalid_argument(
        "microtrace access violates allocation reference alignment");
  }
  return contracts::FineMemoryReference{
      allocation.owner,
      allocation.owner == config.device
          ? contracts::FineMemorySpace::LocalHbm
          : contracts::FineMemorySpace::RemoteHbm,
      allocation.buffer,
      offset,
      interval.end - interval.begin,
      allocation.alignment_bytes};
}

contracts::FineMemoryReference register_reference(
    const FineMicrotraceImportConfig& config,
    std::uint64_t length) {
  return contracts::FineMemoryReference{
      config.device,
      contracts::FineMemorySpace::RegisterFile,
      config.register_buffer,
      0,
      length,
      1};
}

}  // namespace

std::vector<FineMicrotraceRecord> parse_fine_microtrace_tsv(
    std::istream& input) {
  std::string header;
  if (!std::getline(input, header)) {
    throw std::invalid_argument("microtrace TSV is empty");
  }
  if (!header.empty() && header.back() == '\r') header.pop_back();
  const std::vector<std::string> columns = split_tabs(header);
  const std::vector<std::string> expected{
      "probe",
      "block_x",
      "block_y",
      "block_z",
      "thread_x",
      "thread_y",
      "thread_z",
      "clock_delta",
      "address",
      "instr_type"};
  if (columns != expected) {
    throw std::invalid_argument(
        "microtrace TSV header does not match the supported schema");
  }

  std::vector<FineMicrotraceRecord> records;
  std::string line;
  std::uint64_t line_number = 1;
  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const std::vector<std::string> fields = split_tabs(line);
    if (fields.size() != expected.size()) {
      throw std::invalid_argument(
          "microtrace TSV row has the wrong field count at line " +
          std::to_string(line_number));
    }
    records.push_back(FineMicrotraceRecord{
        unsigned_number(fields[0], 10, "probe"),
        coordinate(fields[1], "block_x"),
        coordinate(fields[2], "block_y"),
        coordinate(fields[3], "block_z"),
        coordinate(fields[4], "thread_x"),
        coordinate(fields[5], "thread_y"),
        coordinate(fields[6], "thread_z"),
        unsigned_number(fields[7], 10, "clock_delta"),
        unsigned_number(fields[8], 0, "address"),
        unsigned_number(fields[9], 10, "instr_type")});
  }
  if (records.empty()) {
    throw std::invalid_argument("microtrace TSV contains no records");
  }
  return records;
}

std::vector<FineMicrotraceRecord> parse_fine_microtrace_tsv_file(
    const std::string& path) {
  std::ifstream input(path.c_str());
  if (!input) {
    throw std::invalid_argument("could not open microtrace TSV: " + path);
  }
  return parse_fine_microtrace_tsv(input);
}

FineMicrotraceImportResult import_fine_microtrace(
    const std::vector<FineMicrotraceRecord>& records,
    const FineMicrotraceImportConfig& config) {
  validate_config(config);
  if (records.empty()) {
    throw std::invalid_argument("microtrace import requires records");
  }

  std::map<InstructionKey, std::vector<const FineMicrotraceRecord*> > groups;
  for (const FineMicrotraceRecord& record : records) {
    const std::uint64_t linear = linear_thread(record, config);
    const InstructionKey key{
        BlockKey{record.block_x, record.block_y, record.block_z},
        linear / config.warp_size,
        record.clock,
        record.probe,
        record.encoded_instruction_type};
    groups[key].push_back(&record);
  }

  std::vector<FineMicrotraceAccess> accesses;
  for (const auto& group : groups) {
    std::set<std::uint64_t> lanes;
    for (const FineMicrotraceRecord* record : group.second) {
      const std::uint64_t lane = linear_thread(*record, config) % config.warp_size;
      if (!lanes.insert(lane).second) {
        throw std::invalid_argument(
            "microtrace dynamic instruction contains a duplicate lane");
      }
    }
    for (const CoalescedInterval& interval :
         coalesced_intervals(group.second, config.access_bytes)) {
      accesses.push_back(FineMicrotraceAccess{
          group.first.probe,
          group.first.block.x,
          group.first.block.y,
          group.first.block.z,
          group.first.warp,
          group.first.clock,
          instruction_kind(group.first.encoded_type, config),
          interval.active_lanes,
          memory_reference(interval.address, config)});
    }
  }

  std::map<BlockKey, std::map<std::uint64_t, std::vector<FineMicrotraceAccess> > >
      programs;
  for (const FineMicrotraceAccess& access : accesses) {
    programs[BlockKey{access.block_x, access.block_y, access.block_z}]
        [access.warp]
        .push_back(access);
  }

  std::vector<contracts::FineWorkgroupDefinition> workgroups;
  std::uint64_t next_workgroup = 1;
  std::uint64_t next_wavefront = 1;
  std::uint64_t next_operation = 1;
  for (const auto& block : programs) {
    std::vector<contracts::FineWavefrontProgram> wavefronts;
    for (const auto& warp : block.second) {
      std::vector<contracts::FineGpuOperation> operations;
      for (const FineMicrotraceAccess& access : warp.second) {
        const contracts::FineMemoryReference registers =
            register_reference(config, access.memory.length_bytes);
        if (access.kind == FineMicrotraceInstructionKind::Load) {
          operations.push_back(contracts::FineGpuOperation::load(
              contracts::GpuOperationId(next_operation++),
              access.memory,
              registers));
        } else {
          operations.push_back(contracts::FineGpuOperation::store(
              contracts::GpuOperationId(next_operation++),
              registers,
              access.memory));
        }
      }
      wavefronts.push_back(contracts::FineWavefrontProgram{
          contracts::WavefrontId(next_wavefront++), operations});
    }
    workgroups.push_back(contracts::FineWorkgroupDefinition{
        contracts::WorkgroupId(next_workgroup),
        contracts::ThreadBlockId(next_workgroup),
        contracts::FineWorkgroupResources{
            config.workgroup_residency_slots},
        wavefronts});
    ++next_workgroup;
  }

  contracts::FineKernelDefinition kernel{
      config.kernel, config.device, config.kernel_kind, workgroups};
  kernel.require_valid();
  return FineMicrotraceImportResult{
      records, accesses, config.timestamp_semantics, kernel};
}

FineMicrotraceImportResult import_fine_microtrace_tsv(
    std::istream& input,
    const FineMicrotraceImportConfig& config) {
  return import_fine_microtrace(parse_fine_microtrace_tsv(input), config);
}

FineMicrotraceImportResult import_fine_microtrace_tsv_file(
    const std::string& path,
    const FineMicrotraceImportConfig& config) {
  return import_fine_microtrace(parse_fine_microtrace_tsv_file(path), config);
}

}  // namespace runtime
}  // namespace simai
