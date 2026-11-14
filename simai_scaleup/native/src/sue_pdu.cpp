#include "simai/runtime/sue_pdu.hpp"

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

contracts::BufferSlice sub_slice(
    const contracts::BufferSlice& source,
    std::uint64_t relative_offset,
    std::uint64_t length) {
  return contracts::BufferSlice{
      source.owner,
      source.buffer,
      checked_add(
          source.offset_bytes,
          relative_offset,
          "segmented buffer offset overflow"),
      length,
      source.region};
}

}  // namespace

contracts::ValidationErrors SuePdu::validate() const {
  contracts::ValidationErrors errors;
  if (!id.valid() || packet_sequence == 0 || !source_endpoint.valid() ||
      !destination_endpoint.valid() || !ordering_domain.valid()) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidIdentity,
        "PDU identities and endpoints must be nonzero");
  }
  if (source_endpoint == destination_endpoint || virtual_channel > 3 ||
      transactions.empty() || payload_bytes == 0 ||
      payload_bytes > 4096 ||
      wire_bytes < payload_bytes ||
      wire_bytes - payload_bytes != overhead_bytes) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidTransaction,
        "PDU endpoints, channel, contents, and byte counts must be valid");
  }

  std::uint64_t transaction_bytes = 0;
  for (const contracts::SueTransaction& transaction : transactions) {
    const contracts::ValidationErrors transaction_errors =
        transaction.validate();
    errors.insert(
        errors.end(), transaction_errors.begin(), transaction_errors.end());
    const bool supported =
        (transaction.role == contracts::SueMessageRole::Command &&
         (transaction.service == contracts::SueService::Put ||
          transaction.service == contracts::SueService::Atomic ||
          transaction.service == contracts::SueService::Get)) ||
        (transaction.role == contracts::SueMessageRole::Response &&
         transaction.service == contracts::SueService::Get);
    if (!supported ||
        transaction.source_endpoint != source_endpoint ||
        transaction.destination_endpoint != destination_endpoint ||
        transaction.virtual_channel != virtual_channel ||
        transaction.ordering_domain != ordering_domain) {
      contracts::append_error(
          errors,
          contracts::ValidationCode::InvalidTransaction,
          "PDU transactions must be compatible PUT, GET, or SIGNAL messages");
    }
    if (transaction_bytes >
        std::numeric_limits<std::uint64_t>::max() -
            transaction.logical_bytes) {
      contracts::append_error(
          errors,
          contracts::ValidationCode::InvalidTransaction,
          "PDU transaction byte sum overflows");
    } else {
      transaction_bytes += transaction.logical_bytes;
    }
  }
  if (transaction_bytes != payload_bytes) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidTransaction,
        "PDU payload must equal its transaction byte sum");
  }
  return errors;
}

void SuePdu::require_valid() const {
  contracts::require_no_errors(validate(), "SuePdu");
}

SuePacketizer::SuePacketizer(const LinkConfig& config) : config_(config) {
  if (config_.transaction_payload_bytes == 0 ||
      config_.transaction_payload_bytes > 256 ||
      config_.pdu_payload_bytes < config_.transaction_payload_bytes ||
      config_.pdu_payload_bytes > 4096) {
    throw std::invalid_argument(
        "SUE packetizer requires transaction payload in 1..256 bytes and PDU payload in transaction..4096 bytes");
  }
}

std::vector<SuePdu> SuePacketizer::packetize_put(
    const contracts::MicroOp& operation,
    std::uint64_t& next_transaction_id,
    std::uint64_t& next_pdu_id) const {
  operation.require_valid();
  if (operation.kind != contracts::MicroOpKind::Put) {
    throw std::invalid_argument("SUE packetizer requires a PUT operation");
  }

  std::vector<SuePdu> pdus;
  std::uint64_t processed = 0;
  while (processed < operation.source.length_bytes) {
    if (next_pdu_id == 0 ||
        next_pdu_id == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("PDU identity exhausted");
    }
    std::vector<contracts::SueTransaction> transactions;
    std::uint64_t pdu_payload = 0;
    while (processed < operation.source.length_bytes &&
           pdu_payload < config_.pdu_payload_bytes) {
      if (next_transaction_id == 0 ||
          next_transaction_id == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("transaction identity exhausted");
      }
      const std::uint64_t remaining =
          operation.source.length_bytes - processed;
      const std::uint64_t pdu_remaining =
          config_.pdu_payload_bytes - pdu_payload;
      const std::uint64_t chunk = std::min(
          remaining,
          std::min(config_.transaction_payload_bytes, pdu_remaining));
      transactions.push_back(contracts::SueTransaction::put_command(
          contracts::TransactionId(next_transaction_id++),
          operation.id,
          sub_slice(operation.source, processed, chunk),
          sub_slice(operation.destination, processed, chunk),
          0,
          operation.ordering_domain));
      processed += chunk;
      pdu_payload += chunk;
    }

    const std::uint64_t wire_bytes = checked_add(
        pdu_payload,
        config_.pdu_overhead_bytes,
        "PDU wire byte count overflow");
    const contracts::PduId pdu_id(next_pdu_id++);
    SuePdu pdu{
        pdu_id,
        pdu_id.value(),
        operation.source.owner,
        operation.destination.owner,
        0,
        operation.ordering_domain,
        transactions,
        pdu_payload,
        config_.pdu_overhead_bytes,
        wire_bytes};
    pdu.require_valid();
    pdus.push_back(pdu);
  }
  return pdus;
}

std::vector<SuePdu> SuePacketizer::packetize_signal(
    const contracts::MicroOp& operation,
    std::uint64_t& next_transaction_id,
    std::uint64_t& next_pdu_id) const {
  operation.require_valid();
  if (operation.kind != contracts::MicroOpKind::Signal ||
      next_transaction_id == 0 ||
      next_transaction_id == std::numeric_limits<std::uint64_t>::max() ||
      next_pdu_id == 0 ||
      next_pdu_id == std::numeric_limits<std::uint64_t>::max()) {
    throw std::invalid_argument("SUE packetizer requires a valid SIGNAL operation and identities");
  }
  const contracts::SueTransaction transaction =
      contracts::SueTransaction::signal_command(
          contracts::TransactionId(next_transaction_id++),
          operation.id,
          operation.issuer,
          operation.semaphore_owner,
          1,
          operation.ordering_domain);
  const contracts::PduId pdu_id(next_pdu_id++);
  SuePdu pdu{
      pdu_id,
      pdu_id.value(),
      operation.issuer,
      operation.semaphore_owner,
      1,
      operation.ordering_domain,
      {transaction},
      transaction.logical_bytes,
      config_.pdu_overhead_bytes,
      checked_add(
          transaction.logical_bytes,
          config_.pdu_overhead_bytes,
          "SIGNAL PDU wire byte count overflow")};
  pdu.require_valid();
  return {pdu};
}

std::vector<SuePdu> SuePacketizer::packetize_get_request(
    const contracts::MicroOp& operation,
    std::uint64_t& next_transaction_id,
    std::uint64_t& next_pdu_id) const {
  operation.require_valid();
  if (operation.kind != contracts::MicroOpKind::Get) {
    throw std::invalid_argument("SUE packetizer requires a GET operation");
  }
  std::vector<SuePdu> pdus;
  std::uint64_t processed = 0;
  while (processed < operation.source.length_bytes) {
    if (next_transaction_id == 0 || next_pdu_id == 0 ||
        next_transaction_id == std::numeric_limits<std::uint64_t>::max() ||
        next_pdu_id == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("GET request identity exhausted");
    }
    const std::uint64_t requested = std::min(
        config_.transaction_payload_bytes,
        operation.source.length_bytes - processed);
    const contracts::SueTransaction transaction =
        contracts::SueTransaction::get_command(
            contracts::TransactionId(next_transaction_id++), operation.id,
            operation.issuer, sub_slice(operation.source, processed, requested),
            sub_slice(operation.destination, processed, requested), 2,
            operation.ordering_domain);
    const contracts::PduId pdu_id(next_pdu_id++);
    SuePdu pdu{
        pdu_id, pdu_id.value(), operation.issuer, operation.source.owner, 2,
        operation.ordering_domain, {transaction}, transaction.logical_bytes,
        config_.pdu_overhead_bytes,
        checked_add(transaction.logical_bytes, config_.pdu_overhead_bytes,
                    "GET request PDU wire byte count overflow")};
    pdu.require_valid();
    pdus.push_back(pdu);
    processed += requested;
  }
  return pdus;
}

std::vector<SuePdu> SuePacketizer::packetize_get_response(
    const contracts::MicroOp& operation,
    std::uint64_t& next_transaction_id,
    std::uint64_t& next_pdu_id) const {
  operation.require_valid();
  if (operation.kind != contracts::MicroOpKind::Get) {
    throw std::invalid_argument("SUE packetizer requires a GET operation");
  }
  std::vector<SuePdu> pdus;
  std::uint64_t processed = 0;
  while (processed < operation.source.length_bytes) {
    if (next_pdu_id == 0 ||
        next_pdu_id == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("GET response PDU identity exhausted");
    }
    std::vector<contracts::SueTransaction> transactions;
    std::uint64_t pdu_payload = 0;
    while (processed < operation.source.length_bytes &&
           pdu_payload < config_.pdu_payload_bytes) {
      if (next_transaction_id == 0 ||
          next_transaction_id == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("GET response transaction identity exhausted");
      }
      const std::uint64_t chunk = std::min(
          operation.source.length_bytes - processed,
          std::min(config_.transaction_payload_bytes,
                   config_.pdu_payload_bytes - pdu_payload));
      transactions.push_back(contracts::SueTransaction::get_response(
          contracts::TransactionId(next_transaction_id++), operation.id,
          sub_slice(operation.source, processed, chunk),
          sub_slice(operation.destination, processed, chunk), 2,
          operation.ordering_domain, contracts::SueResponseStatus::Success));
      processed += chunk;
      pdu_payload += chunk;
    }
    const contracts::PduId pdu_id(next_pdu_id++);
    SuePdu pdu{
        pdu_id, pdu_id.value(), operation.source.owner,
        operation.destination.owner, 2, operation.ordering_domain,
        transactions, pdu_payload, config_.pdu_overhead_bytes,
        checked_add(pdu_payload, config_.pdu_overhead_bytes,
                    "GET response PDU wire byte count overflow")};
    pdu.require_valid();
    pdus.push_back(pdu);
  }
  return pdus;
}

}  // namespace runtime
}  // namespace simai
