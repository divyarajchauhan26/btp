#ifndef SIMAI_RUNTIME_FINE_SIMULATOR_HPP
#define SIMAI_RUNTIME_FINE_SIMULATOR_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "simai/runtime/fine_graph_execution.hpp"
#include "simai/runtime/fine_gpu_scheduler.hpp"
#include "simai/runtime/fine_local_memory.hpp"
#include "simai/runtime/fine_remote.hpp"
#include "simai/runtime/fine_synchronization.hpp"
#include "simai/runtime/transport_backend.hpp"

namespace simai {
namespace runtime {

struct FineExecutionTraceRecord {
  contracts::SimTimeNs time_ns;
  std::string event;
  contracts::DeviceId device;
  contracts::ComputeUnitId compute_unit;
  contracts::WorkgroupInstanceId workgroup;
  contracts::WavefrontInstanceId wavefront;
  contracts::GpuInstructionId instruction;
};

struct FineDispatchRecord {
  contracts::KernelInstanceId kernel;
  contracts::WorkgroupInstanceId workgroup;
  contracts::DeviceId device;
  contracts::ComputeUnitId compute_unit;
  contracts::SimTimeNs dispatch_ns;
  contracts::SimTimeNs completion_ns;
};

struct FineGraphNodeExecutionRecord {
  contracts::WorkloadEventId node;
  std::string name;
  std::size_t source_node_count;
  std::size_t kernel_count;
  contracts::SimTimeNs ready_ns;
  contracts::SimTimeNs start_ns;
  contracts::SimTimeNs completion_ns;
};

struct FineSimulationReport {
  contracts::SimTimeNs end_to_end_ns;
  std::uint64_t issued_instructions;
  std::uint64_t completed_instructions;
  std::uint64_t trace_wait_instructions;
  std::uint64_t local_memory_requests;
  std::uint64_t local_memory_bytes;
  std::uint64_t remote_requests;
  std::uint64_t remote_memory_bytes;
  std::uint64_t remote_command_pdus;
  std::uint64_t remote_response_pdus;
  std::uint64_t remote_payload_bytes;
  std::uint64_t remote_wire_bytes;
  std::vector<FineGraphNodeExecutionRecord> graph_nodes;
  std::vector<FineDispatchRecord> dispatches;
  std::vector<FineExecutionTraceRecord> trace;
  std::vector<contracts::RailUtilization> rails;
  std::vector<contracts::SwitchQueueUtilization> switch_queues;
  std::vector<FineLocalMemoryTierConfig> memory_tiers;
  std::vector<std::string> diagnostics;

  contracts::ValidationErrors validate() const;
  void require_valid() const;
};

struct FineSimulatorConfig {
  FineGpuSchedulerConfig scheduler;
  FineWavefrontEngineConfig wavefront;
  FineLocalMemorySystemConfig local_memory;
  LinkConfig link;
  contracts::BufferId semaphore_register_buffer;

  contracts::ValidationErrors validate() const;
  void require_valid() const;
};

class FineScaleUpSimulator {
 public:
  FineSimulationReport run(
      const std::vector<contracts::FineKernelDefinition>& kernels,
      const FineSimulatorConfig& config) const;
  FineSimulationReport run(
      const std::vector<contracts::FineKernelDefinition>& kernels,
      const FineSimulatorConfig& config,
      const SueTransportBackendFactory& backend_factory) const;
  FineSimulationReport run(
      const FineGraphExecutionPlan& plan,
      const FineSimulatorConfig& config) const;
  FineSimulationReport run(
      const FineGraphExecutionPlan& plan,
      const FineSimulatorConfig& config,
      const SueTransportBackendFactory& backend_factory) const;
};

std::string format_fine_report(const FineSimulationReport& report);

}  // namespace runtime
}  // namespace simai

#endif
