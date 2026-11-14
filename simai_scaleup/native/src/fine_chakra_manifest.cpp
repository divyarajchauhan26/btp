#include "simai/runtime/fine_chakra_manifest.hpp"

#include "simai/runtime/fine_synthetic_trace.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace simai {
namespace runtime {
namespace {

struct SourceNodeKey {
  std::uint64_t rank;
  std::uint64_t node;

  bool operator<(const SourceNodeKey& other) const {
    return std::make_pair(rank, node) <
        std::make_pair(other.rank, other.node);
  }
};

struct ComputeSpec {
  std::string name;
  SourceNodeKey source;
  std::uint32_t workgroups;
  std::uint32_t wavefronts;
  std::uint32_t residency;
  std::string cycles;
};

struct CollectiveSpec {
  std::string name;
  std::vector<SourceNodeKey> sources;
  std::string plan_path;
  std::string payload;
  MscclppFineLoweringConfig config;
};

struct SyntheticSpec {
  std::string name;
  SourceNodeKey source;
  std::string trace_path;
};

enum class BindingKind { Compute, Collective, Synthetic };

struct BindingOrder {
  BindingKind kind;
  std::size_t index;
};

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
  std::vector<std::string> result;
  std::stringstream stream(value);
  std::string item;
  while (std::getline(stream, item, delimiter)) result.push_back(trim(item));
  return result;
}

std::uint64_t number(const std::string& value, const char* field) {
  if (value.empty() || value[0] == '-') {
    throw std::invalid_argument(std::string(field) + " must be unsigned");
  }
  std::size_t consumed = 0;
  const std::uint64_t result = std::stoull(value, &consumed, 10);
  if (consumed != value.size()) {
    throw std::invalid_argument(std::string(field) + " must be unsigned");
  }
  return result;
}

std::uint32_t count(const std::string& value, const char* field) {
  const std::uint64_t parsed = number(value, field);
  if (parsed == 0 || parsed > UINT32_MAX) {
    throw std::invalid_argument(std::string(field) + " is out of range");
  }
  return static_cast<std::uint32_t>(parsed);
}

SourceNodeKey source_node(const std::string& value) {
  const std::vector<std::string> fields = split(value, ':');
  if (fields.size() != 2) {
    throw std::invalid_argument(
        "manifest source node must use zero-based rank:node syntax");
  }
  return SourceNodeKey{
      number(fields[0], "source rank"), number(fields[1], "source node")};
}

std::string parent_directory(const std::string& path) {
  const std::size_t separator = path.find_last_of("/\\");
  return separator == std::string::npos ? std::string() :
      path.substr(0, separator + 1);
}

bool absolute_path(const std::string& path) {
  return !path.empty() &&
      (path[0] == '/' || path[0] == '\\' ||
       (path.size() > 1 && path[1] == ':'));
}

std::string resolve_path(
    const std::string& directory, const std::string& path) {
  return absolute_path(path) ? path : directory + path;
}

const FineKernelGraphNode& resolve_node(
    const std::map<SourceNodeKey, const FineKernelGraphNode*>& nodes,
    const SourceNodeKey& key) {
  const auto found = nodes.find(key);
  if (found == nodes.end()) {
    throw std::invalid_argument(
        "manifest binding references an absent rank/source node");
  }
  return *found->second;
}

std::uint64_t collective_payload(
    const CollectiveSpec& spec,
    const std::map<SourceNodeKey, const FineKernelGraphNode*>& nodes) {
  if (spec.payload != "trace") {
    const std::uint64_t payload = number(spec.payload, "collective payload");
    if (payload == 0) {
      throw std::invalid_argument("collective payload must be nonzero");
    }
    return payload;
  }
  std::uint64_t payload = 0;
  for (const SourceNodeKey& source : spec.sources) {
    const FineKernelGraphNode& node = resolve_node(nodes, source);
    if (!node.has_payload) {
      throw std::invalid_argument(
          "trace payload requested but a collective node has no payload");
    }
    if (payload == 0) payload = node.payload_bytes;
    if (node.payload_bytes != payload) {
      throw std::invalid_argument(
          "grouped collective nodes disagree on trace payload");
    }
  }
  return payload;
}

std::uint64_t compute_cycles(
    const ComputeSpec& spec, const FineKernelGraphNode& node) {
  if (spec.cycles == "duration") {
    if (!node.has_duration || node.duration_ns == 0) {
      throw std::invalid_argument(
          "duration compute binding requires a nonzero Chakra duration");
    }
    return node.duration_ns;
  }
  const std::uint64_t cycles = number(spec.cycles, "compute cycles");
  if (cycles == 0) {
    throw std::invalid_argument("compute cycles must be nonzero");
  }
  return cycles;
}

}  // namespace

FineChakraExecutionProgram load_fine_chakra_execution_manifest(
    const std::string& path,
    std::uint64_t maximum_message_bytes) {
  std::ifstream input(path.c_str());
  if (!input) {
    throw std::invalid_argument(
        "could not open fine Chakra execution manifest: " + path);
  }
  const std::string directory = parent_directory(path);
  std::uint64_t schema_version = 0;
  std::vector<ChakraEtRankTrace> traces;
  std::vector<ComputeSpec> compute_specs;
  std::vector<CollectiveSpec> collective_specs;
  std::vector<SyntheticSpec> synthetic_specs;
  std::vector<BindingOrder> binding_order;
  std::string line;
  while (std::getline(input, line)) {
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) line.erase(comment);
    line = trim(line);
    if (line.empty()) continue;
    if (line.find("schema_version=") == 0) {
      schema_version = number(line.substr(15), "manifest schema version");
      continue;
    }
    if (line.find("trace=") == 0) {
      const std::vector<std::string> fields = split(line.substr(6), '|');
      if (fields.size() != 2) {
        throw std::invalid_argument("manifest trace requires two fields");
      }
      traces.push_back(ChakraEtRankTrace{
          resolve_path(directory, fields[1]),
          contracts::DeviceId(number(fields[0], "trace device"))});
      continue;
    }
    if (line.find("compute=") == 0) {
      const std::vector<std::string> fields = split(line.substr(8), '|');
      if (fields.size() != 6) {
        throw std::invalid_argument("manifest compute requires six fields");
      }
      compute_specs.push_back(ComputeSpec{
          fields[0],
          source_node(fields[1]),
          count(fields[2], "compute workgroups"),
          count(fields[3], "compute wavefronts"),
          count(fields[4], "compute residency"),
          fields[5]});
      binding_order.push_back(
          BindingOrder{BindingKind::Compute, compute_specs.size() - 1});
      continue;
    }
    if (line.find("collective=") == 0) {
      const std::vector<std::string> fields = split(line.substr(11), '|');
      if (fields.size() != 7) {
        throw std::invalid_argument("manifest collective requires seven fields");
      }
      std::vector<SourceNodeKey> sources;
      for (const std::string& source : split(fields[1], ',')) {
        sources.push_back(source_node(source));
      }
      collective_specs.push_back(CollectiveSpec{
          fields[0],
          sources,
          resolve_path(directory, fields[2]),
          fields[3],
          MscclppFineLoweringConfig{
              count(fields[4], "collective residency"),
              number(fields[5], "collective alignment"),
              number(fields[6], "collective reduce cycles")}});
      binding_order.push_back(
          BindingOrder{BindingKind::Collective, collective_specs.size() - 1});
      continue;
    }
    if (line.find("synthetic=") == 0) {
      const std::vector<std::string> fields = split(line.substr(10), '|');
      if (fields.size() != 3) {
        throw std::invalid_argument("manifest synthetic requires three fields");
      }
      synthetic_specs.push_back(SyntheticSpec{
          fields[0],
          source_node(fields[1]),
          resolve_path(directory, fields[2])});
      binding_order.push_back(
          BindingOrder{BindingKind::Synthetic, synthetic_specs.size() - 1});
      continue;
    }
    throw std::invalid_argument("unsupported fine Chakra manifest field");
  }
  if (schema_version != 1) {
    throw std::invalid_argument(
        "fine Chakra execution manifest requires schema version 1");
  }

  FineChakraExecutionProgram program;
  program.graph = import_chakra_et_files(traces, maximum_message_bytes);
  std::map<SourceNodeKey, const FineKernelGraphNode*> nodes;
  for (const FineKernelGraphNode& node : program.graph.nodes) {
    nodes.emplace(SourceNodeKey{node.source_rank, node.source_id}, &node);
  }

  std::vector<FineGraphBinding> bindings;
  for (const BindingOrder& ordered : binding_order) {
    if (ordered.kind == BindingKind::Compute) {
      const ComputeSpec& spec = compute_specs.at(ordered.index);
      const FineKernelGraphNode& node = resolve_node(nodes, spec.source);
      const std::uint64_t cycles = compute_cycles(spec, node);
      const FineComputeKernelTemplate description{
          contracts::KernelId(1),
          node.device,
          spec.workgroups,
          spec.wavefronts,
          spec.residency,
          {FineComputeOperationTemplate{
              contracts::FineGpuOperation::reduce(
                  contracts::GpuOperationId(1), cycles),
              0,
              0}}};
      bindings.push_back(FineGraphBinding::compute_kernel(
          spec.name, node.id, description));
    } else if (ordered.kind == BindingKind::Collective) {
      const CollectiveSpec& spec = collective_specs.at(ordered.index);
      std::vector<contracts::WorkloadEventId> source_nodes;
      for (const SourceNodeKey& source : spec.sources) {
        source_nodes.push_back(resolve_node(nodes, source).id);
      }
      bindings.push_back(FineGraphBinding::collective(
          spec.name,
          source_nodes,
          parse_mscclpp_plan_file(spec.plan_path),
          collective_payload(spec, nodes),
          spec.config));
    } else {
      const SyntheticSpec& spec = synthetic_specs.at(ordered.index);
      const FineKernelGraphNode& node = resolve_node(nodes, spec.source);
      const FineSyntheticTrace trace =
          parse_fine_synthetic_trace_file(spec.trace_path);
      bindings.push_back(FineGraphBinding::traced(
          spec.name, node.id, trace.kernel));
    }
  }
  program.execution = lower_fine_kernel_graph(program.graph, bindings);
  return program;
}

}  // namespace runtime
}  // namespace simai
