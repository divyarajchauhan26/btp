#include "simai/runtime/fine_local_memory.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace simai {
namespace runtime {
namespace {

bool is_local_tier(contracts::FineMemorySpace space) {
  return space == contracts::FineMemorySpace::L1 ||
      space == contracts::FineMemorySpace::L2 ||
      space == contracts::FineMemorySpace::LocalHbm;
}

contracts::SimTimeNs checked_add(
    contracts::SimTimeNs lhs,
    contracts::SimTimeNs rhs,
    const char* message) {
  if (lhs > std::numeric_limits<contracts::SimTimeNs>::max() - rhs) {
    throw std::overflow_error(message);
  }
  return lhs + rhs;
}

std::uint64_t divide_round_up(std::uint64_t value, std::uint64_t divisor) {
  return value / divisor + (value % divisor == 0 ? 0 : 1);
}

contracts::FineMemoryCompletion empty_completion() {
  return contracts::FineMemoryCompletion{};
}

}  // namespace

contracts::ValidationErrors FineLocalMemoryTierConfig::validate() const {
  contracts::ValidationErrors errors;
  if (!is_local_tier(space)) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidValue,
        "fine local-memory tier must be L1, L2, or LocalHbm");
  }
  if (base_latency_ns == 0 || bytes_per_ns == 0 || max_outstanding == 0) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidResourceRequirement,
        "fine local-memory timing and outstanding capacity must be nonzero");
  }
  return errors;
}

void FineLocalMemoryTierConfig::require_valid() const {
  contracts::require_no_errors(validate(), "FineLocalMemoryTierConfig");
}

contracts::ValidationErrors FineLocalMemorySystemConfig::validate() const {
  contracts::ValidationErrors errors;
  if (device_count == 0 || tiers.empty()) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidResourceRequirement,
        "fine local-memory system needs devices and tiers");
  }
  std::unordered_set<int> spaces;
  for (const FineLocalMemoryTierConfig& tier : tiers) {
    const contracts::ValidationErrors tier_errors = tier.validate();
    errors.insert(errors.end(), tier_errors.begin(), tier_errors.end());
    if (!spaces.insert(static_cast<int>(tier.space)).second) {
      contracts::append_error(
          errors,
          contracts::ValidationCode::DuplicateIdentity,
          "fine local-memory tier spaces must be unique");
    }
  }
  return errors;
}

void FineLocalMemorySystemConfig::require_valid() const {
  contracts::require_no_errors(validate(), "FineLocalMemorySystemConfig");
}

contracts::FineMemoryRequest lower_local_memory_request(
    contracts::MemoryRequestId request_id,
    const FineInstructionIssue& issue) {
  const contracts::FineGpuInstruction& instruction = issue.instruction;
  const bool load = instruction.kind == contracts::FineGpuInstructionKind::Load;
  const bool store = instruction.kind == contracts::FineGpuInstructionKind::Store;
  if (!load && !store) {
    throw std::invalid_argument(
        "only fine Load and Store instructions lower to local memory");
  }
  const contracts::FineMemoryReference target =
      load ? instruction.source : instruction.destination;
  if (!is_local_tier(target.space) || target.owner != issue.device) {
    throw std::invalid_argument(
        "fine instruction target is not local to its issuing device");
  }
  contracts::FineMemoryRequest request{
      request_id,
      issue.kernel_instance,
      issue.workgroup_instance,
      issue.wavefront_instance,
      instruction.id,
      issue.device,
      load ? contracts::FineMemoryRequestKind::LocalLoad
           : contracts::FineMemoryRequestKind::LocalStore,
      target,
      issue.issue_ns};
  request.require_valid();
  return request;
}

FineLocalMemorySystem::FineLocalMemorySystem(
    const FineLocalMemorySystemConfig& config)
    : config_(config), tiers_(), requests_() {
  config_.require_valid();
  const std::uint64_t state_count =
      static_cast<std::uint64_t>(config_.device_count) * config_.tiers.size();
  if (state_count > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument("fine local-memory tier count is too large");
  }
  tiers_.assign(static_cast<std::size_t>(state_count), TierState{0, {}});
}

std::size_t FineLocalMemorySystem::tier_config_index(
    contracts::FineMemorySpace space) const {
  for (std::size_t index = 0; index < config_.tiers.size(); ++index) {
    if (config_.tiers[index].space == space) return index;
  }
  throw std::invalid_argument("fine local-memory space has no configured tier");
}

std::size_t FineLocalMemorySystem::tier_state_index(
    contracts::DeviceId device, std::size_t tier_config) const {
  if (!device.valid() || device.value() > config_.device_count ||
      tier_config >= config_.tiers.size()) {
    throw std::invalid_argument("fine local-memory tier identity is out of range");
  }
  return static_cast<std::size_t>(
      (device.value() - 1) * config_.tiers.size() + tier_config);
}

FineMemorySubmitResult FineLocalMemorySystem::submit(
    const contracts::FineMemoryRequest& request) {
  request.require_valid();
  if (request.kind != contracts::FineMemoryRequestKind::LocalLoad &&
      request.kind != contracts::FineMemoryRequestKind::LocalStore) {
    throw std::invalid_argument("fine local-memory system rejects nonlocal request kinds");
  }
  if (request.target.owner != request.issuer ||
      !is_local_tier(request.target.space)) {
    throw std::invalid_argument("fine local-memory request has nonlocal routing");
  }
  if (requests_.count(request.id) != 0) {
    throw std::invalid_argument("fine memory request identity is already active");
  }
  const std::size_t config_index = tier_config_index(request.target.space);
  const std::size_t state_index =
      tier_state_index(request.issuer, config_index);
  const FineLocalMemoryTierConfig& config = config_.tiers[config_index];
  TierState& tier = tiers_[state_index];
  if (tier.requests.size() >= config.max_outstanding) {
    return FineMemorySubmitResult{
        false,
        contracts::FineStallReason::MemoryBackpressure,
        empty_completion()};
  }
  std::size_t queued = 0;
  for (contracts::MemoryRequestId active : tier.requests) {
    if (requests_.at(active).expected.service_start_ns > request.issue_ns) {
      ++queued;
    }
  }
  const contracts::SimTimeNs service_start =
      std::max(request.issue_ns, tier.next_service_ns);
  if (service_start > request.issue_ns && queued >= config.queue_capacity) {
    return FineMemorySubmitResult{
        false,
        contracts::FineStallReason::MemoryBackpressure,
        empty_completion()};
  }
  const contracts::SimTimeNs service_duration = divide_round_up(
      request.target.length_bytes, config.bytes_per_ns);
  const contracts::SimTimeNs service_end = checked_add(
      service_start, service_duration, "fine local-memory service overflow");
  const contracts::SimTimeNs completion_ns = checked_add(
      service_end, config.base_latency_ns, "fine local-memory latency overflow");
  contracts::FineMemoryCompletion completion{
      request, service_start, service_end, completion_ns};
  completion.require_valid();
  tier.next_service_ns = service_end;
  tier.requests.push_back(request.id);
  requests_.insert(
      std::make_pair(request.id, RequestState{state_index, completion}));
  return FineMemorySubmitResult{
      true, contracts::FineStallReason::None, completion};
}

contracts::FineMemoryCompletion FineLocalMemorySystem::complete(
    contracts::MemoryRequestId request,
    contracts::SimTimeNs completion_ns) {
  const auto found = requests_.find(request);
  if (found == requests_.end()) {
    throw std::invalid_argument("fine memory completion request is unknown");
  }
  if (completion_ns < found->second.expected.completion_ns) {
    throw std::invalid_argument("fine memory request completed before modeled time");
  }
  contracts::FineMemoryCompletion completion = found->second.expected;
  completion.completion_ns = completion_ns;
  TierState& tier = tiers_[found->second.tier_state];
  tier.requests.erase(
      std::remove(tier.requests.begin(), tier.requests.end(), request),
      tier.requests.end());
  requests_.erase(found);
  completion.require_valid();
  return completion;
}

std::size_t FineLocalMemorySystem::active_requests(
    contracts::DeviceId device,
    contracts::FineMemorySpace space) const {
  const std::size_t config_index = tier_config_index(space);
  return tiers_[tier_state_index(device, config_index)].requests.size();
}

}  // namespace runtime
}  // namespace simai
