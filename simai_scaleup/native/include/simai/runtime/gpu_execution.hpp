#ifndef SIMAI_RUNTIME_GPU_EXECUTION_HPP
#define SIMAI_RUNTIME_GPU_EXECUTION_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "simai/contracts/collective.hpp"
#include "simai/contracts/report.hpp"
#include "simai/runtime/workload.hpp"

namespace simai {
namespace runtime {

struct GpuWavefront {
  contracts::WavefrontId id;
};

struct GpuWorkgroup {
  contracts::WorkgroupId id;
  contracts::DeviceId device;
  contracts::ThreadBlockId source_thread_block;
  std::vector<GpuWavefront> wavefronts;
};

struct GpuKernel {
  contracts::KernelId id;
  contracts::OperationId source_operation;
  std::vector<GpuWorkgroup> workgroups;
};

struct GpuDispatch {
  std::size_t operation_index;
  contracts::DeviceId device;
  std::uint32_t compute_unit;
  std::uint32_t workgroup_slot;
  contracts::SimTimeNs dispatch_ns;
  contracts::SimTimeNs instruction_ready_ns;
  contracts::SimTimeNs local_execution_complete_ns;
  bool resource_stalled;
  contracts::SimTimeNs resource_stall_ns;
};

class GpuExecutionModel {
 public:
  GpuExecutionModel(const GpuConfig& config, std::uint32_t endpoint_count);

  GpuKernel lower(const contracts::MicroOp& operation) const;
  std::vector<GpuDispatch> admit(
      std::size_t operation_index,
      const contracts::MicroOp& operation,
      contracts::SimTimeNs ready_ns);
  std::vector<GpuDispatch> complete(
      std::size_t operation_index,
      contracts::SimTimeNs complete_ns);

  std::vector<contracts::GpuUtilization> utilization() const;
  bool enabled() const { return config_.enabled != 0; }

 private:
  struct PendingWorkgroup {
    std::size_t operation_index;
    contracts::DeviceId device;
    contracts::MicroOpKind kind;
    std::uint64_t bytes;
    contracts::SimTimeNs ready_ns;
  };

  struct Slot {
    bool occupied;
    std::size_t operation_index;
    contracts::SimTimeNs available_ns;
  };

  struct Assignment {
    std::size_t device_index;
    std::size_t slot_index;
  };

  struct DeviceState {
    std::vector<Slot> slots;
    std::deque<PendingWorkgroup> waiting;
    contracts::GpuUtilization counters;
    std::uint64_t resident_workgroups;
  };

  GpuDispatch dispatch(
      const PendingWorkgroup& pending,
      std::size_t device_index,
      std::size_t slot_index,
      contracts::SimTimeNs dispatch_ns);
  std::size_t free_slot(std::size_t device_index) const;

  GpuConfig config_;
  std::vector<DeviceState> devices_;
  std::unordered_map<std::size_t, Assignment> active_;
  std::unordered_set<std::size_t> outstanding_;
};

}  // namespace runtime
}  // namespace simai

#endif
