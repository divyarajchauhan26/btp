#include "simai/runtime/aicb_importer.hpp"

#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace simai {
namespace runtime {
namespace {

std::vector<std::string> tokens(const std::string& line) {
  std::istringstream stream(line);
  std::vector<std::string> result;
  std::string token;
  while (stream >> token) {
    result.push_back(token);
  }
  return result;
}

std::uint64_t parse_unsigned(
    const std::string& text,
    const std::string& field) {
  if (text.empty() || text[0] == '-') {
    throw std::invalid_argument("AICB " + field + " must be unsigned");
  }
  std::size_t consumed = 0;
  std::uint64_t result = 0;
  try {
    result = std::stoull(text, &consumed, 10);
  } catch (const std::exception&) {
    throw std::invalid_argument("AICB " + field + " is not an integer");
  }
  if (consumed != text.size()) {
    throw std::invalid_argument("AICB " + field + " is not an integer");
  }
  return result;
}

std::uint64_t header_value(
    const std::vector<std::string>& header,
    const std::string& key) {
  for (std::size_t index = 0; index + 1 < header.size(); ++index) {
    if (header[index] == key) {
      return parse_unsigned(header[index + 1], key);
    }
  }
  throw std::invalid_argument("AICB header is missing " + key);
}

contracts::SimTimeNs checked_scale(
    std::uint64_t ticks,
    std::uint64_t nanoseconds_per_tick) {
  if (nanoseconds_per_tick != 0 &&
      ticks > std::numeric_limits<std::uint64_t>::max() /
          nanoseconds_per_tick) {
    throw std::overflow_error("AICB compute time scaling overflow");
  }
  return ticks * nanoseconds_per_tick;
}

LayerCollectiveKind decode_collective(const std::string& value) {
  if (value == "NONE") {
    return LayerCollectiveKind::None;
  }
  if (value == "ALLREDUCE") {
    return LayerCollectiveKind::AllReduce;
  }
  if (value == "REDUCESCATTER") {
    return LayerCollectiveKind::ReduceScatter;
  }
  if (value == "ALLGATHER") {
    return LayerCollectiveKind::AllGather;
  }
  if (value == "ALLTOALL") {
    return LayerCollectiveKind::AllToAll;
  }
  throw std::invalid_argument(
      "AICB collective is unsupported in one scale-up group: " + value);
}

void append_phase(
    WorkloadSpec& workload,
    const std::string& name,
    std::uint64_t compute_ticks,
    const std::string& collective_name,
    std::uint64_t payload_bytes,
    std::uint64_t nanoseconds_per_tick) {
  const LayerCollectiveKind collective =
      decode_collective(collective_name);
  if ((collective == LayerCollectiveKind::None) != (payload_bytes == 0)) {
    throw std::invalid_argument(
        "AICB NONE/collective payload fields are inconsistent");
  }
  if (compute_ticks == 0 && collective == LayerCollectiveKind::None) {
    return;
  }
  if (workload.layers.size() >=
      std::numeric_limits<std::uint32_t>::max()) {
    throw std::overflow_error("AICB expansion exceeds layer index range");
  }
  const std::uint32_t index =
      static_cast<std::uint32_t>(workload.layers.size());
  workload.layers.push_back(LayerSpec{
      index,
      checked_scale(compute_ticks, nanoseconds_per_tick),
      collective,
      payload_bytes,
      LayerStartPolicy::AfterPreviousFinish,
      name});
}

}  // namespace

WorkloadSpec import_aicb_workload(
    std::istream& input,
    const AicbImportOptions& options) {
  if (options.nanoseconds_per_tick == 0) {
    throw std::invalid_argument("AICB nanoseconds_per_tick must be nonzero");
  }
  std::string header_line;
  if (!std::getline(input, header_line)) {
    throw std::invalid_argument("AICB workload is missing its header");
  }
  const std::vector<std::string> header = tokens(header_line);
  if (header.empty() ||
      (header[0] != "HYBRID_TRANSFORMER" &&
       header[0] != "HYBRID_TRANSFORMER_FWD_IN_BCKWD")) {
    throw std::invalid_argument(
        "AICB adapter supports transformer workloads only");
  }
  const std::uint64_t tensor_parallel =
      header_value(header, "model_parallel_NPU_group:");
  const std::uint64_t expert_parallel = header_value(header, "ep:");
  const std::uint64_t pipeline_parallel = header_value(header, "pp:");
  const std::uint64_t all_gpus = header_value(header, "all_gpus:");
  if (all_gpus < 2 || all_gpus > 1024 ||
      tensor_parallel != all_gpus || expert_parallel != 1 ||
      pipeline_parallel != 1) {
    throw std::invalid_argument(
        "AICB adapter requires one 2..1024-endpoint TP-only scale-up domain");
  }

  std::string count_line;
  if (!std::getline(input, count_line)) {
    throw std::invalid_argument("AICB workload is missing its row count");
  }
  const std::vector<std::string> count_fields = tokens(count_line);
  if (count_fields.size() != 1) {
    throw std::invalid_argument("AICB row count line must contain one integer");
  }
  const std::uint64_t row_count =
      parse_unsigned(count_fields[0], "row count");
  WorkloadSpec workload{
      options.schema_version,
      options.deterministic_seed,
      static_cast<std::uint32_t>(all_gpus),
      options.collective_algorithm,
      contracts::ParallelGroup{contracts::ParallelGroupId(1), {}},
      options.link,
      options.gpu,
      {}};
  for (std::uint32_t endpoint = 1; endpoint <= workload.endpoint_count;
       ++endpoint) {
    workload.tensor_parallel_group.members.push_back(
        contracts::DeviceId(endpoint));
  }

  for (std::uint64_t row = 0; row < row_count; ++row) {
    std::string line;
    if (!std::getline(input, line)) {
      throw std::invalid_argument("AICB row count exceeds available rows");
    }
    const std::vector<std::string> fields = tokens(line);
    if (fields.size() != 12 || fields[1] != "-1") {
      throw std::invalid_argument(
          "AICB row must have 12 fields and dependency -1");
    }
    const std::uint64_t forward_compute =
        parse_unsigned(fields[2], "forward compute");
    const std::uint64_t forward_bytes =
        parse_unsigned(fields[4], "forward bytes");
    const std::uint64_t input_compute =
        parse_unsigned(fields[5], "input-gradient compute");
    const std::uint64_t input_bytes =
        parse_unsigned(fields[7], "input-gradient bytes");
    const std::uint64_t weight_compute =
        parse_unsigned(fields[8], "weight-gradient compute");
    const std::uint64_t weight_bytes =
        parse_unsigned(fields[10], "weight-gradient bytes");
    const std::uint64_t update_compute =
        parse_unsigned(fields[11], "weight update compute");
    if (weight_compute >
        std::numeric_limits<std::uint64_t>::max() - update_compute) {
      throw std::overflow_error("AICB weight compute time overflow");
    }
    append_phase(
        workload,
        fields[0] + ".forward",
        forward_compute,
        fields[3],
        forward_bytes,
        options.nanoseconds_per_tick);
    append_phase(
        workload,
        fields[0] + ".input_gradient",
        input_compute,
        fields[6],
        input_bytes,
        options.nanoseconds_per_tick);
    append_phase(
        workload,
        fields[0] + ".weight_gradient",
        weight_compute + update_compute,
        fields[9],
        weight_bytes,
        options.nanoseconds_per_tick);
  }
  std::string trailing;
  while (std::getline(input, trailing)) {
    if (!tokens(trailing).empty()) {
      throw std::invalid_argument("AICB workload has rows beyond row count");
    }
  }
  workload.require_valid();
  return workload;
}

WorkloadSpec import_aicb_workload_file(
    const std::string& path,
    const AicbImportOptions& options) {
  std::ifstream input(path.c_str());
  if (!input) {
    throw std::runtime_error("could not open AICB workload file: " + path);
  }
  return import_aicb_workload(input, options);
}

}  // namespace runtime
}  // namespace simai
