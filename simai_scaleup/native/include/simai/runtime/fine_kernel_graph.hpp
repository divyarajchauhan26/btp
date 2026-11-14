#ifndef SIMAI_RUNTIME_FINE_KERNEL_GRAPH_HPP
#define SIMAI_RUNTIME_FINE_KERNEL_GRAPH_HPP

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

#include "simai/contracts/collective.hpp"

namespace simai {
namespace runtime {

enum class FineKernelGraphNodeKind {
  Metadata,
  Compute,
  MemoryLoad,
  MemoryStore,
  Collective,
  Send,
  Receive
};

struct FineKernelGraphIo {
  std::string values;
  std::string shapes;
  std::string types;
};

struct FineKernelGraphAttribute {
  std::string name;
  bool has_integer;
  std::uint64_t integer;
  bool integer_is_signed;
  std::string string_value;
  std::string protobuf_hex;
};

struct FineKernelGraphNode {
  contracts::WorkloadEventId id;
  std::uint64_t source_rank;
  std::uint64_t source_id;
  std::string name;
  FineKernelGraphNodeKind kind;
  contracts::DeviceId device;
  std::string descriptor;
  bool has_start_time;
  std::uint64_t start_time_ns;
  bool has_duration;
  std::uint64_t duration_ns;
  bool has_collective;
  contracts::CollectiveKind collective;
  bool has_payload;
  std::uint64_t payload_bytes;
  bool has_peer;
  std::uint64_t peer_rank;
  FineKernelGraphIo inputs;
  FineKernelGraphIo outputs;
  std::vector<FineKernelGraphAttribute> attributes;
  std::vector<std::uint64_t> control_dependencies;
  std::vector<contracts::WorkloadEventId> dependencies;
};

struct FineKernelGraph {
  std::uint64_t schema_version;
  std::string source_format;
  std::string source_version;
  std::vector<FineKernelGraphNode> nodes;

  contracts::ValidationErrors validate() const;
  void require_valid() const;
  std::vector<contracts::WorkloadEventId> topological_order() const;
};

FineKernelGraph parse_fine_kernel_graph_sidecar(std::istream& input);
FineKernelGraph parse_fine_kernel_graph_sidecar_file(const std::string& path);
FineKernelGraph merge_fine_kernel_graphs(
    const std::vector<FineKernelGraph>& graphs);
const char* fine_kernel_graph_node_kind_name(FineKernelGraphNodeKind kind);
std::string format_fine_kernel_graph(const FineKernelGraph& graph);

}  // namespace runtime
}  // namespace simai

#endif
