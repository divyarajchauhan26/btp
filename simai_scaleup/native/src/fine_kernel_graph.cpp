#include "simai/runtime/fine_kernel_graph.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace simai {
namespace runtime {
namespace {

std::string trim(const std::string& value) {
  std::size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
  std::size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
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

contracts::CollectiveKind collective(const std::string& value) {
  if (value == "allreduce") return contracts::CollectiveKind::AllReduce;
  if (value == "reduce_scatter") {
    return contracts::CollectiveKind::ReduceScatter;
  }
  if (value == "allgather") return contracts::CollectiveKind::AllGather;
  if (value == "alltoall") return contracts::CollectiveKind::AllToAll;
  if (value == "broadcast") return contracts::CollectiveKind::Broadcast;
  throw std::invalid_argument("unsupported sidecar collective kind");
}

struct LaterEventId {
  bool operator()(
      contracts::WorkloadEventId lhs,
      contracts::WorkloadEventId rhs) const {
    return rhs < lhs;
  }
};

}  // namespace

contracts::ValidationErrors FineKernelGraph::validate() const {
  contracts::ValidationErrors errors;
  if (schema_version != 1 || source_format.empty() || source_version.empty() ||
      nodes.empty()) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "fine kernel graph requires schema version 1 and nodes");
  }
  std::unordered_set<contracts::WorkloadEventId> ids;
  std::set<std::pair<std::uint64_t, std::uint64_t>> source_ids;
  for (const FineKernelGraphNode& node : nodes) {
    if (!node.id.valid() || !node.device.valid() || node.name.empty() ||
        node.descriptor.empty() || !ids.insert(node.id).second) {
      contracts::append_error(
          errors,
          contracts::ValidationCode::InvalidIdentity,
          "fine kernel graph node identities and descriptors must be unique");
    }
    if (!source_ids.insert(
            std::make_pair(node.source_rank, node.source_id)).second) {
      contracts::append_error(
          errors,
          contracts::ValidationCode::DuplicateIdentity,
          "fine kernel graph source rank/node identities must be unique");
    }
    if ((!node.has_start_time && node.start_time_ns != 0) ||
        (!node.has_duration && node.duration_ns != 0)) {
      contracts::append_error(
          errors,
          contracts::ValidationCode::InvalidValue,
          "unknown fine graph timing fields must remain zero");
    }
    std::set<std::string> attribute_names;
    for (const FineKernelGraphAttribute& attribute : node.attributes) {
      if (attribute.name.empty() || attribute.protobuf_hex.empty() ||
          !attribute_names.insert(attribute.name).second) {
        contracts::append_error(
            errors,
            contracts::ValidationCode::InvalidValue,
            "fine graph attributes must have unique names and raw values");
      }
    }
    const bool may_carry_payload =
        node.kind == FineKernelGraphNodeKind::MemoryLoad ||
        node.kind == FineKernelGraphNodeKind::MemoryStore ||
        node.kind == FineKernelGraphNodeKind::Collective ||
        node.kind == FineKernelGraphNodeKind::Send ||
        node.kind == FineKernelGraphNodeKind::Receive;
    if ((node.has_collective &&
         node.kind != FineKernelGraphNodeKind::Collective) ||
        (node.has_payload &&
         (!may_carry_payload || node.payload_bytes == 0)) ||
        (!node.has_payload && node.payload_bytes != 0) ||
        (node.has_peer && node.kind != FineKernelGraphNodeKind::Send &&
         node.kind != FineKernelGraphNodeKind::Receive)) {
      contracts::append_error(
          errors,
          contracts::ValidationCode::InvalidValue,
          "fine kernel graph node kind and payload are inconsistent");
    }
  }
  for (const FineKernelGraphNode& node : nodes) {
    std::unordered_set<contracts::WorkloadEventId> dependencies;
    for (contracts::WorkloadEventId dependency : node.dependencies) {
      if (dependency == node.id || ids.count(dependency) == 0 ||
          !dependencies.insert(dependency).second) {
        contracts::append_error(
            errors,
            contracts::ValidationCode::MissingDependency,
            "fine kernel graph dependency is missing, duplicate, or self-referential");
      }
    }
  }
  if (errors.empty()) {
    try {
      topological_order();
    } catch (const std::invalid_argument&) {
      contracts::append_error(
          errors,
          contracts::ValidationCode::DependencyCycle,
          "fine kernel graph contains a cycle");
    }
  }
  return errors;
}

void FineKernelGraph::require_valid() const {
  contracts::require_no_errors(validate(), "FineKernelGraph");
}

std::vector<contracts::WorkloadEventId> FineKernelGraph::topological_order()
    const {
  std::map<contracts::WorkloadEventId, std::size_t> indegree;
  std::map<contracts::WorkloadEventId,
           std::vector<contracts::WorkloadEventId>> dependents;
  for (const FineKernelGraphNode& node : nodes) indegree[node.id] = 0;
  for (const FineKernelGraphNode& node : nodes) {
    for (contracts::WorkloadEventId dependency : node.dependencies) {
      if (indegree.count(dependency) == 0) {
        throw std::invalid_argument("fine kernel graph dependency is missing");
      }
      ++indegree[node.id];
      dependents[dependency].push_back(node.id);
    }
  }
  std::priority_queue<
      contracts::WorkloadEventId,
      std::vector<contracts::WorkloadEventId>,
      LaterEventId> ready;
  for (const auto& entry : indegree) {
    if (entry.second == 0) ready.push(entry.first);
  }
  std::vector<contracts::WorkloadEventId> order;
  while (!ready.empty()) {
    const contracts::WorkloadEventId current = ready.top();
    ready.pop();
    order.push_back(current);
    for (contracts::WorkloadEventId dependent : dependents[current]) {
      if (--indegree[dependent] == 0) ready.push(dependent);
    }
  }
  if (order.size() != nodes.size()) {
    throw std::invalid_argument("fine kernel graph contains a cycle");
  }
  return order;
}

FineKernelGraph parse_fine_kernel_graph_sidecar(std::istream& input) {
  FineKernelGraph graph{0, "fine_graph_sidecar", "1", {}};
  std::string line;
  while (std::getline(input, line)) {
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) line.erase(comment);
    line = trim(line);
    if (line.empty()) continue;
    if (line.find("schema_version=") == 0) {
      graph.schema_version = number(line.substr(15), "schema_version");
      continue;
    }
    if (line.find("node=") != 0) {
      throw std::invalid_argument("unsupported fine graph sidecar field");
    }
    const std::vector<std::string> fields = split(line.substr(5), '|');
    if (fields.size() != 8) {
      throw std::invalid_argument("fine graph node requires eight fields");
    }
    FineKernelGraphNode node{};
    node.id = contracts::WorkloadEventId(number(fields[0], "node id"));
    node.source_rank = number(fields[3], "device") - 1;
    node.source_id = node.id.value();
    node.name = fields[1];
    node.kind = fields[2] == "compute"
        ? FineKernelGraphNodeKind::Compute
        : FineKernelGraphNodeKind::Collective;
    if (fields[2] != "compute" && fields[2] != "collective") {
      throw std::invalid_argument("fine graph node kind is unsupported");
    }
    node.device = contracts::DeviceId(number(fields[3], "device"));
    node.descriptor = fields[4];
    node.has_start_time = false;
    node.start_time_ns = 0;
    node.has_duration = false;
    node.duration_ns = 0;
    node.has_collective = node.kind == FineKernelGraphNodeKind::Collective;
    node.collective = node.has_collective
        ? collective(fields[5])
        : contracts::CollectiveKind::AllReduce;
    node.payload_bytes = number(fields[6], "payload");
    node.has_payload = node.payload_bytes != 0;
    node.has_peer = false;
    node.peer_rank = 0;
    if (fields[7] != "-") {
      for (const std::string& dependency : split(fields[7], ',')) {
        node.dependencies.push_back(contracts::WorkloadEventId(
            number(dependency, "dependency")));
      }
    }
    graph.nodes.push_back(node);
  }
  graph.require_valid();
  return graph;
}

FineKernelGraph parse_fine_kernel_graph_sidecar_file(const std::string& path) {
  std::ifstream input(path.c_str());
  if (!input) {
    throw std::invalid_argument("could not open fine graph sidecar: " + path);
  }
  return parse_fine_kernel_graph_sidecar(input);
}

FineKernelGraph merge_fine_kernel_graphs(
    const std::vector<FineKernelGraph>& graphs) {
  if (graphs.empty()) {
    throw std::invalid_argument("fine kernel graph merge requires inputs");
  }
  const std::string source_format = graphs.front().source_format;
  const std::string source_version = graphs.front().source_version;
  struct NodeReference {
    std::size_t graph_index;
    const FineKernelGraphNode* node;
  };
  std::vector<NodeReference> ordered;
  for (std::size_t graph_index = 0; graph_index < graphs.size(); ++graph_index) {
    const FineKernelGraph& graph = graphs[graph_index];
    graph.require_valid();
    if (graph.source_format != source_format ||
        graph.source_version != source_version) {
      throw std::invalid_argument(
          "merged fine kernel graphs must use one source format/version");
    }
    for (const FineKernelGraphNode& node : graph.nodes) {
      ordered.push_back(NodeReference{graph_index, &node});
    }
  }
  std::sort(
      ordered.begin(), ordered.end(),
      [](const NodeReference& lhs, const NodeReference& rhs) {
        return std::tie(lhs.node->source_rank, lhs.node->source_id) <
            std::tie(rhs.node->source_rank, rhs.node->source_id);
      });
  for (std::size_t index = 1; index < ordered.size(); ++index) {
    if (ordered[index - 1].node->source_rank ==
            ordered[index].node->source_rank &&
        ordered[index - 1].node->source_id == ordered[index].node->source_id) {
      throw std::invalid_argument(
          "merged fine kernel graph has duplicate rank/node identity");
    }
  }

  using OldIdentity = std::pair<std::size_t, contracts::WorkloadEventId>;
  std::map<OldIdentity, contracts::WorkloadEventId> identities;
  for (std::size_t index = 0; index < ordered.size(); ++index) {
    identities.emplace(
        OldIdentity(ordered[index].graph_index, ordered[index].node->id),
        contracts::WorkloadEventId(index + 1));
  }

  FineKernelGraph merged{1, source_format, source_version, {}};
  merged.nodes.reserve(ordered.size());
  for (std::size_t index = 0; index < ordered.size(); ++index) {
    FineKernelGraphNode node = *ordered[index].node;
    node.id = contracts::WorkloadEventId(index + 1);
    node.dependencies.clear();
    for (contracts::WorkloadEventId dependency :
         ordered[index].node->dependencies) {
      const auto found = identities.find(
          OldIdentity(ordered[index].graph_index, dependency));
      if (found == identities.end()) {
        throw std::invalid_argument(
            "fine kernel graph merge could not remap dependency");
      }
      node.dependencies.push_back(found->second);
    }
    merged.nodes.push_back(node);
  }
  merged.require_valid();
  return merged;
}

const char* fine_kernel_graph_node_kind_name(FineKernelGraphNodeKind kind) {
  switch (kind) {
    case FineKernelGraphNodeKind::Metadata:
      return "metadata";
    case FineKernelGraphNodeKind::Compute:
      return "compute";
    case FineKernelGraphNodeKind::MemoryLoad:
      return "memory_load";
    case FineKernelGraphNodeKind::MemoryStore:
      return "memory_store";
    case FineKernelGraphNodeKind::Collective:
      return "collective";
    case FineKernelGraphNodeKind::Send:
      return "send";
    case FineKernelGraphNodeKind::Receive:
      return "receive";
  }
  throw std::invalid_argument("unknown fine kernel graph node kind");
}

std::string format_fine_kernel_graph(const FineKernelGraph& graph) {
  graph.require_valid();
  std::ostringstream output;
  output << "fine_graph_schema_version=" << graph.schema_version << '\n'
         << "source_format=" << graph.source_format << '\n'
         << "source_version=" << graph.source_version << '\n'
         << "node_count=" << graph.nodes.size() << '\n';
  for (const FineKernelGraphNode& node : graph.nodes) {
    output << "node=id:" << node.id.value()
           << ",source_rank:" << node.source_rank
           << ",source_id:" << node.source_id
           << ",name:" << node.name
           << ",kind:" << fine_kernel_graph_node_kind_name(node.kind)
           << ",device:" << node.device.value()
           << ",descriptor:" << node.descriptor
           << ",start_known:" << (node.has_start_time ? 1 : 0)
           << ",start_ns:" << node.start_time_ns
           << ",duration_known:" << (node.has_duration ? 1 : 0)
           << ",duration_ns:" << node.duration_ns
           << ",payload_known:" << (node.has_payload ? 1 : 0)
           << ",payload_bytes:" << node.payload_bytes;
    if (node.has_peer) output << ",peer_rank:" << node.peer_rank;
    output << ",dependencies:";
    if (node.dependencies.empty()) {
      output << '-';
    } else {
      for (std::size_t index = 0; index < node.dependencies.size(); ++index) {
        if (index != 0) output << ',';
        output << node.dependencies[index].value();
      }
    }
    output << '\n';
  }
  output << "topological_order=";
  const std::vector<contracts::WorkloadEventId> order =
      graph.topological_order();
  for (std::size_t index = 0; index < order.size(); ++index) {
    if (index != 0) output << ',';
    output << order[index].value();
  }
  output << '\n';
  return output.str();
}

}  // namespace runtime
}  // namespace simai
