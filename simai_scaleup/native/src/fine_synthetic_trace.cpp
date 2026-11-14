#include "simai/runtime/fine_synthetic_trace.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace simai {
namespace runtime {
namespace {

std::string trim(const std::string& value) {
  std::size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(begin, end - begin);
}

std::vector<std::string> split(const std::string& value, char delimiter) {
  std::vector<std::string> fields;
  std::stringstream stream(value);
  std::string field;
  while (std::getline(stream, field, delimiter)) {
    fields.push_back(trim(field));
  }
  return fields;
}

std::uint64_t number(
    const std::string& value, int base, const char* field) {
  if (value.empty() || value[0] == '-') {
    throw std::invalid_argument(std::string(field) + " must be unsigned");
  }
  std::size_t consumed = 0;
  const std::uint64_t result = std::stoull(value, &consumed, base);
  if (consumed != value.size()) {
    throw std::invalid_argument(
        std::string(field) + " contains trailing characters");
  }
  return result;
}

std::uint32_t positive_count(const std::string& value, const char* field) {
  const std::uint64_t result = number(value, 10, field);
  if (result == 0 || result > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument(std::string(field) + " is out of range");
  }
  return static_cast<std::uint32_t>(result);
}

contracts::FineKernelKind kernel_kind(const std::string& value) {
  if (value == "compute") return contracts::FineKernelKind::Compute;
  if (value == "communication") {
    return contracts::FineKernelKind::Communication;
  }
  throw std::invalid_argument(
      "synthetic trace kernel_kind must be compute or communication");
}

contracts::FineGpuOperationKind operation_kind(const std::string& value) {
  if (value == "load") return contracts::FineGpuOperationKind::Load;
  if (value == "store") return contracts::FineGpuOperationKind::Store;
  throw std::invalid_argument(
      "synthetic trace operation must be load or store");
}

FineSyntheticLocality locality_kind(const std::string& value) {
  if (value == "local") return FineSyntheticLocality::Local;
  if (value == "remote") return FineSyntheticLocality::Remote;
  throw std::invalid_argument(
      "synthetic trace locality must be local or remote");
}

FineSyntheticTimestampSemantics timestamp_semantics_kind(
    const std::string& value) {
  if (value == "earliest_issue") {
    return FineSyntheticTimestampSemantics::EarliestIssue;
  }
  throw std::invalid_argument(
      "synthetic trace timestamp_semantics currently supports earliest_issue");
}

const char* timestamp_semantics_name(
    FineSyntheticTimestampSemantics semantics) {
  if (semantics == FineSyntheticTimestampSemantics::EarliestIssue) {
    return "earliest_issue";
  }
  throw std::invalid_argument("unknown synthetic timestamp semantics");
}

const char* kernel_kind_name(contracts::FineKernelKind kind) {
  return kind == contracts::FineKernelKind::Compute
      ? "compute"
      : "communication";
}

contracts::FineMemoryReference memory_reference(
    const FineSyntheticTraceRecord& record,
    contracts::DeviceId issuer) {
  const bool local = record.locality == FineSyntheticLocality::Local;
  if (local != (record.owner == issuer)) {
    throw std::invalid_argument(
        "synthetic trace locality conflicts with owner and issuing device");
  }
  if (record.size_bytes == 0 ||
      record.address >
          std::numeric_limits<std::uint64_t>::max() - record.size_bytes) {
    throw std::invalid_argument(
        "synthetic trace address range must be nonzero and non-overflowing");
  }
  return contracts::FineMemoryReference{
      record.owner,
      local ? contracts::FineMemorySpace::LocalHbm
            : contracts::FineMemorySpace::RemoteHbm,
      record.buffer,
      record.address,
      record.size_bytes,
      1};
}

contracts::FineMemoryReference register_reference(
    contracts::DeviceId device,
    contracts::BufferId buffer,
    std::uint64_t size_bytes) {
  return contracts::FineMemoryReference{
      device,
      contracts::FineMemorySpace::RegisterFile,
      buffer,
      0,
      size_bytes,
      1};
}

contracts::FineKernelDefinition lower_kernel(
    contracts::DeviceId device,
    contracts::FineKernelKind kind,
    std::uint32_t residency,
    contracts::BufferId register_buffer,
    const std::vector<FineSyntheticTraceRecord>& records) {
  using RecordMap = std::map<
      contracts::WorkgroupId,
      std::map<std::uint64_t, std::vector<const FineSyntheticTraceRecord*> > >;
  RecordMap programs;
  for (const FineSyntheticTraceRecord& record : records) {
    programs[record.workgroup][record.wavefront].push_back(&record);
  }

  std::vector<contracts::FineWorkgroupDefinition> workgroups;
  std::uint64_t next_wavefront = 1;
  std::uint64_t next_operation = 1;
  for (auto& workgroup : programs) {
    std::vector<contracts::FineWavefrontProgram> wavefronts;
    for (auto& wavefront : workgroup.second) {
      std::vector<const FineSyntheticTraceRecord*>& ordered = wavefront.second;
      std::sort(
          ordered.begin(),
          ordered.end(),
          [](const FineSyntheticTraceRecord* lhs,
             const FineSyntheticTraceRecord* rhs) {
            if (lhs->timestamp_ns != rhs->timestamp_ns) {
              return lhs->timestamp_ns < rhs->timestamp_ns;
            }
            return lhs->sequence < rhs->sequence;
          });
      std::vector<contracts::FineGpuOperation> operations;
      for (const FineSyntheticTraceRecord* record : ordered) {
        operations.push_back(contracts::FineGpuOperation::trace_wait_until(
            contracts::GpuOperationId(next_operation++),
            record->timestamp_ns));
        const contracts::FineMemoryReference memory =
            memory_reference(*record, device);
        const contracts::FineMemoryReference registers = register_reference(
            device, register_buffer, record->size_bytes);
        operations.push_back(
            record->operation == contracts::FineGpuOperationKind::Load
            ? contracts::FineGpuOperation::load(
                  contracts::GpuOperationId(next_operation++),
                  memory,
                  registers)
            : contracts::FineGpuOperation::store(
                  contracts::GpuOperationId(next_operation++),
                  registers,
                  memory));
      }
      wavefronts.push_back(contracts::FineWavefrontProgram{
          contracts::WavefrontId(next_wavefront++), operations});
    }
    workgroups.push_back(contracts::FineWorkgroupDefinition{
        workgroup.first,
        contracts::ThreadBlockId(workgroup.first.value()),
        contracts::FineWorkgroupResources{residency},
        wavefronts});
  }
  contracts::FineKernelDefinition kernel{
      contracts::KernelId(1), device, kind, workgroups};
  kernel.require_valid();
  return kernel;
}

}  // namespace

FineSyntheticTrace parse_fine_synthetic_trace(std::istream& input) {
  std::uint64_t schema_version = 0;
  std::string name;
  std::string trace_producer;
  std::string launch_id;
  std::string hardware_id;
  FineSyntheticTimestampSemantics timestamp_semantics =
      FineSyntheticTimestampSemantics::EarliestIssue;
  bool has_timestamp_semantics = false;
  contracts::DeviceId device;
  contracts::FineKernelKind kind = contracts::FineKernelKind::Compute;
  bool has_kind = false;
  std::uint32_t residency = 0;
  contracts::BufferId register_buffer;
  bool timestamp_unit_ns = false;
  std::vector<FineSyntheticBufferDeclaration> buffers;
  std::set<contracts::BufferId> buffer_ids;
  std::vector<FineSyntheticTraceRecord> records;
  std::set<std::string> singleton_fields;
  std::string line;
  std::uint64_t sequence = 0;
  while (std::getline(input, line)) {
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) line.erase(comment);
    line = trim(line);
    if (line.empty()) continue;
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) {
      throw std::invalid_argument(
          "synthetic trace lines must use key=value syntax");
    }
    const std::string key = trim(line.substr(0, equals));
    const std::string value = trim(line.substr(equals + 1));
    if (key == "buffer") {
      const std::vector<std::string> fields = split(value, '|');
      if (fields.size() != 3) {
        throw std::invalid_argument(
            "synthetic trace buffer requires buffer, owner, and size");
      }
      const contracts::BufferId buffer(number(fields[0], 10, "buffer"));
      const contracts::DeviceId owner(number(fields[1], 10, "buffer owner"));
      const std::uint64_t size = number(fields[2], 10, "buffer size");
      if (!buffer.valid() || !owner.valid() || size == 0 ||
          !buffer_ids.insert(buffer).second) {
        throw std::invalid_argument(
            "synthetic trace buffer declarations require unique nonzero identities and size");
      }
      buffers.push_back(FineSyntheticBufferDeclaration{buffer, owner, size});
      continue;
    }
    if (key == "record") {
      const std::vector<std::string> fields = split(value, '|');
      if (fields.size() != 9) {
        throw std::invalid_argument(
            "synthetic trace record requires nine fields");
      }
      const std::uint64_t workgroup = number(fields[1], 10, "workgroup");
      const std::uint64_t wavefront = number(fields[2], 10, "wavefront");
      const std::uint64_t owner = number(fields[6], 10, "owner device");
      const std::uint64_t buffer = number(fields[7], 10, "buffer");
      if (workgroup == 0 || wavefront == 0 || owner == 0 || buffer == 0) {
        throw std::invalid_argument(
            "synthetic trace identities must be nonzero");
      }
      records.push_back(FineSyntheticTraceRecord{
          sequence++,
          number(fields[0], 10, "timestamp_ns"),
          contracts::WorkgroupId(workgroup),
          wavefront,
          operation_kind(fields[3]),
          number(fields[4], 0, "address"),
          locality_kind(fields[5]),
          contracts::DeviceId(owner),
          contracts::BufferId(buffer),
          number(fields[8], 10, "size_bytes")});
      continue;
    }
    if (!singleton_fields.insert(key).second) {
      throw std::invalid_argument(
          "synthetic trace contains a duplicate metadata field");
    }
    if (key == "schema_version") {
      schema_version = number(value, 10, "schema version");
    } else if (key == "kernel_name") {
      name = value;
    } else if (key == "trace_producer") {
      trace_producer = value;
    } else if (key == "launch_id") {
      launch_id = value;
    } else if (key == "hardware_id") {
      hardware_id = value;
    } else if (key == "timestamp_semantics") {
      timestamp_semantics = timestamp_semantics_kind(value);
      has_timestamp_semantics = true;
    } else if (key == "device") {
      device = contracts::DeviceId(number(value, 10, "device"));
    } else if (key == "kernel_kind") {
      kind = kernel_kind(value);
      has_kind = true;
    } else if (key == "timestamp_unit") {
      if (value != "ns") {
        throw std::invalid_argument(
            "synthetic trace timestamp_unit must be ns");
      }
      timestamp_unit_ns = true;
    } else if (key == "workgroup_residency_slots") {
      residency = positive_count(value, "workgroup residency");
    } else if (key == "register_buffer") {
      register_buffer =
          contracts::BufferId(number(value, 10, "register buffer"));
    } else {
      throw std::invalid_argument("unsupported synthetic trace field: " + key);
    }
  }
  const bool version_one = schema_version == 1;
  const bool version_two = schema_version == 2;
  const bool has_v2_metadata = !trace_producer.empty() || !launch_id.empty() ||
      !hardware_id.empty() || has_timestamp_semantics || !buffers.empty();
  if ((!version_one && !version_two) || name.empty() || !device.valid() ||
      !has_kind ||
      !timestamp_unit_ns || residency == 0 || !register_buffer.valid() ||
      records.empty()) {
    throw std::invalid_argument(
        "synthetic trace is missing required schema or kernel metadata");
  }
  if (version_one && has_v2_metadata) {
    throw std::invalid_argument(
        "synthetic trace schema version 1 may not contain version 2 metadata");
  }
  if (version_two &&
      (trace_producer.empty() || launch_id.empty() || hardware_id.empty() ||
       !has_timestamp_semantics || buffers.empty())) {
    throw std::invalid_argument(
        "synthetic trace schema version 2 requires producer, launch, "
        "hardware, timestamp semantics, and buffer declarations");
  }
  if (version_two) {
    std::map<contracts::BufferId, FineSyntheticBufferDeclaration>
        declared_buffers;
    for (const FineSyntheticBufferDeclaration& buffer : buffers) {
      declared_buffers.emplace(buffer.buffer, buffer);
    }
    for (const FineSyntheticTraceRecord& record : records) {
      const auto found = declared_buffers.find(record.buffer);
      if (found == declared_buffers.end()) {
        throw std::invalid_argument(
            "synthetic trace record references an undeclared buffer");
      }
      const FineSyntheticBufferDeclaration& buffer = found->second;
      if (buffer.owner != record.owner ||
          record.address > buffer.size_bytes ||
          record.size_bytes > buffer.size_bytes - record.address) {
        throw std::invalid_argument(
            "synthetic trace record conflicts with its buffer owner or bounds");
      }
    }
  }
  const contracts::FineKernelDefinition kernel = lower_kernel(
      device, kind, residency, register_buffer, records);
  return FineSyntheticTrace{
      schema_version,
      name,
      trace_producer,
      launch_id,
      hardware_id,
      timestamp_semantics,
      device,
      kind,
      residency,
      register_buffer,
      buffers,
      records,
      kernel};
}

FineSyntheticTrace parse_fine_synthetic_trace_file(const std::string& path) {
  std::ifstream input(path.c_str());
  if (!input) {
    throw std::invalid_argument("could not open synthetic trace: " + path);
  }
  return parse_fine_synthetic_trace(input);
}

std::string format_fine_synthetic_trace_summary(
    const FineSyntheticTrace& trace) {
  if (trace.records.empty()) {
    throw std::invalid_argument(
        "synthetic trace summary requires at least one record");
  }
  std::uint64_t loads = 0;
  std::uint64_t stores = 0;
  std::uint64_t local_bytes = 0;
  std::uint64_t remote_bytes = 0;
  std::uint64_t declared_buffer_bytes = 0;
  contracts::SimTimeNs first_timestamp = trace.records.front().timestamp_ns;
  contracts::SimTimeNs last_timestamp = first_timestamp;
  std::set<contracts::WorkgroupId> workgroups;
  std::set<std::pair<contracts::WorkgroupId, std::uint64_t> > wavefronts;
  for (const FineSyntheticBufferDeclaration& buffer : trace.buffers) {
    declared_buffer_bytes += buffer.size_bytes;
  }
  for (const FineSyntheticTraceRecord& record : trace.records) {
    if (record.operation == contracts::FineGpuOperationKind::Load) {
      ++loads;
    } else {
      ++stores;
    }
    if (record.locality == FineSyntheticLocality::Local) {
      local_bytes += record.size_bytes;
    } else {
      remote_bytes += record.size_bytes;
    }
    first_timestamp = std::min(first_timestamp, record.timestamp_ns);
    last_timestamp = std::max(last_timestamp, record.timestamp_ns);
    workgroups.insert(record.workgroup);
    wavefronts.insert(std::make_pair(record.workgroup, record.wavefront));
  }
  std::ostringstream output;
  output << "synthetic_trace_schema_version=" << trace.schema_version << '\n'
         << "kernel_name=" << trace.kernel_name << '\n'
         << "trace_producer="
         << (trace.trace_producer.empty() ? "-" : trace.trace_producer) << '\n'
         << "launch_id=" << (trace.launch_id.empty() ? "-" : trace.launch_id)
         << '\n'
         << "hardware_id="
         << (trace.hardware_id.empty() ? "-" : trace.hardware_id) << '\n'
         << "timestamp_semantics="
         << timestamp_semantics_name(trace.timestamp_semantics) << '\n'
         << "device=" << trace.device.value() << '\n'
         << "kernel_kind=" << kernel_kind_name(trace.kernel_kind) << '\n'
         << "records=" << trace.records.size() << '\n'
         << "workgroups=" << workgroups.size() << '\n'
         << "wavefronts=" << wavefronts.size() << '\n'
         << "declared_buffers=" << trace.buffers.size() << '\n'
         << "declared_buffer_bytes=" << declared_buffer_bytes << '\n'
         << "loads=" << loads << '\n'
         << "stores=" << stores << '\n'
         << "local_bytes=" << local_bytes << '\n'
         << "remote_bytes=" << remote_bytes << '\n'
         << "first_timestamp_ns=" << first_timestamp << '\n'
         << "last_timestamp_ns=" << last_timestamp << '\n';
  return output.str();
}

}  // namespace runtime
}  // namespace simai
