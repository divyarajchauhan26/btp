#include "simai/runtime/workload.hpp"

#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace simai {
namespace runtime {
namespace {

std::string trim(const std::string& value) {
  std::size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return value.substr(begin, end - begin);
}

std::vector<std::string> split(
    const std::string& value,
    char delimiter) {
  std::vector<std::string> pieces;
  std::stringstream stream(value);
  std::string piece;
  while (std::getline(stream, piece, delimiter)) {
    pieces.push_back(trim(piece));
  }
  return pieces;
}

std::uint64_t parse_unsigned(
    const std::string& value,
    const std::string& field,
    std::size_t line_number) {
  if (value.empty() || value[0] == '-') {
    throw std::invalid_argument(
        "line " + std::to_string(line_number) + ": " + field +
        " must be an unsigned integer");
  }
  std::size_t consumed = 0;
  std::uint64_t result = 0;
  try {
    result = std::stoull(value, &consumed, 10);
  } catch (const std::exception&) {
    throw std::invalid_argument(
        "line " + std::to_string(line_number) + ": invalid " + field);
  }
  if (consumed != value.size()) {
    throw std::invalid_argument(
        "line " + std::to_string(line_number) + ": invalid " + field);
  }
  return result;
}

void append_all(
    contracts::ValidationErrors& destination,
    const contracts::ValidationErrors& source) {
  destination.insert(destination.end(), source.begin(), source.end());
}

}  // namespace

contracts::ValidationErrors WorkloadSpec::validate() const {
  contracts::ValidationErrors errors;
  if (schema_version != 1) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "workload schema_version must be 1");
  }
  if (endpoint_count < 2 || endpoint_count > 1024) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "scale-up workload endpoint count must be in the range 2..1024");
  }
  if (collective_algorithm != CollectiveAlgorithmPolicy::Auto &&
      collective_algorithm !=
          CollectiveAlgorithmPolicy::GatherReduceBroadcast &&
      collective_algorithm != CollectiveAlgorithmPolicy::Ring &&
      collective_algorithm != CollectiveAlgorithmPolicy::Direct) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "collective algorithm policy is invalid");
  }
  append_all(errors, tensor_parallel_group.validate());
  if (tensor_parallel_group.members.size() != endpoint_count) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "tensor-parallel group must contain every endpoint exactly once");
  }
  if (link.bandwidth_gbps == 0) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "link bandwidth must be nonzero");
  }
  if (link.transaction_payload_bytes == 0 ||
      link.transaction_payload_bytes > 256 ||
      link.pdu_payload_bytes < link.transaction_payload_bytes ||
      link.pdu_payload_bytes > 4096) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "transaction payload must be 1..256 bytes and PDU payload must be transaction..4096 bytes");
  }
  if (link.rail_count == 0 || link.rail_count > 4) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "rail_count must be in the inclusive range 1..4");
  }
  if (link.switch_enabled > 1 ||
      (link.switch_enabled == 1 &&
       link.switch_queue_capacity_pdus == 0)) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "switch_enabled must be 0 or 1 and enabled switch capacity must be nonzero");
  }
  if (link.switch_enabled == 1 &&
      link.switch_credits_per_egress >
          link.switch_queue_capacity_pdus) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "switch credits per egress may not exceed queue capacity");
  }
  if ((link.error_injection_pdu_id == 0) !=
      (link.error_injection_attempts == 0)) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "error injection PDU and attempt count must both be zero or nonzero");
  }
  if (gpu.enabled > 1) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "gpu_enabled must be 0 or 1");
  }
  const bool gpu_parameters_present =
      gpu.compute_units_per_device != 0 ||
      gpu.workgroup_slots_per_compute_unit != 0 ||
      gpu.wavefronts_per_workgroup != 0 || gpu.wavefront_issue_width != 0 ||
      gpu.wavefront_issue_latency_ns != 0 || gpu.reduction_bytes_per_ns != 0;
  if (gpu.enabled == 0 && gpu_parameters_present) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "disabled GPU execution model must have zero-valued parameters");
  }
  if (gpu.enabled == 1 &&
      (gpu.compute_units_per_device == 0 ||
       gpu.workgroup_slots_per_compute_unit == 0 ||
       gpu.wavefronts_per_workgroup == 0 || gpu.wavefront_issue_width == 0 ||
       gpu.wavefront_issue_latency_ns == 0 ||
       gpu.reduction_bytes_per_ns == 0)) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "enabled GPU execution model requires all resource parameters");
  }
  if (layers.empty() || layers.size() > 1000000) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "workload must contain between one and one million layers");
  }

  for (std::size_t index = 0; index < layers.size(); ++index) {
    const LayerSpec& layer = layers[index];
    if (layer.index != index || layer.name.empty()) {
      contracts::append_error(
          errors,
          contracts::ValidationCode::InvalidValue,
          "layers must be zero-based, ordered, and named");
    }
    for (const char character : layer.name) {
      if (std::isalnum(static_cast<unsigned char>(character)) == 0 &&
          character != '_' && character != '-' && character != '.') {
        contracts::append_error(
            errors,
            contracts::ValidationCode::InvalidValue,
            "layer names may contain letters, digits, underscore, dash, and dot only");
        break;
      }
    }
    if (layer.start_policy != LayerStartPolicy::AfterPreviousFinish &&
        layer.start_policy != LayerStartPolicy::AfterPreviousCompute) {
      contracts::append_error(
          errors,
          contracts::ValidationCode::InvalidValue,
          "layer start policy is invalid");
    }
    if (index == 0 &&
        layer.start_policy != LayerStartPolicy::AfterPreviousFinish) {
      contracts::append_error(
          errors,
          contracts::ValidationCode::InvalidValue,
          "first layer must use after_previous_finish start policy");
    }
    if (layer.collective != LayerCollectiveKind::None) {
      if (layer.payload_bytes == 0) {
        contracts::append_error(
            errors,
            contracts::ValidationCode::InvalidValue,
            "collective layer requires nonzero payload bytes");
      }
    } else if (layer.payload_bytes != 0) {
      contracts::append_error(
          errors,
          contracts::ValidationCode::InvalidValue,
          "layer without a collective must have zero payload bytes");
    }
  }
  return errors;
}

void WorkloadSpec::require_valid() const {
  contracts::require_no_errors(validate(), "WorkloadSpec");
}

WorkloadSpec parse_workload(std::istream& input) {
  WorkloadSpec result{
      0,
      0,
      0,
      CollectiveAlgorithmPolicy::Auto,
      contracts::ParallelGroup{contracts::ParallelGroupId(1), {}},
      LinkConfig{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
      GpuConfig{0, 0, 0, 0, 0, 0, 0},
      {}};
  std::unordered_set<std::string> seen_scalars;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) {
      line.erase(comment);
    }
    line = trim(line);
    if (line.empty()) {
      continue;
    }
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos || equals == 0 ||
        equals == line.size() - 1) {
      throw std::invalid_argument(
          "line " + std::to_string(line_number) +
          ": expected key=value");
    }
    const std::string key = trim(line.substr(0, equals));
    const std::string value = trim(line.substr(equals + 1));

    if (key == "layer") {
      const std::vector<std::string> fields = split(value, ',');
      if (fields.size() != 4 && fields.size() != 5) {
        throw std::invalid_argument(
            "line " + std::to_string(line_number) +
            ": layer requires index,compute_ns,collective,payload_bytes[,start_policy]");
      }
      const std::uint64_t index =
          parse_unsigned(fields[0], "layer index", line_number);
      const std::uint64_t compute =
          parse_unsigned(fields[1], "compute_ns", line_number);
      const std::uint64_t payload =
          parse_unsigned(fields[3], "payload_bytes", line_number);
      if (index > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("layer index exceeds uint32 range");
      }
      LayerCollectiveKind collective = LayerCollectiveKind::None;
      if (fields[2] == "allreduce") {
        collective = LayerCollectiveKind::AllReduce;
      } else if (fields[2] == "reducescatter") {
        collective = LayerCollectiveKind::ReduceScatter;
      } else if (fields[2] == "allgather") {
        collective = LayerCollectiveKind::AllGather;
      } else if (fields[2] == "alltoall") {
        collective = LayerCollectiveKind::AllToAll;
      } else if (fields[2] != "none") {
        throw std::invalid_argument(
            "line " + std::to_string(line_number) +
            ": collective must be allreduce, reducescatter, allgather, alltoall, or none");
      }
      LayerStartPolicy start_policy = LayerStartPolicy::AfterPreviousFinish;
      if (fields.size() == 5) {
        if (fields[4] == "after_previous_compute") {
          start_policy = LayerStartPolicy::AfterPreviousCompute;
        } else if (fields[4] != "after_previous_finish") {
          throw std::invalid_argument(
              "line " + std::to_string(line_number) +
              ": start_policy must be after_previous_finish or after_previous_compute");
        }
      }
      result.layers.push_back(LayerSpec{
          static_cast<std::uint32_t>(index),
          compute,
          collective,
          payload,
          start_policy,
          "layer_" + std::to_string(index)});
      continue;
    }

    if (!seen_scalars.insert(key).second) {
      throw std::invalid_argument(
          "line " + std::to_string(line_number) +
          ": duplicate key " + key);
    }
    if (key == "collective_algorithm") {
      if (value == "auto") {
        result.collective_algorithm = CollectiveAlgorithmPolicy::Auto;
      } else if (value == "gather_reduce_broadcast") {
        result.collective_algorithm =
            CollectiveAlgorithmPolicy::GatherReduceBroadcast;
      } else if (value == "ring") {
        result.collective_algorithm = CollectiveAlgorithmPolicy::Ring;
      } else if (value == "direct") {
        result.collective_algorithm = CollectiveAlgorithmPolicy::Direct;
      } else {
        throw std::invalid_argument(
            "collective_algorithm must be auto, gather_reduce_broadcast, ring, or direct");
      }
      continue;
    }
    const std::uint64_t parsed = parse_unsigned(value, key, line_number);
    if (key == "schema_version") {
      result.schema_version = parsed;
    } else if (key == "deterministic_seed") {
      result.deterministic_seed = parsed;
    } else if (key == "endpoint_count") {
      if (parsed > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("endpoint_count exceeds uint32 range");
      }
      result.endpoint_count = static_cast<std::uint32_t>(parsed);
    } else if (key == "link_bandwidth_gbps") {
      result.link.bandwidth_gbps = parsed;
    } else if (key == "propagation_delay_ns") {
      result.link.propagation_delay_ns = parsed;
    } else if (key == "transaction_payload_bytes") {
      result.link.transaction_payload_bytes = parsed;
    } else if (key == "pdu_payload_bytes") {
      result.link.pdu_payload_bytes = parsed;
    } else if (key == "pdu_overhead_bytes") {
      result.link.pdu_overhead_bytes = parsed;
    } else if (key == "rail_count") {
      if (parsed > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("rail_count exceeds uint32 range");
      }
      result.link.rail_count = static_cast<std::uint32_t>(parsed);
    } else if (key == "switch_enabled") {
      if (parsed > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("switch_enabled exceeds uint32 range");
      }
      result.link.switch_enabled = static_cast<std::uint32_t>(parsed);
    } else if (key == "switch_queue_capacity_pdus") {
      result.link.switch_queue_capacity_pdus = parsed;
    } else if (key == "switch_latency_ns") {
      result.link.switch_latency_ns = parsed;
    } else if (key == "switch_credits_per_egress") {
      result.link.switch_credits_per_egress = parsed;
    } else if (key == "credit_return_latency_ns") {
      result.link.credit_return_latency_ns = parsed;
    } else if (key == "error_injection_pdu_id") {
      result.link.error_injection_pdu_id = parsed;
    } else if (key == "error_injection_attempts") {
      result.link.error_injection_attempts = parsed;
    } else if (key == "maximum_retries") {
      result.link.maximum_retries = parsed;
    } else if (key == "replay_latency_ns") {
      result.link.replay_latency_ns = parsed;
    } else if (key == "gpu_enabled") {
      if (parsed > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("gpu_enabled exceeds uint32 range");
      }
      result.gpu.enabled = static_cast<std::uint32_t>(parsed);
    } else if (key == "gpu_compute_units_per_device") {
      if (parsed > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(
            "gpu_compute_units_per_device exceeds uint32 range");
      }
      result.gpu.compute_units_per_device =
          static_cast<std::uint32_t>(parsed);
    } else if (key == "gpu_workgroup_slots_per_compute_unit") {
      if (parsed > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(
            "gpu_workgroup_slots_per_compute_unit exceeds uint32 range");
      }
      result.gpu.workgroup_slots_per_compute_unit =
          static_cast<std::uint32_t>(parsed);
    } else if (key == "gpu_wavefronts_per_workgroup") {
      if (parsed > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(
            "gpu_wavefronts_per_workgroup exceeds uint32 range");
      }
      result.gpu.wavefronts_per_workgroup =
          static_cast<std::uint32_t>(parsed);
    } else if (key == "gpu_wavefront_issue_width") {
      if (parsed > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(
            "gpu_wavefront_issue_width exceeds uint32 range");
      }
      result.gpu.wavefront_issue_width =
          static_cast<std::uint32_t>(parsed);
    } else if (key == "gpu_wavefront_issue_latency_ns") {
      result.gpu.wavefront_issue_latency_ns = parsed;
    } else if (key == "gpu_reduction_bytes_per_ns") {
      result.gpu.reduction_bytes_per_ns = parsed;
    } else {
      throw std::invalid_argument(
          "line " + std::to_string(line_number) +
          ": unknown key " + key);
    }
  }

  if (result.endpoint_count >= 2 && result.endpoint_count <= 1024) {
    for (std::uint32_t endpoint = 1; endpoint <= result.endpoint_count;
         ++endpoint) {
      result.tensor_parallel_group.members.push_back(
          contracts::DeviceId(endpoint));
    }
  }
  result.require_valid();
  return result;
}

WorkloadSpec parse_workload_file(const std::string& path) {
  std::ifstream input(path.c_str());
  if (!input) {
    throw std::runtime_error("could not open workload file: " + path);
  }
  return parse_workload(input);
}

}  // namespace runtime
}  // namespace simai
