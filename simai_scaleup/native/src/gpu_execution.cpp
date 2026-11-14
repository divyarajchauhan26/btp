#include "simai/runtime/gpu_execution.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace simai {
namespace runtime {
namespace {

contracts::SimTimeNs checked_add(
    contracts::SimTimeNs lhs,
    contracts::SimTimeNs rhs,
    const char* message) {
  if (lhs > std::numeric_limits<contracts::SimTimeNs>::max() - rhs) {
    throw std::overflow_error(message);
  }
  return lhs + rhs;
}

contracts::SimTimeNs checked_multiply(
    std::uint64_t lhs,
    std::uint64_t rhs,
    const char* message) {
  if (rhs != 0 && lhs > std::numeric_limits<std::uint64_t>::max() / rhs) {
    throw std::overflow_error(message);
  }
  return lhs * rhs;
}

std::uint64_t divide_round_up(std::uint64_t value, std::uint64_t divisor) {
  return value / divisor + (value % divisor == 0 ? 0 : 1);
}

}  // namespace

GpuExecutionModel::GpuExecutionModel(
    const GpuConfig& config,
    std::uint32_t endpoint_count)
    : config_(config), devices_(), active_(), outstanding_() {
  if (endpoint_count == 0 || config_.enabled > 1) {
    throw std::invalid_argument("invalid GPU execution model configuration");
  }
  if (!enabled()) {
    return;
  }
  if (config_.compute_units_per_device == 0 ||
      config_.workgroup_slots_per_compute_unit == 0 ||
      config_.wavefronts_per_workgroup == 0 ||
      config_.wavefront_issue_width == 0 ||
      config_.wavefront_issue_latency_ns == 0 ||
      config_.reduction_bytes_per_ns == 0) {
    throw std::invalid_argument("enabled GPU resources must be nonzero");
  }
  const std::uint64_t slots =
      static_cast<std::uint64_t>(config_.compute_units_per_device) *
      config_.workgroup_slots_per_compute_unit;
  if (slots > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument("GPU workgroup slot count exceeds host range");
  }
  for (std::uint32_t endpoint = 1; endpoint <= endpoint_count; ++endpoint) {
    DeviceState state;
    state.slots.assign(
        static_cast<std::size_t>(slots), Slot{false, 0, 0});
    state.counters = contracts::GpuUtilization{
        endpoint,
        config_.compute_units_per_device,
        config_.workgroup_slots_per_compute_unit,
        0,
        0,
        0,
        0,
        0,
        0,
        0};
    state.resident_workgroups = 0;
    devices_.push_back(state);
  }
}

GpuKernel GpuExecutionModel::lower(
    const contracts::MicroOp& operation) const {
  operation.require_valid();
  const std::uint32_t wavefront_count =
      enabled() ? config_.wavefronts_per_workgroup : 1;
  std::vector<GpuWavefront> wavefronts;
  for (std::uint32_t index = 0; index < wavefront_count; ++index) {
    const std::uint64_t id =
        (operation.id.value() - 1) * wavefront_count + index + 1;
    wavefronts.push_back(GpuWavefront{contracts::WavefrontId(id)});
  }
  return GpuKernel{
      contracts::KernelId(operation.id.value()),
      operation.id,
      {GpuWorkgroup{
          contracts::WorkgroupId(operation.id.value()),
          operation.issuer,
          operation.thread_block,
          wavefronts}}};
}

std::size_t GpuExecutionModel::free_slot(std::size_t device_index) const {
  const std::vector<Slot>& slots = devices_.at(device_index).slots;
  std::size_t selected = slots.size();
  for (std::size_t index = 0; index < slots.size(); ++index) {
    if (!slots[index].occupied &&
        (selected == slots.size() ||
         slots[index].available_ns < slots[selected].available_ns)) {
      selected = index;
    }
  }
  return selected;
}

GpuDispatch GpuExecutionModel::dispatch(
    const PendingWorkgroup& pending,
    std::size_t device_index,
    std::size_t slot_index,
    contracts::SimTimeNs dispatch_ns) {
  DeviceState& device = devices_.at(device_index);
  Slot& slot = device.slots.at(slot_index);
  if (slot.occupied || active_.count(pending.operation_index) != 0) {
    throw std::logic_error("GPU workgroup dispatched into occupied state");
  }
  slot.occupied = true;
  slot.operation_index = pending.operation_index;
  active_.insert(std::make_pair(
      pending.operation_index, Assignment{device_index, slot_index}));
  ++device.resident_workgroups;
  ++device.counters.workgroups_dispatched;
  device.counters.wavefronts_issued += config_.wavefronts_per_workgroup;
  device.counters.maximum_resident_workgroups = std::max(
      device.counters.maximum_resident_workgroups,
      device.resident_workgroups);

  const std::uint64_t issue_rounds = divide_round_up(
      config_.wavefronts_per_workgroup, config_.wavefront_issue_width);
  const contracts::SimTimeNs issue_ns = checked_multiply(
      issue_rounds,
      config_.wavefront_issue_latency_ns,
      "GPU wavefront issue duration overflow");
  device.counters.wavefront_issue_busy_ns = checked_add(
      device.counters.wavefront_issue_busy_ns,
      issue_ns,
      "GPU issue busy counter overflow");
  contracts::SimTimeNs reduction_ns = 0;
  if (pending.kind == contracts::MicroOpKind::Reduce) {
    reduction_ns = divide_round_up(
        pending.bytes, config_.reduction_bytes_per_ns);
    device.counters.reduction_busy_ns = checked_add(
        device.counters.reduction_busy_ns,
        reduction_ns,
        "GPU reduction busy counter overflow");
  }
  const bool stalled = dispatch_ns > pending.ready_ns;
  const contracts::SimTimeNs stall_ns = dispatch_ns - pending.ready_ns;
  if (stalled) {
    ++device.counters.resource_stall_events;
    device.counters.total_resource_stall_ns = checked_add(
        device.counters.total_resource_stall_ns,
        stall_ns,
        "GPU resource stall counter overflow");
  }
  const contracts::SimTimeNs instruction_ready = checked_add(
      dispatch_ns, issue_ns, "GPU instruction-ready timestamp overflow");
  const contracts::SimTimeNs local_complete = checked_add(
      instruction_ready,
      reduction_ns,
      "GPU local execution timestamp overflow");
  return GpuDispatch{
      pending.operation_index,
      pending.device,
      static_cast<std::uint32_t>(
          slot_index / config_.workgroup_slots_per_compute_unit),
      static_cast<std::uint32_t>(
          slot_index % config_.workgroup_slots_per_compute_unit),
      dispatch_ns,
      instruction_ready,
      local_complete,
      stalled,
      stall_ns};
}

std::vector<GpuDispatch> GpuExecutionModel::admit(
    std::size_t operation_index,
    const contracts::MicroOp& operation,
    contracts::SimTimeNs ready_ns) {
  operation.require_valid();
  if (!enabled()) {
    return {GpuDispatch{
        operation_index,
        operation.issuer,
        0,
        0,
        ready_ns,
        ready_ns,
        ready_ns,
        false,
        0}};
  }
  if (operation.issuer.value() == 0 ||
      operation.issuer.value() > devices_.size()) {
    throw std::invalid_argument("GPU operation issuer is outside endpoint set");
  }
  if (!outstanding_.insert(operation_index).second) {
    throw std::logic_error("GPU operation was admitted more than once");
  }
  const GpuKernel kernel = lower(operation);
  if (kernel.workgroups.size() != 1) {
    throw std::logic_error("CP-10 requires one workgroup per micro-operation");
  }
  const std::uint64_t bytes =
      operation.has_source ? operation.source.length_bytes : 0;
  const PendingWorkgroup pending{
      operation_index, operation.issuer, operation.kind, bytes, ready_ns};
  const std::size_t device_index = operation.issuer.value() - 1;
  const std::size_t slot_index = free_slot(device_index);
  if (slot_index == devices_[device_index].slots.size()) {
    devices_[device_index].waiting.push_back(pending);
    return {};
  }
  const contracts::SimTimeNs dispatch_ns = std::max(
      ready_ns, devices_[device_index].slots[slot_index].available_ns);
  return {dispatch(pending, device_index, slot_index, dispatch_ns)};
}

std::vector<GpuDispatch> GpuExecutionModel::complete(
    std::size_t operation_index,
    contracts::SimTimeNs complete_ns) {
  if (!enabled()) {
    return {};
  }
  const auto found = active_.find(operation_index);
  if (found == active_.end()) {
    throw std::logic_error("completed GPU workgroup is not active");
  }
  const Assignment assignment = found->second;
  active_.erase(found);
  if (outstanding_.erase(operation_index) != 1) {
    throw std::logic_error("GPU outstanding operation state is inconsistent");
  }
  DeviceState& device = devices_.at(assignment.device_index);
  Slot& slot = device.slots.at(assignment.slot_index);
  if (!slot.occupied || slot.operation_index != operation_index ||
      device.resident_workgroups == 0) {
    throw std::logic_error("GPU workgroup completion state is inconsistent");
  }
  slot = Slot{false, 0, complete_ns};
  --device.resident_workgroups;
  if (device.waiting.empty()) {
    return {};
  }
  const PendingWorkgroup pending = device.waiting.front();
  device.waiting.pop_front();
  return {dispatch(
      pending, assignment.device_index, assignment.slot_index, complete_ns)};
}

std::vector<contracts::GpuUtilization>
GpuExecutionModel::utilization() const {
  if (!active_.empty() || !outstanding_.empty()) {
    throw std::logic_error("GPU execution model still has active workgroups");
  }
  std::vector<contracts::GpuUtilization> result;
  for (const DeviceState& device : devices_) {
    if (device.resident_workgroups != 0 || !device.waiting.empty()) {
      throw std::logic_error("GPU execution model did not drain");
    }
    result.push_back(device.counters);
  }
  return result;
}

}  // namespace runtime
}  // namespace simai
