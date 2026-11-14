#ifndef SIMAI_CONTRACTS_SUE_TRANSACTION_HPP
#define SIMAI_CONTRACTS_SUE_TRANSACTION_HPP

#include <cstdint>

#include "simai/contracts/buffer.hpp"
#include "simai/contracts/ids.hpp"
#include "simai/contracts/validation.hpp"

namespace simai {
namespace contracts {

enum class SueService { Put, Get, Atomic, Control };
enum class SueMessageRole { Command, Response };
enum class SueResponseStatus { NotApplicable, Success, Failed };

struct SueTransaction {
  TransactionId id;
  OperationId parent_operation;
  DeviceId source_endpoint;
  DeviceId destination_endpoint;
  SueService service;
  SueMessageRole role;
  std::uint8_t virtual_channel;
  OrderingDomainId ordering_domain;
  std::uint64_t logical_bytes;
  bool data_present;
  bool has_source;
  BufferSlice source;
  bool has_destination;
  BufferSlice destination;
  SueResponseStatus response_status;

  static SueTransaction put_command(
      TransactionId id,
      OperationId parent_operation,
      const BufferSlice& source,
      const BufferSlice& destination,
      std::uint8_t virtual_channel,
      OrderingDomainId ordering_domain);

  static SueTransaction put_response(
      TransactionId id,
      OperationId parent_operation,
      DeviceId responder,
      DeviceId requester,
      std::uint8_t virtual_channel,
      OrderingDomainId ordering_domain,
      SueResponseStatus status);

  static SueTransaction get_command(
      TransactionId id,
      OperationId parent_operation,
      DeviceId requester,
      const BufferSlice& remote_source,
      const BufferSlice& local_destination,
      std::uint8_t virtual_channel,
      OrderingDomainId ordering_domain);

  static SueTransaction get_response(
      TransactionId id,
      OperationId parent_operation,
      const BufferSlice& remote_source,
      const BufferSlice& local_destination,
      std::uint8_t virtual_channel,
      OrderingDomainId ordering_domain,
      SueResponseStatus status);

  static SueTransaction signal_command(
      TransactionId id,
      OperationId parent_operation,
      DeviceId source,
      DeviceId destination,
      std::uint8_t virtual_channel,
      OrderingDomainId ordering_domain);

  ValidationErrors validate() const;
  void require_valid() const;
};

}  // namespace contracts
}  // namespace simai

#endif
