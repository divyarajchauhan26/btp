#ifndef SIMAI_RUNTIME_FINE_REMOTE_HPP
#define SIMAI_RUNTIME_FINE_REMOTE_HPP

#include <cstdint>
#include <vector>

#include "simai/contracts/fine_remote.hpp"
#include "simai/runtime/fine_wavefront_engine.hpp"
#include "simai/runtime/sue_pdu.hpp"

namespace simai {
namespace runtime {

contracts::FineRemoteRequest lower_remote_request(
    contracts::MemoryRequestId request_id,
    const FineInstructionIssue& issue);

struct FineRemotePacketBatch {
  contracts::FineRemoteRequest request;
  std::vector<SuePdu> command_pdus;
  std::vector<SuePdu> response_pdus;
};

class FineRemotePacketizer {
 public:
  FineRemotePacketizer(
      const LinkConfig& config,
      contracts::BufferId semaphore_register_buffer);

  FineRemotePacketBatch packetize(
      const contracts::FineRemoteRequest& request,
      std::uint64_t& next_transaction_id,
      std::uint64_t& next_pdu_id) const;

 private:
  SuePacketizer packetizer_;
  contracts::BufferId semaphore_register_buffer_;
};

}  // namespace runtime
}  // namespace simai

#endif
