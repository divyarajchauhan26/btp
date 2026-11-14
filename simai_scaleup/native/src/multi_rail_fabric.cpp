#include "simai/runtime/multi_rail_fabric.hpp"

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

std::uint64_t mix(std::uint64_t value) {
  value += UINT64_C(0x9e3779b97f4a7c15);
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

}  // namespace

MultiRailFabric::MultiRailFabric(
    const LinkConfig& config,
    std::uint32_t endpoint_count)
    : config_(config), rails_(), rail_counters_(), switch_() {
  if (config.rail_count == 0 || config.rail_count > 4) {
    throw std::invalid_argument("rail count must be in the range 1..4");
  }
  for (std::uint32_t rail = 0; rail < config.rail_count; ++rail) {
    rails_.emplace_back(config);
    rail_counters_.push_back(
        contracts::RailUtilization{rail, 0, 0, 0, 0, 0, 0, 0});
  }
  if (config.switch_enabled == 1) {
    switch_.reset(new FiniteSwitch(config, endpoint_count));
  }
}

std::size_t MultiRailFabric::select_rail(const SuePdu& pdu) const {
  const std::uint64_t count = rails_.size();
  const std::uint64_t flow_hash =
      mix(pdu.source_endpoint.value()) ^
      mix(pdu.destination_endpoint.value() + UINT64_C(0x517cc1b727220a95)) ^
      mix(pdu.ordering_domain.value() + UINT64_C(0x6eed0e9da4d94a4f));
  return static_cast<std::size_t>(flow_hash % count);
}

FabricIngressTiming MultiRailFabric::transmit_ingress(
    const SuePdu& pdu,
    contracts::SimTimeNs issue_ns) {
  pdu.require_valid();
  const std::size_t rail_index = select_rail(pdu);
  contracts::SimTimeNs attempt_issue_ns = issue_ns;
  LinkTransferTiming ingress{};
  std::uint64_t attempts = 0;
  std::uint64_t retries = 0;
  std::uint64_t transmitted_wire_bytes = 0;
  contracts::SimTimeNs attempt_busy_time_ns = 0;
  while (true) {
    ingress = rails_[rail_index].transmit(pdu, attempt_issue_ns);
    ++attempts;
    transmitted_wire_bytes = checked_add(
        transmitted_wire_bytes,
        pdu.wire_bytes,
        "transmitted wire byte counter overflow");
    attempt_busy_time_ns = checked_add(
        attempt_busy_time_ns,
        ingress.serialization_complete_ns - ingress.serialization_start_ns,
        "rail attempt busy-time counter overflow");
    const bool injected_error =
        pdu.id.value() == config_.error_injection_pdu_id &&
        attempts <= config_.error_injection_attempts;
    if (!injected_error) {
      break;
    }
    if (retries >= config_.maximum_retries) {
      throw std::runtime_error("maximum PDU retries exhausted");
    }
    ++retries;
    attempt_issue_ns = checked_add(
        ingress.arrival_ns,
        config_.replay_latency_ns,
        "PDU replay timestamp overflow");
  }
  contracts::RailUtilization& counters = rail_counters_[rail_index];
  ++counters.pdus;
  counters.pdu_payload_bytes = checked_add(
      counters.pdu_payload_bytes,
      pdu.payload_bytes,
      "rail PDU payload counter overflow");
  counters.wire_bytes = checked_add(
      counters.wire_bytes,
      pdu.wire_bytes,
      "rail wire byte counter overflow");
  counters.busy_time_ns = checked_add(
      counters.busy_time_ns,
      attempt_busy_time_ns,
      "rail busy-time counter overflow");
  counters.transmission_attempts = checked_add(
      counters.transmission_attempts,
      attempts,
      "rail attempt counter overflow");
  counters.retries = checked_add(
      counters.retries, retries, "rail retry counter overflow");
  counters.transmitted_wire_bytes = checked_add(
      counters.transmitted_wire_bytes,
      transmitted_wire_bytes,
      "rail transmitted wire counter overflow");
  return FabricIngressTiming{
      contracts::RailId(rail_index + 1),
      ingress.serialization_complete_ns,
      ingress.arrival_ns,
      attempts,
      retries,
      transmitted_wire_bytes};
}

SwitchForwardTiming MultiRailFabric::forward_from_switch(
    const SuePdu& pdu,
    contracts::SimTimeNs ingress_arrival_ns) {
  if (!switch_) {
    throw std::logic_error("switch forwarding requested while switch is disabled");
  }
  return switch_->forward(pdu, ingress_arrival_ns);
}

std::vector<contracts::SwitchQueueUtilization>
MultiRailFabric::finalize_switch(contracts::SimTimeNs now_ns) {
  if (!switch_) {
    return {};
  }
  return switch_->finalize(now_ns);
}

}  // namespace runtime
}  // namespace simai
