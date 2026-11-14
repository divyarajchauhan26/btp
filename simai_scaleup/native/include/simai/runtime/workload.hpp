#ifndef SIMAI_RUNTIME_WORKLOAD_HPP
#define SIMAI_RUNTIME_WORKLOAD_HPP

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

#include "simai/contracts/parallel_group.hpp"
#include "simai/contracts/validation.hpp"

namespace simai {
namespace runtime {

enum class LayerCollectiveKind {
  None,
  AllReduce,
  ReduceScatter,
  AllGather,
  AllToAll
};
enum class CollectiveAlgorithmPolicy {
  Auto,
  GatherReduceBroadcast,
  Ring,
  Direct
};
enum class LayerStartPolicy { AfterPreviousFinish, AfterPreviousCompute };

struct LayerSpec {
  std::uint32_t index;
  contracts::SimTimeNs compute_duration_ns;
  LayerCollectiveKind collective;
  std::uint64_t payload_bytes;
  LayerStartPolicy start_policy;
  std::string name;
};

struct LinkConfig {
  std::uint64_t bandwidth_gbps;
  contracts::SimTimeNs propagation_delay_ns;
  std::uint64_t transaction_payload_bytes;
  std::uint64_t pdu_payload_bytes;
  std::uint64_t pdu_overhead_bytes;
  std::uint32_t rail_count;
  std::uint32_t switch_enabled;
  std::uint64_t switch_queue_capacity_pdus;
  contracts::SimTimeNs switch_latency_ns;
  std::uint64_t switch_credits_per_egress;
  contracts::SimTimeNs credit_return_latency_ns;
  std::uint64_t error_injection_pdu_id;
  std::uint64_t error_injection_attempts;
  std::uint64_t maximum_retries;
  contracts::SimTimeNs replay_latency_ns;
};

struct GpuConfig {
  std::uint32_t enabled;
  std::uint32_t compute_units_per_device;
  std::uint32_t workgroup_slots_per_compute_unit;
  std::uint32_t wavefronts_per_workgroup;
  std::uint32_t wavefront_issue_width;
  contracts::SimTimeNs wavefront_issue_latency_ns;
  std::uint64_t reduction_bytes_per_ns;
};

struct WorkloadSpec {
  std::uint64_t schema_version;
  std::uint64_t deterministic_seed;
  std::uint32_t endpoint_count;
  CollectiveAlgorithmPolicy collective_algorithm;
  contracts::ParallelGroup tensor_parallel_group;
  LinkConfig link;
  GpuConfig gpu;
  std::vector<LayerSpec> layers;

  contracts::ValidationErrors validate() const;
  void require_valid() const;
};

WorkloadSpec parse_workload(std::istream& input);
WorkloadSpec parse_workload_file(const std::string& path);

}  // namespace runtime
}  // namespace simai

#endif
