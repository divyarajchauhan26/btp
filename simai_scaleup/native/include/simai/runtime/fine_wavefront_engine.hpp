#ifndef SIMAI_RUNTIME_FINE_WAVEFRONT_ENGINE_HPP
#define SIMAI_RUNTIME_FINE_WAVEFRONT_ENGINE_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "simai/contracts/fine_gpu.hpp"
#include "simai/runtime/fine_gpu_scheduler.hpp"
#include "simai/runtime/fine_instruction_stream.hpp"

namespace simai {
namespace runtime {

struct FineWavefrontEngineConfig {
  std::uint32_t device_count;
  std::uint32_t compute_units_per_device;
  std::uint32_t issue_width;
  contracts::SimTimeNs issue_latency_ns;
  contracts::SimTimeNs reduce_cycle_ns;
  FineInstructionGeneratorConfig instruction_generator;

  contracts::ValidationErrors validate() const;
  void require_valid() const;
};

struct FineInstructionIssue {
  contracts::KernelInstanceId kernel_instance;
  contracts::WorkgroupInstanceId workgroup_instance;
  contracts::WavefrontInstanceId wavefront_instance;
  contracts::WavefrontId wavefront;
  contracts::DeviceId device;
  contracts::ComputeUnitId compute_unit;
  contracts::SimTimeNs issue_ns;
  bool has_modeled_completion;
  contracts::SimTimeNs modeled_completion_ns;
  contracts::FineGpuInstruction instruction;
};

struct FineWorkgroupCompletion {
  contracts::KernelInstanceId kernel_instance;
  contracts::WorkgroupInstanceId workgroup_instance;
  contracts::KernelId kernel;
  contracts::WorkgroupId workgroup;
  contracts::DeviceId device;
  contracts::ComputeUnitId compute_unit;
  contracts::SimTimeNs completion_ns;
};

struct FineWavefrontSnapshot {
  contracts::WavefrontInstanceId instance;
  contracts::WavefrontId definition;
  std::size_t operation_index;
  bool instruction_pending;
  std::size_t outstanding_instructions;
  std::size_t outstanding_loads;
  std::size_t outstanding_stores;
  bool waitcnt_pending;
  std::uint64_t wait_threshold;
  bool completed;
  contracts::FineStallReason stall_reason;
};

class FineWavefrontEngine {
 public:
  explicit FineWavefrontEngine(const FineWavefrontEngineConfig& config);

  void admit(
      const FineWorkgroupDispatch& dispatch,
      const contracts::FineWorkgroupDefinition& workgroup,
      contracts::SimTimeNs ready_ns);
  void admit(
      const FineWorkgroupDispatch& dispatch,
      const contracts::FineWorkgroupDefinition& workgroup,
      contracts::SimTimeNs ready_ns,
      contracts::SimTimeNs trace_origin_ns);
  std::vector<FineInstructionIssue> issue_ready(contracts::SimTimeNs now_ns);
  void complete_instruction(
      contracts::WavefrontInstanceId wavefront_instance,
      contracts::GpuInstructionId instruction,
      contracts::SimTimeNs completion_ns);
  std::vector<FineWorkgroupCompletion> take_completed_workgroups();

  FineWavefrontSnapshot snapshot(
      contracts::WavefrontInstanceId wavefront_instance) const;
  std::size_t active_workgroups() const { return workgroups_.size(); }
  std::size_t active_wavefronts() const { return wavefronts_.size(); }
  bool drained() const;

 private:
  struct OutstandingInstruction {
    contracts::SimTimeNs issue_ns;
    contracts::FineGpuInstructionKind kind;
  };

  struct WavefrontState {
    contracts::WavefrontInstanceId instance;
    contracts::WavefrontId definition;
    std::vector<contracts::FineGpuOperation> operations;
    std::size_t operation_index;
    std::unique_ptr<FineInstructionStream> stream;
    contracts::SimTimeNs trace_origin_ns;
    contracts::SimTimeNs ready_ns;
    bool instruction_pending;
    contracts::GpuInstructionId pending_instruction;
    contracts::SimTimeNs pending_issue_ns;
    contracts::SimTimeNs pending_completion_ns;
    std::unordered_map<contracts::GpuInstructionId, OutstandingInstruction>
        outstanding_instructions;
    std::size_t outstanding_loads;
    std::size_t outstanding_stores;
    bool waitcnt_pending;
    contracts::GpuInstructionId waitcnt_instruction;
    std::uint64_t wait_threshold;
    bool completed;
    contracts::FineStallReason stall_reason;
  };

  struct WorkgroupState {
    FineWorkgroupDispatch dispatch;
    contracts::WorkgroupId definition;
    std::vector<contracts::WavefrontInstanceId> wavefronts;
    bool completion_queued;
  };

  struct WavefrontLocation {
    contracts::WorkgroupInstanceId workgroup;
    std::size_t wavefront_index;
  };

  struct ComputeUnitState {
    std::vector<contracts::WavefrontInstanceId> wavefronts;
    std::size_t next_wavefront;
    contracts::SimTimeNs next_issue_ns;
  };

  std::size_t compute_unit_index(
      contracts::DeviceId device,
      contracts::ComputeUnitId compute_unit) const;
  WavefrontState& wavefront(contracts::WavefrontInstanceId instance);
  const WavefrontState& wavefront(
      contracts::WavefrontInstanceId instance) const;
  bool prepare_stream(WavefrontState& state);
  contracts::WavefrontInstanceId select_ready_wavefront(
      ComputeUnitState& compute_unit, contracts::SimTimeNs now_ns);
  void queue_workgroup_completion(
      WorkgroupState& workgroup, contracts::SimTimeNs completion_ns);
  void erase_workgroup(contracts::WorkgroupInstanceId workgroup);

  FineWavefrontEngineConfig config_;
  std::vector<ComputeUnitState> compute_units_;
  std::unordered_map<contracts::WorkgroupInstanceId, WorkgroupState> workgroups_;
  std::unordered_map<contracts::WavefrontInstanceId, WavefrontState> wavefronts_;
  std::unordered_map<contracts::WavefrontInstanceId, WavefrontLocation>
      wavefront_locations_;
  std::deque<FineWorkgroupCompletion> completed_workgroups_;
  std::uint64_t next_wavefront_instance_;
  std::uint64_t next_instruction_id_;
};

}  // namespace runtime
}  // namespace simai

#endif
