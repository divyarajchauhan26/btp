#ifdef SIMAI_ENABLE_SUE_SIM

#include "simai/runtime/sue_sim_transport_backend.hpp"

#include <limits>
#include <stdexcept>

#include "ns3/simulator.h"

namespace simai {
namespace runtime {

SueSimTransportBackend::SueSimTransportBackend(
    ns3::SimaiSueBridge& bridge, std::uint32_t endpoint_count,
    std::uint32_t rail_count)
    : bridge_(bridge),
      endpoint_count_(endpoint_count),
      expected_pdu_(0),
      expected_operation_(0),
      expected_source_xpu_(0),
      expected_destination_xpu_(0),
      expected_vc_(0),
      expected_payload_bytes_(0),
      visible_(false),
      completion_mismatch_(false),
      visible_ns_(0),
      visible_rail_(0),
      rails_(rail_count, contracts::RailUtilization{0, 0, 0, 0, 0, 0, 0, 0}) {
  if (endpoint_count_ < 2 || rail_count == 0) {
    throw std::invalid_argument("SUE-Sim backend requires endpoints and rails");
  }
  for (std::size_t index = 0; index < rails_.size(); ++index) {
    rails_[index].rail_index = static_cast<std::uint32_t>(index);
  }
  bridge_.SetCompletionCallback(
      ns3::MakeCallback(&SueSimTransportBackend::HandleVisible, this));
}

FabricIngressTiming SueSimTransportBackend::transmit_ingress(
    const SuePdu& pdu, contracts::SimTimeNs issue_ns) {
  pdu.require_valid();
  if (pdu.source_endpoint.value() > endpoint_count_ ||
      pdu.destination_endpoint.value() > endpoint_count_ ||
      issue_ns < static_cast<contracts::SimTimeNs>(
                     ns3::Simulator::Now().GetNanoSeconds())) {
    throw std::invalid_argument("SUE-Sim PDU endpoint or issue time is invalid");
  }
  expected_pdu_ = pdu.id.value();
  expected_operation_ = pdu.transactions.front().parent_operation.value();
  expected_source_xpu_ = static_cast<std::uint32_t>(pdu.source_endpoint.value() - 1);
  expected_destination_xpu_ =
      static_cast<std::uint32_t>(pdu.destination_endpoint.value() - 1);
  expected_vc_ = pdu.virtual_channel;
  expected_payload_bytes_ = static_cast<std::uint32_t>(pdu.payload_bytes);
  visible_ = false;
  completion_mismatch_ = false;
  const ns3::Time delay = ns3::NanoSeconds(
      issue_ns - static_cast<contracts::SimTimeNs>(
                     ns3::Simulator::Now().GetNanoSeconds()));
  ns3::Simulator::Schedule(
      delay, &ns3::SimaiSueBridge::InjectPdu, &bridge_,
      pdu.transactions.front().parent_operation.value(), pdu.id.value(),
      static_cast<std::uint32_t>(pdu.source_endpoint.value() - 1),
      static_cast<std::uint32_t>(pdu.destination_endpoint.value() - 1),
      pdu.virtual_channel, static_cast<std::uint32_t>(pdu.payload_bytes));
  ns3::Simulator::Run();
  if (completion_mismatch_ || visible_rail_ >= rails_.size()) {
    throw std::runtime_error("SUE-Sim returned mismatched completion metadata");
  }
  if (!visible_) {
    throw std::runtime_error("SUE-Sim stopped before destination visibility");
  }
  contracts::RailUtilization& rail = rails_[visible_rail_];
  ++rail.pdus;
  rail.pdu_payload_bytes += pdu.payload_bytes;
  rail.wire_bytes += pdu.wire_bytes;
  ++rail.transmission_attempts;
  rail.transmitted_wire_bytes += pdu.wire_bytes;
  rail.busy_time_ns += visible_ns_ - issue_ns;
  return FabricIngressTiming{
      contracts::RailId(visible_rail_ + 1), issue_ns, visible_ns_, 1, 0,
      pdu.wire_bytes};
}

SwitchForwardTiming SueSimTransportBackend::forward_from_switch(
    const SuePdu&, contracts::SimTimeNs) {
  throw std::logic_error("SUE-Sim owns switch forwarding internally");
}

bool SueSimTransportBackend::has_switch() const { return false; }

const std::vector<contracts::RailUtilization>&
SueSimTransportBackend::rail_counters() const {
  return rails_;
}

std::vector<contracts::SwitchQueueUtilization>
SueSimTransportBackend::finalize_switch(contracts::SimTimeNs) {
  return {};
}

void SueSimTransportBackend::HandleVisible(
    std::uint64_t operation_id, std::uint64_t pdu_id,
    std::uint32_t source_xpu, std::uint32_t destination_xpu,
    std::uint8_t vc_id, std::uint32_t payload_bytes,
    std::uint32_t port_id) {
  if (pdu_id == expected_pdu_) {
    completion_mismatch_ =
        operation_id != expected_operation_ ||
        source_xpu != expected_source_xpu_ ||
        destination_xpu != expected_destination_xpu_ ||
        vc_id != expected_vc_ || payload_bytes != expected_payload_bytes_;
    visible_ = !completion_mismatch_;
    visible_rail_ = port_id;
    visible_ns_ = static_cast<contracts::SimTimeNs>(
        ns3::Simulator::Now().GetNanoSeconds());
    ns3::Simulator::Stop();
  }
}

SueSimTransportBackendFactory::SueSimTransportBackendFactory(
    ns3::SimaiSueBridge& bridge) : bridge_(bridge) {}

std::unique_ptr<SueTransportBackend> SueSimTransportBackendFactory::create(
    const LinkConfig& config, std::uint32_t endpoint_count) const {
  return std::unique_ptr<SueTransportBackend>(
      new SueSimTransportBackend(bridge_, endpoint_count, config.rail_count));
}

}  // namespace runtime
}  // namespace simai

#endif
