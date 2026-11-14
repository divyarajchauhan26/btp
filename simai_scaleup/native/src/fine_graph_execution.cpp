#include "simai/runtime/fine_graph_execution.hpp"

#include <algorithm>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace simai {
namespace runtime {
namespace {

struct LaterEventId {
  bool operator()(
      contracts::WorkloadEventId lhs,
      contracts::WorkloadEventId rhs) const {
    return rhs < lhs;
  }
};

void rebase_reference(
    contracts::FineMemoryReference& reference,
    contracts::DeviceId old_device,
    contracts::DeviceId new_device) {
  if (reference.owner == old_device) reference.owner = new_device;
}

void rebase_compute_template(
    FineComputeKernelTemplate& description,
    contracts::DeviceId device,
    contracts::KernelId kernel) {
  const contracts::DeviceId old_device = description.device;
  description.device = device;
  description.kernel = kernel;
  for (FineComputeOperationTemplate& operation : description.operations) {
    if (operation.operation.has_source) {
      rebase_reference(operation.operation.source, old_device, device);
    }
    if (operation.operation.has_destination) {
      rebase_reference(operation.operation.destination, old_device, device);
    }
    if (operation.operation.has_semaphore &&
        operation.operation.semaphore_owner == old_device) {
      operation.operation.semaphore_owner = device;
    }
  }
}

const FineKernelGraphNode& graph_node(
    const std::map<contracts::WorkloadEventId, const FineKernelGraphNode*>&
        nodes,
    contracts::WorkloadEventId id) {
  const auto found = nodes.find(id);
  if (found == nodes.end()) {
    throw std::invalid_argument("fine graph binding references an absent node");
  }
  return *found->second;
}

}  // namespace

FineGraphBinding FineGraphBinding::passthrough(
    const std::string& name,
    const std::vector<contracts::WorkloadEventId>& source_nodes) {
  FineGraphBinding binding{};
  binding.name = name;
  binding.kind = FineGraphBindingKind::Passthrough;
  binding.source_nodes = source_nodes;
  return binding;
}

FineGraphBinding FineGraphBinding::compute_kernel(
    const std::string& name,
    contracts::WorkloadEventId source_node,
    const FineComputeKernelTemplate& description) {
  FineGraphBinding binding{};
  binding.name = name;
  binding.kind = FineGraphBindingKind::Compute;
  binding.source_nodes.push_back(source_node);
  binding.compute = description;
  return binding;
}

FineGraphBinding FineGraphBinding::traced(
    const std::string& name,
    contracts::WorkloadEventId source_node,
    const contracts::FineKernelDefinition& kernel) {
  FineGraphBinding binding{};
  binding.name = name;
  binding.kind = FineGraphBindingKind::TracedKernel;
  binding.source_nodes.push_back(source_node);
  binding.traced_kernel = kernel;
  return binding;
}

FineGraphBinding FineGraphBinding::collective(
    const std::string& name,
    const std::vector<contracts::WorkloadEventId>& source_nodes,
    const MscclppPlan& plan,
    std::uint64_t payload_bytes,
    const MscclppFineLoweringConfig& config) {
  FineGraphBinding binding{};
  binding.name = name;
  binding.kind = FineGraphBindingKind::Collective;
  binding.source_nodes = source_nodes;
  binding.collective_plan = plan;
  binding.payload_bytes = payload_bytes;
  binding.collective_config = config;
  return binding;
}

contracts::ValidationErrors FineGraphExecutionPlan::validate() const {
  contracts::ValidationErrors errors;
  if (nodes.empty()) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "fine graph execution plan requires nodes");
    return errors;
  }
  std::unordered_set<contracts::WorkloadEventId> node_ids;
  std::unordered_set<contracts::KernelId> kernel_ids;
  for (const FineGraphExecutionNode& node : nodes) {
    if (!node.id.valid() || node.name.empty() || node.source_nodes.empty() ||
        !node_ids.insert(node.id).second) {
      contracts::append_error(
          errors,
          contracts::ValidationCode::InvalidIdentity,
          "fine execution nodes require unique identities, names, and sources");
    }
    std::unordered_set<contracts::WorkloadEventId> source_nodes;
    for (contracts::WorkloadEventId source : node.source_nodes) {
      if (!source.valid() || !source_nodes.insert(source).second) {
        contracts::append_error(
            errors,
            contracts::ValidationCode::DuplicateIdentity,
            "fine execution node source identities must be unique");
      }
    }
    for (const contracts::FineKernelDefinition& kernel : node.kernels) {
      const contracts::ValidationErrors kernel_errors = kernel.validate();
      errors.insert(errors.end(), kernel_errors.begin(), kernel_errors.end());
      if (!kernel_ids.insert(kernel.id).second) {
        contracts::append_error(
            errors,
            contracts::ValidationCode::DuplicateIdentity,
            "fine execution kernel identities must be globally unique");
      }
    }
  }
  for (const FineGraphExecutionNode& node : nodes) {
    std::unordered_set<contracts::WorkloadEventId> dependencies;
    for (contracts::WorkloadEventId dependency : node.dependencies) {
      if (dependency == node.id || node_ids.count(dependency) == 0 ||
          !dependencies.insert(dependency).second) {
        contracts::append_error(
            errors,
            contracts::ValidationCode::MissingDependency,
            "fine execution dependency is missing, duplicate, or self-referential");
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
          "fine execution plan contains a cycle");
    }
  }
  return errors;
}

void FineGraphExecutionPlan::require_valid() const {
  contracts::require_no_errors(validate(), "FineGraphExecutionPlan");
}

std::vector<contracts::WorkloadEventId>
FineGraphExecutionPlan::topological_order() const {
  std::map<contracts::WorkloadEventId, std::size_t> indegree;
  std::map<contracts::WorkloadEventId,
           std::vector<contracts::WorkloadEventId>> dependents;
  for (const FineGraphExecutionNode& node : nodes) indegree[node.id] = 0;
  for (const FineGraphExecutionNode& node : nodes) {
    for (contracts::WorkloadEventId dependency : node.dependencies) {
      if (indegree.count(dependency) == 0) {
        throw std::invalid_argument("fine execution dependency is absent");
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
    throw std::invalid_argument("fine execution plan contains a cycle");
  }
  return order;
}

FineGraphExecutionPlan lower_fine_kernel_graph(
    const FineKernelGraph& graph,
    const std::vector<FineGraphBinding>& requested_bindings) {
  graph.require_valid();
  std::map<contracts::WorkloadEventId, const FineKernelGraphNode*> graph_nodes;
  for (const FineKernelGraphNode& node : graph.nodes) {
    graph_nodes.emplace(node.id, &node);
  }

  std::vector<FineGraphBinding> bindings = requested_bindings;
  std::set<contracts::WorkloadEventId> bound;
  for (const FineGraphBinding& binding : bindings) {
    if (binding.name.empty() || binding.source_nodes.empty()) {
      throw std::invalid_argument("fine graph binding requires name and nodes");
    }
    for (contracts::WorkloadEventId source : binding.source_nodes) {
      static_cast<void>(graph_node(graph_nodes, source));
      if (!bound.insert(source).second) {
        throw std::invalid_argument(
            "fine graph node is assigned to multiple bindings");
      }
    }
  }
  for (const FineKernelGraphNode& node : graph.nodes) {
    if (bound.count(node.id) != 0) continue;
    if (node.kind != FineKernelGraphNodeKind::Metadata) {
      throw std::invalid_argument(
          "executable fine graph node has no kernel binding");
    }
    bindings.push_back(FineGraphBinding::passthrough(node.name, {node.id}));
    bound.insert(node.id);
  }

  std::map<contracts::WorkloadEventId, contracts::WorkloadEventId>
      source_to_execution;
  for (std::size_t index = 0; index < bindings.size(); ++index) {
    const contracts::WorkloadEventId execution_id(index + 1);
    for (contracts::WorkloadEventId source : bindings[index].source_nodes) {
      source_to_execution.emplace(source, execution_id);
    }
  }

  FineGraphExecutionPlan result;
  std::uint64_t next_kernel_id = 1;
  for (std::size_t index = 0; index < bindings.size(); ++index) {
    const FineGraphBinding& binding = bindings[index];
    FineGraphExecutionNode execution{};
    execution.id = contracts::WorkloadEventId(index + 1);
    execution.name = binding.name;
    execution.source_nodes = binding.source_nodes;

    std::set<contracts::WorkloadEventId> dependencies;
    for (contracts::WorkloadEventId source_id : binding.source_nodes) {
      const FineKernelGraphNode& source = graph_node(graph_nodes, source_id);
      for (contracts::WorkloadEventId dependency : source.dependencies) {
        const contracts::WorkloadEventId execution_dependency =
            source_to_execution.at(dependency);
        if (execution_dependency != execution.id) {
          dependencies.insert(execution_dependency);
        }
      }
    }
    execution.dependencies.assign(dependencies.begin(), dependencies.end());

    if (binding.kind == FineGraphBindingKind::Compute) {
      if (binding.source_nodes.size() != 1) {
        throw std::invalid_argument(
            "compute binding must describe exactly one graph node");
      }
      const FineKernelGraphNode& source =
          graph_node(graph_nodes, binding.source_nodes.front());
      if (source.kind != FineKernelGraphNodeKind::Compute &&
          source.kind != FineKernelGraphNodeKind::MemoryLoad &&
          source.kind != FineKernelGraphNodeKind::MemoryStore) {
        throw std::invalid_argument(
            "compute binding is incompatible with graph node kind");
      }
      FineComputeKernelTemplate description = binding.compute;
      rebase_compute_template(
          description,
          source.device,
          contracts::KernelId(next_kernel_id++));
      execution.kernels.push_back(lower_compute_kernel(description));
    } else if (binding.kind == FineGraphBindingKind::TracedKernel) {
      if (binding.source_nodes.size() != 1) {
        throw std::invalid_argument(
            "traced kernel binding must describe exactly one graph node");
      }
      const FineKernelGraphNode& source =
          graph_node(graph_nodes, binding.source_nodes.front());
      if (source.kind == FineKernelGraphNodeKind::Metadata) {
        throw std::invalid_argument(
            "metadata graph nodes cannot bind a traced kernel");
      }
      contracts::FineKernelDefinition kernel = binding.traced_kernel;
      if (kernel.device != source.device) {
        throw std::invalid_argument(
            "traced kernel device conflicts with its graph node");
      }
      kernel.id = contracts::KernelId(next_kernel_id++);
      kernel.require_valid();
      execution.kernels.push_back(kernel);
    } else if (binding.kind == FineGraphBindingKind::Collective) {
      if (binding.payload_bytes == 0) {
        throw std::invalid_argument(
            "collective binding requires an explicit payload");
      }
      std::set<contracts::DeviceId> source_devices;
      for (contracts::WorkloadEventId source_id : binding.source_nodes) {
        const FineKernelGraphNode& source = graph_node(graph_nodes, source_id);
        if (source.kind != FineKernelGraphNodeKind::Collective ||
            (source.has_payload &&
             source.payload_bytes != binding.payload_bytes)) {
          throw std::invalid_argument(
              "collective binding conflicts with Chakra node semantics");
        }
        source_devices.insert(source.device);
      }
      std::vector<contracts::FineKernelDefinition> kernels =
          lower_mscclpp_fine_kernels(
              binding.collective_plan,
              binding.payload_bytes,
              binding.collective_config);
      std::set<contracts::DeviceId> kernel_devices;
      for (contracts::FineKernelDefinition& kernel : kernels) {
        kernel_devices.insert(kernel.device);
        kernel.id = contracts::KernelId(next_kernel_id++);
      }
      if (source_devices != kernel_devices) {
        throw std::invalid_argument(
            "collective binding participants do not match plan devices");
      }
      execution.kernels = kernels;
    }
    result.nodes.push_back(execution);
  }
  result.require_valid();
  return result;
}

}  // namespace runtime
}  // namespace simai
