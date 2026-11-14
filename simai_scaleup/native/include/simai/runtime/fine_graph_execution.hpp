#ifndef SIMAI_RUNTIME_FINE_GRAPH_EXECUTION_HPP
#define SIMAI_RUNTIME_FINE_GRAPH_EXECUTION_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "simai/runtime/fine_compute.hpp"
#include "simai/runtime/fine_kernel_graph.hpp"
#include "simai/runtime/mscclpp_plan.hpp"

namespace simai {
namespace runtime {

enum class FineGraphBindingKind { Passthrough, Compute, TracedKernel, Collective };

struct FineGraphBinding {
  std::string name;
  FineGraphBindingKind kind;
  std::vector<contracts::WorkloadEventId> source_nodes;
  FineComputeKernelTemplate compute;
  contracts::FineKernelDefinition traced_kernel;
  MscclppPlan collective_plan;
  std::uint64_t payload_bytes;
  MscclppFineLoweringConfig collective_config;

  static FineGraphBinding passthrough(
      const std::string& name,
      const std::vector<contracts::WorkloadEventId>& source_nodes);
  static FineGraphBinding compute_kernel(
      const std::string& name,
      contracts::WorkloadEventId source_node,
      const FineComputeKernelTemplate& description);
  static FineGraphBinding traced(
      const std::string& name,
      contracts::WorkloadEventId source_node,
      const contracts::FineKernelDefinition& kernel);
  static FineGraphBinding collective(
      const std::string& name,
      const std::vector<contracts::WorkloadEventId>& source_nodes,
      const MscclppPlan& plan,
      std::uint64_t payload_bytes,
      const MscclppFineLoweringConfig& config);
};

struct FineGraphExecutionNode {
  contracts::WorkloadEventId id;
  std::string name;
  std::vector<contracts::WorkloadEventId> source_nodes;
  std::vector<contracts::WorkloadEventId> dependencies;
  std::vector<contracts::FineKernelDefinition> kernels;
};

struct FineGraphExecutionPlan {
  std::vector<FineGraphExecutionNode> nodes;

  contracts::ValidationErrors validate() const;
  void require_valid() const;
  std::vector<contracts::WorkloadEventId> topological_order() const;
};

FineGraphExecutionPlan lower_fine_kernel_graph(
    const FineKernelGraph& graph,
    const std::vector<FineGraphBinding>& bindings);

}  // namespace runtime
}  // namespace simai

#endif
