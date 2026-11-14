#ifndef SIMAI_RUNTIME_SUE_PDU_HPP
#define SIMAI_RUNTIME_SUE_PDU_HPP

#include <cstdint>
#include <vector>

#include "simai/contracts/collective.hpp"
#include "simai/contracts/sue_transaction.hpp"
#include "simai/runtime/workload.hpp"

namespace simai {
namespace runtime {

struct SuePdu {
  contracts::PduId id;
  std::uint64_t packet_sequence;
  contracts::DeviceId source_endpoint;
  contracts::DeviceId destination_endpoint;
  std::uint8_t virtual_channel;
  contracts::OrderingDomainId ordering_domain;
  std::vector<contracts::SueTransaction> transactions;
  std::uint64_t payload_bytes;
  std::uint64_t overhead_bytes;
  std::uint64_t wire_bytes;

  contracts::ValidationErrors validate() const;
  void require_valid() const;
};

class SuePacketizer {
 public:
  explicit SuePacketizer(const LinkConfig& config);

  std::vector<SuePdu> packetize_put(
      const contracts::MicroOp& operation,
      std::uint64_t& next_transaction_id,
      std::uint64_t& next_pdu_id) const;

  std::vector<SuePdu> packetize_signal(
      const contracts::MicroOp& operation,
      std::uint64_t& next_transaction_id,
      std::uint64_t& next_pdu_id) const;

  std::vector<SuePdu> packetize_get_request(
      const contracts::MicroOp& operation,
      std::uint64_t& next_transaction_id,
      std::uint64_t& next_pdu_id) const;

  std::vector<SuePdu> packetize_get_response(
      const contracts::MicroOp& operation,
      std::uint64_t& next_transaction_id,
      std::uint64_t& next_pdu_id) const;

 private:
  LinkConfig config_;
};

}  // namespace runtime
}  // namespace simai

#endif
