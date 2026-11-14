#include "simai/runtime/finite_switch.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace simai {
namespace runtime {
namespace {

std::uint64_t checked_add(
    std::uint64_t lhs,
    std::uint64_t rhs,
    const char* message) {
  if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs) {
    throw std::overflow_error(message);
  }
  return lhs + rhs;
}

}  // namespace

FiniteSwitch::QueueState::QueueState(
    const LinkConfig& config,
    std::uint32_t endpoint)
    : egress_link(config),
      resident_departures(),
      credit_returns(),
      available_credits(config.switch_credits_per_egress),
      seen_pdus(),
      counters{
          endpoint,
          0, 0, 0, 0, 0, 0, 0, 0, 0,
          config.switch_credits_per_egress,
          config.switch_credits_per_egress,
          0, 0, 0, 0, 0, 0},
      arbitration() {}

FiniteSwitch::FiniteSwitch(
    const LinkConfig& config,
    std::uint32_t endpoint_count)
    : config_(config),
      capacity_pdus_(config.switch_queue_capacity_pdus),
      last_arrival_ns_(0),
      queues_() {
  if (config_.switch_enabled != 1 || endpoint_count == 0 ||
      capacity_pdus_ == 0) {
    throw std::invalid_argument(
        "finite switch requires enabled mode, endpoints, and queue capacity");
  }
  for (std::uint32_t endpoint = 1; endpoint <= endpoint_count; ++endpoint) {
    queues_.emplace_back(config_, endpoint);
  }
}

void FiniteSwitch::drain(
    QueueState& queue,
    contracts::SimTimeNs now_ns) {
  while (!queue.resident_departures.empty() &&
         queue.resident_departures.front() <= now_ns) {
    queue.resident_departures.pop_front();
    ++queue.counters.departed_pdus;
  }
  while (!queue.credit_returns.empty() &&
         queue.credit_returns.front() <= now_ns) {
    queue.credit_returns.pop_front();
    ++queue.available_credits;
    ++queue.counters.credits_returned;
  }
  require_credit_conservation(queue);
}

void FiniteSwitch::require_credit_conservation(
    const QueueState& queue) const {
  if (queue.available_credits + queue.credit_returns.size() !=
      queue.counters.initial_credits) {
    throw std::logic_error("switch credit conservation violated");
  }
}

SwitchForwardTiming FiniteSwitch::forward(
    const SuePdu& pdu,
    contracts::SimTimeNs arrival_at_switch_ns) {
  pdu.require_valid();
  if (arrival_at_switch_ns < last_arrival_ns_) {
    throw std::logic_error(
        "switch arrivals must be submitted in simulation-time order");
  }
  last_arrival_ns_ = arrival_at_switch_ns;
  if (pdu.destination_endpoint.value() == 0 ||
      pdu.destination_endpoint.value() > queues_.size()) {
    throw std::invalid_argument("switch destination endpoint is out of range");
  }
  QueueState& queue = queues_[pdu.destination_endpoint.value() - 1];
  if (!queue.seen_pdus.insert(pdu.id).second) {
    ++queue.counters.duplicate_pdus;
    return SwitchForwardTiming{
        arrival_at_switch_ns,
        arrival_at_switch_ns,
        arrival_at_switch_ns,
        arrival_at_switch_ns,
        arrival_at_switch_ns,
        false,
        true};
  }
  drain(queue, arrival_at_switch_ns);

  contracts::SimTimeNs admission_ns = arrival_at_switch_ns;
  if (queue.resident_departures.size() >= capacity_pdus_) {
    admission_ns = std::max(
        admission_ns, queue.resident_departures.front());
  }
  const bool credit_enabled = queue.counters.initial_credits != 0;
  const bool credit_starved =
      credit_enabled && queue.available_credits == 0;
  contracts::SimTimeNs credit_ready_ns = arrival_at_switch_ns;
  if (credit_starved) {
    if (queue.credit_returns.empty()) {
      throw std::logic_error("credit-starved queue has no pending return");
    }
    credit_ready_ns = queue.credit_returns.front();
    admission_ns = std::max(admission_ns, credit_ready_ns);
  }
  drain(queue, admission_ns);
  if (queue.resident_departures.size() >= capacity_pdus_) {
    throw std::logic_error("switch queue remained full after admission wait");
  }
  if (credit_enabled) {
    if (queue.available_credits == 0) {
      throw std::logic_error("switch admission occurred without a credit");
    }
    --queue.available_credits;
    ++queue.counters.credits_consumed;
  }
  const bool backpressured = admission_ns > arrival_at_switch_ns;
  const contracts::SimTimeNs ready_ns = checked_add(
      admission_ns,
      config_.switch_latency_ns,
      "switch processing timestamp overflow");
  const LinkTransferTiming timing = queue.egress_link.transmit(pdu, ready_ns);
  queue.resident_departures.push_back(timing.serialization_complete_ns);
  if (credit_enabled) {
    queue.credit_returns.push_back(checked_add(
        timing.serialization_complete_ns,
        config_.credit_return_latency_ns,
        "credit return timestamp overflow"));
    queue.counters.maximum_outstanding_credits = std::max<std::uint64_t>(
        queue.counters.maximum_outstanding_credits,
        queue.counters.initial_credits - queue.available_credits);
  }

  ++queue.counters.accepted_pdus;
  queue.counters.pdu_payload_bytes = checked_add(
      queue.counters.pdu_payload_bytes,
      pdu.payload_bytes,
      "switch payload counter overflow");
  queue.counters.wire_bytes = checked_add(
      queue.counters.wire_bytes,
      pdu.wire_bytes,
      "switch wire counter overflow");
  queue.counters.maximum_occupancy_pdus = std::max<std::uint64_t>(
      queue.counters.maximum_occupancy_pdus,
      queue.resident_departures.size());
  queue.counters.busy_time_ns = checked_add(
      queue.counters.busy_time_ns,
      timing.serialization_complete_ns - timing.serialization_start_ns,
      "switch busy-time counter overflow");
  if (backpressured) {
    ++queue.counters.backpressure_events;
    queue.counters.total_admission_delay_ns = checked_add(
        queue.counters.total_admission_delay_ns,
        admission_ns - arrival_at_switch_ns,
        "switch admission-delay counter overflow");
  }
  if (credit_starved) {
    ++queue.counters.credit_starvation_events;
    queue.counters.total_credit_starvation_ns = checked_add(
        queue.counters.total_credit_starvation_ns,
        credit_ready_ns - arrival_at_switch_ns,
        "credit starvation counter overflow");
  }
  queue.arbitration.push_back(pdu.id);
  require_credit_conservation(queue);

  return SwitchForwardTiming{
      arrival_at_switch_ns,
      admission_ns,
      timing.serialization_start_ns,
      timing.serialization_complete_ns,
      timing.arrival_ns,
      backpressured,
      false};
}

std::vector<contracts::SwitchQueueUtilization> FiniteSwitch::finalize(
    contracts::SimTimeNs now_ns) {
  std::vector<contracts::SwitchQueueUtilization> reports;
  for (QueueState& queue : queues_) {
    drain(queue, now_ns);
    queue.counters.final_occupancy_pdus =
        queue.resident_departures.size();
    if (!queue.credit_returns.empty()) {
      drain(queue, queue.credit_returns.back());
    }
    queue.counters.final_available_credits = queue.available_credits;
    require_credit_conservation(queue);
    reports.push_back(queue.counters);
  }
  return reports;
}

const std::vector<contracts::PduId>& FiniteSwitch::arbitration_trace(
    contracts::DeviceId destination) const {
  if (!destination.valid() || destination.value() > queues_.size()) {
    throw std::invalid_argument("switch destination endpoint is out of range");
  }
  return queues_[destination.value() - 1].arbitration;
}

}  // namespace runtime
}  // namespace simai
