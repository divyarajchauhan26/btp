#ifndef SIMAI_RUNTIME_SUE_SIM_TRANSPORT_BACKEND_HPP
#define SIMAI_RUNTIME_SUE_SIM_TRANSPORT_BACKEND_HPP

#include <memory>
#include <deque>
#include <map>

#include "ns3/simai-sue-bridge.h"
#include "simai/runtime/transport_backend.hpp"

namespace simai {
namespace runtime {

class SueSimTransportBackend final : public SueTransportBackend {
 public:
  SueSimTransportBackend(
      ns3::SimaiSueBridge& bridge, std::uint32_t endpoint_count,
      std::uint32_t rail_count);

  FabricIngressTiming transmit_ingress(
      const SuePdu& pdu, contracts::SimTimeNs issue_ns) override;
  SwitchForwardTiming forward_from_switch(
      const SuePdu& pdu, contracts::SimTimeNs ingress_arrival_ns) override;
  bool has_switch() const override;
  const std::vector<contracts::RailUtilization>& rail_counters() const override;
  std::vector<contracts::SwitchQueueUtilization> finalize_switch(
      contracts::SimTimeNs now_ns) override;
  bool supports_asynchronous_submissions() const override;
  void submit_async(
      const SuePdu& pdu, contracts::SimTimeNs issue_ns) override;
  bool has_pending_async() const override;
  bool advance_to_next_completion(
      contracts::SimTimeNs deadline_ns,
      SueTransportCompletion& completion) override;

 private:
  struct PendingPdu {
    std::uint64_t operation;
    std::uint32_t source_xpu;
    std::uint32_t destination_xpu;
    std::uint8_t virtual_channel;
    std::uint32_t payload_bytes;
    std::uint64_t wire_bytes;
    contracts::SimTimeNs issue_ns;
  };

  void HandleDeadline();
  void HandleVisible(
      std::uint64_t operation_id, std::uint64_t pdu_id,
      std::uint32_t source_xpu, std::uint32_t destination_xpu,
      std::uint8_t vc_id, std::uint32_t payload_bytes,
      std::uint32_t port_id);

  ns3::SimaiSueBridge& bridge_;
  std::uint32_t endpoint_count_;
  bool completion_mismatch_;
  bool deadline_reached_;
  std::map<contracts::PduId, PendingPdu> pending_;
  std::deque<SueTransportCompletion> completions_;
  std::vector<contracts::RailUtilization> rails_;
};

class SueSimTransportBackendFactory final
    : public SueTransportBackendFactory {
 public:
  explicit SueSimTransportBackendFactory(ns3::SimaiSueBridge& bridge);
  std::unique_ptr<SueTransportBackend> create(
      const LinkConfig& config,
      std::uint32_t endpoint_count) const override;

 private:
  ns3::SimaiSueBridge& bridge_;
};

}  // namespace runtime
}  // namespace simai

#endif
