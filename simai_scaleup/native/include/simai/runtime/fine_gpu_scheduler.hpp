#ifndef SIMAI_RUNTIME_FINE_GPU_SCHEDULER_HPP
#define SIMAI_RUNTIME_FINE_GPU_SCHEDULER_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "simai/contracts/fine_gpu.hpp"

namespace simai {
namespace runtime {

struct FineGpuSchedulerConfig {
  std::uint32_t device_count;
  std::uint32_t compute_units_per_device;
  std::uint32_t workgroup_slots_per_compute_unit;
  contracts::FineSchedulingPolicy policy;
  std::uint64_t seed;

  contracts::ValidationErrors validate() const;
  void require_valid() const;
};

struct FineWorkgroupDispatch {
  contracts::KernelInstanceId kernel_instance;
  contracts::WorkgroupInstanceId workgroup_instance;
  contracts::KernelId kernel;
  contracts::WorkgroupId workgroup;
  contracts::DeviceId device;
  contracts::ComputeUnitId compute_unit;
  std::uint32_t residency_slots;
  std::uint64_t dispatch_sequence;
  bool waited_for_residency;
};

class FineGpuScheduler {
 public:
  explicit FineGpuScheduler(const FineGpuSchedulerConfig& config);

  std::vector<FineWorkgroupDispatch> submit(
      const contracts::FineKernelDefinition& kernel);
  std::vector<FineWorkgroupDispatch> complete(
      contracts::WorkgroupInstanceId workgroup_instance);

  std::uint32_t resident_slots(
      contracts::DeviceId device,
      contracts::ComputeUnitId compute_unit) const;
  std::size_t active_workgroups(contracts::DeviceId device) const;
  std::size_t waiting_workgroups(contracts::DeviceId device) const;
  bool drained() const;

 private:
  struct PendingWorkgroup {
    contracts::KernelInstanceId kernel_instance;
    contracts::WorkgroupInstanceId workgroup_instance;
    contracts::KernelId kernel;
    contracts::WorkgroupId workgroup;
    contracts::DeviceId device;
    std::uint32_t residency_slots;
    bool waited_for_residency;
  };

  struct ComputeUnitState {
    std::uint32_t resident_slots;
  };

  struct Assignment {
    std::size_t device_index;
    std::size_t compute_unit_index;
    std::uint32_t residency_slots;
  };

  struct DeviceState {
    std::vector<ComputeUnitState> compute_units;
    std::deque<PendingWorkgroup> waiting;
    std::size_t next_round_robin;
  };

  std::size_t select_compute_unit(
      std::size_t device_index, std::uint32_t required_slots);
  FineWorkgroupDispatch dispatch(
      const PendingWorkgroup& pending,
      std::size_t device_index,
      std::size_t compute_unit_index);
  std::vector<FineWorkgroupDispatch> drain_waiting(std::size_t device_index);
  std::uint64_t next_random();

  FineGpuSchedulerConfig config_;
  std::vector<DeviceState> devices_;
  std::unordered_map<contracts::WorkgroupInstanceId, Assignment> active_;
  std::unordered_set<contracts::WorkgroupInstanceId> outstanding_;
  std::uint64_t random_state_;
  std::uint64_t next_kernel_instance_;
  std::uint64_t next_workgroup_instance_;
  std::uint64_t next_dispatch_sequence_;
};

}  // namespace runtime
}  // namespace simai

#endif
