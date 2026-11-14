#include "simai/runtime/fine_gpu_scheduler.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace simai {
namespace runtime {
namespace {

const std::size_t kNoComputeUnit = std::numeric_limits<std::size_t>::max();

}  // namespace

contracts::ValidationErrors FineGpuSchedulerConfig::validate() const {
  contracts::ValidationErrors errors;
  if (device_count == 0 || compute_units_per_device == 0 ||
      workgroup_slots_per_compute_unit == 0) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidResourceRequirement,
        "fine GPU scheduler counts must be nonzero");
  }
  if (policy == contracts::FineSchedulingPolicy::SeededRandom && seed == 0) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "seeded-random fine scheduling requires a nonzero seed");
  }
  return errors;
}

void FineGpuSchedulerConfig::require_valid() const {
  contracts::require_no_errors(validate(), "FineGpuSchedulerConfig");
}

FineGpuScheduler::FineGpuScheduler(const FineGpuSchedulerConfig& config)
    : config_(config),
      devices_(),
      active_(),
      outstanding_(),
      random_state_(config.seed),
      next_kernel_instance_(1),
      next_workgroup_instance_(1),
      next_dispatch_sequence_(1) {
  config_.require_valid();
  devices_.reserve(config_.device_count);
  for (std::uint32_t device = 0; device < config_.device_count; ++device) {
    DeviceState state;
    state.compute_units.assign(
        config_.compute_units_per_device, ComputeUnitState{0});
    state.next_round_robin = 0;
    devices_.push_back(state);
  }
}

std::uint64_t FineGpuScheduler::next_random() {
  random_state_ ^= random_state_ >> 12;
  random_state_ ^= random_state_ << 25;
  random_state_ ^= random_state_ >> 27;
  return random_state_ * 2685821657736338717ULL;
}

std::size_t FineGpuScheduler::select_compute_unit(
    std::size_t device_index, std::uint32_t required_slots) {
  DeviceState& device = devices_.at(device_index);
  std::vector<std::size_t> eligible;
  for (std::size_t index = 0; index < device.compute_units.size(); ++index) {
    const std::uint32_t occupied = device.compute_units[index].resident_slots;
    if (required_slots <=
        config_.workgroup_slots_per_compute_unit - occupied) {
      eligible.push_back(index);
    }
  }
  if (eligible.empty()) return kNoComputeUnit;

  if (config_.policy == contracts::FineSchedulingPolicy::RoundRobin) {
    for (std::size_t offset = 0; offset < device.compute_units.size(); ++offset) {
      const std::size_t candidate =
          (device.next_round_robin + offset) % device.compute_units.size();
      if (std::find(eligible.begin(), eligible.end(), candidate) !=
          eligible.end()) {
        device.next_round_robin =
            (candidate + 1) % device.compute_units.size();
        return candidate;
      }
    }
  }
  if (config_.policy == contracts::FineSchedulingPolicy::LeastOccupied) {
    return *std::min_element(
        eligible.begin(), eligible.end(),
        [&device](std::size_t lhs, std::size_t rhs) {
          const std::uint32_t lhs_slots =
              device.compute_units[lhs].resident_slots;
          const std::uint32_t rhs_slots =
              device.compute_units[rhs].resident_slots;
          return lhs_slots == rhs_slots ? lhs < rhs : lhs_slots < rhs_slots;
        });
  }
  return eligible[next_random() % eligible.size()];
}

FineWorkgroupDispatch FineGpuScheduler::dispatch(
    const PendingWorkgroup& pending,
    std::size_t device_index,
    std::size_t compute_unit_index) {
  if (compute_unit_index == kNoComputeUnit ||
      active_.count(pending.workgroup_instance) != 0) {
    throw std::logic_error("invalid fine workgroup dispatch state");
  }
  ComputeUnitState& compute_unit =
      devices_.at(device_index).compute_units.at(compute_unit_index);
  if (pending.residency_slots >
      config_.workgroup_slots_per_compute_unit - compute_unit.resident_slots) {
    throw std::logic_error("fine workgroup exceeds available CU residency");
  }
  compute_unit.resident_slots += pending.residency_slots;
  active_.insert(std::make_pair(
      pending.workgroup_instance,
      Assignment{device_index, compute_unit_index, pending.residency_slots}));
  return FineWorkgroupDispatch{
      pending.kernel_instance,
      pending.workgroup_instance,
      pending.kernel,
      pending.workgroup,
      pending.device,
      contracts::ComputeUnitId(compute_unit_index + 1),
      pending.residency_slots,
      next_dispatch_sequence_++,
      pending.waited_for_residency};
}

std::vector<FineWorkgroupDispatch> FineGpuScheduler::submit(
    const contracts::FineKernelDefinition& kernel) {
  kernel.require_valid();
  if (kernel.device.value() == 0 ||
      kernel.device.value() > config_.device_count) {
    throw std::invalid_argument("fine kernel device is outside scheduler domain");
  }
  for (const contracts::FineWorkgroupDefinition& workgroup : kernel.workgroups) {
    if (workgroup.resources.residency_slots >
        config_.workgroup_slots_per_compute_unit) {
      throw std::invalid_argument(
          "fine workgroup can never fit on a configured compute unit");
    }
  }
  const std::size_t device_index = kernel.device.value() - 1;
  const contracts::KernelInstanceId kernel_instance(next_kernel_instance_++);
  std::vector<FineWorkgroupDispatch> dispatched;
  for (const contracts::FineWorkgroupDefinition& workgroup : kernel.workgroups) {
    const contracts::WorkgroupInstanceId workgroup_instance(
        next_workgroup_instance_++);
    if (!outstanding_.insert(workgroup_instance).second) {
      throw std::logic_error("fine workgroup instance identity was reused");
    }
    PendingWorkgroup pending{
        kernel_instance,
        workgroup_instance,
        kernel.id,
        workgroup.id,
        kernel.device,
        workgroup.resources.residency_slots,
        false};
    const std::size_t compute_unit =
        select_compute_unit(device_index, pending.residency_slots);
    if (compute_unit == kNoComputeUnit) {
      pending.waited_for_residency = true;
      devices_[device_index].waiting.push_back(pending);
    } else {
      dispatched.push_back(dispatch(pending, device_index, compute_unit));
    }
  }
  return dispatched;
}

std::vector<FineWorkgroupDispatch> FineGpuScheduler::drain_waiting(
    std::size_t device_index) {
  DeviceState& device = devices_.at(device_index);
  std::vector<FineWorkgroupDispatch> dispatched;
  while (!device.waiting.empty()) {
    const PendingWorkgroup& pending = device.waiting.front();
    const std::size_t compute_unit =
        select_compute_unit(device_index, pending.residency_slots);
    if (compute_unit == kNoComputeUnit) break;
    const PendingWorkgroup selected = pending;
    device.waiting.pop_front();
    dispatched.push_back(dispatch(selected, device_index, compute_unit));
  }
  return dispatched;
}

std::vector<FineWorkgroupDispatch> FineGpuScheduler::complete(
    contracts::WorkgroupInstanceId workgroup_instance) {
  if (!workgroup_instance.valid()) {
    throw std::invalid_argument("fine workgroup instance identity is invalid");
  }
  const auto found = active_.find(workgroup_instance);
  if (found == active_.end()) {
    throw std::invalid_argument("fine workgroup instance is not active");
  }
  const Assignment assignment = found->second;
  active_.erase(found);
  if (outstanding_.erase(workgroup_instance) != 1) {
    throw std::logic_error("fine scheduler outstanding state is inconsistent");
  }
  ComputeUnitState& compute_unit =
      devices_[assignment.device_index].compute_units[assignment.compute_unit_index];
  if (compute_unit.resident_slots < assignment.residency_slots) {
    throw std::logic_error("fine scheduler CU residency underflow");
  }
  compute_unit.resident_slots -= assignment.residency_slots;
  return drain_waiting(assignment.device_index);
}

std::uint32_t FineGpuScheduler::resident_slots(
    contracts::DeviceId device,
    contracts::ComputeUnitId compute_unit) const {
  if (!device.valid() || device.value() > devices_.size() ||
      !compute_unit.valid() ||
      compute_unit.value() > config_.compute_units_per_device) {
    throw std::invalid_argument("fine scheduler residency query is out of range");
  }
  return devices_[device.value() - 1]
      .compute_units[compute_unit.value() - 1]
      .resident_slots;
}

std::size_t FineGpuScheduler::active_workgroups(
    contracts::DeviceId device) const {
  if (!device.valid() || device.value() > devices_.size()) {
    throw std::invalid_argument("fine scheduler device query is out of range");
  }
  const std::size_t device_index = device.value() - 1;
  std::size_t count = 0;
  for (const auto& entry : active_) {
    if (entry.second.device_index == device_index) ++count;
  }
  return count;
}

std::size_t FineGpuScheduler::waiting_workgroups(
    contracts::DeviceId device) const {
  if (!device.valid() || device.value() > devices_.size()) {
    throw std::invalid_argument("fine scheduler device query is out of range");
  }
  return devices_[device.value() - 1].waiting.size();
}

bool FineGpuScheduler::drained() const {
  if (!active_.empty() || !outstanding_.empty()) return false;
  for (const DeviceState& device : devices_) {
    if (!device.waiting.empty()) return false;
    for (const ComputeUnitState& compute_unit : device.compute_units) {
      if (compute_unit.resident_slots != 0) return false;
    }
  }
  return true;
}

}  // namespace runtime
}  // namespace simai
