#include "simai/contracts/sue_transaction.hpp"

namespace simai {
namespace contracts {
namespace {

BufferSlice empty_slice() {
  return BufferSlice{DeviceId(), BufferId(), 0, 0, MemoryRegion::DeviceLocal};
}

void append_all(ValidationErrors& destination, const ValidationErrors& source) {
  destination.insert(destination.end(), source.begin(), source.end());
}

}  // namespace

SueTransaction SueTransaction::put_command(
    TransactionId id,
    OperationId parent_operation,
    const BufferSlice& source,
    const BufferSlice& destination,
    std::uint8_t virtual_channel,
    OrderingDomainId ordering_domain) {
  return SueTransaction{
      id,
      parent_operation,
      source.owner,
      destination.owner,
      SueService::Put,
      SueMessageRole::Command,
      virtual_channel,
      ordering_domain,
      source.length_bytes,
      true,
      true,
      source,
      true,
      destination,
      SueResponseStatus::NotApplicable};
}

SueTransaction SueTransaction::put_response(
    TransactionId id,
    OperationId parent_operation,
    DeviceId responder,
    DeviceId requester,
    std::uint8_t virtual_channel,
    OrderingDomainId ordering_domain,
    SueResponseStatus status) {
  return SueTransaction{
      id,
      parent_operation,
      responder,
      requester,
      SueService::Put,
      SueMessageRole::Response,
      virtual_channel,
      ordering_domain,
      0,
      false,
      false,
      empty_slice(),
      false,
      empty_slice(),
      status};
}

SueTransaction SueTransaction::get_command(
    TransactionId id,
    OperationId parent_operation,
    DeviceId requester,
    const BufferSlice& remote_source,
    const BufferSlice& local_destination,
    std::uint8_t virtual_channel,
    OrderingDomainId ordering_domain) {
  return SueTransaction{
      id,
      parent_operation,
      requester,
      remote_source.owner,
      SueService::Get,
      SueMessageRole::Command,
      virtual_channel,
      ordering_domain,
      8,
      false,
      true,
      remote_source,
      true,
      local_destination,
      SueResponseStatus::NotApplicable};
}

SueTransaction SueTransaction::get_response(
    TransactionId id,
    OperationId parent_operation,
    const BufferSlice& remote_source,
    const BufferSlice& local_destination,
    std::uint8_t virtual_channel,
    OrderingDomainId ordering_domain,
    SueResponseStatus status) {
  return SueTransaction{
      id,
      parent_operation,
      remote_source.owner,
      local_destination.owner,
      SueService::Get,
      SueMessageRole::Response,
      virtual_channel,
      ordering_domain,
      remote_source.length_bytes,
      status == SueResponseStatus::Success,
      true,
      remote_source,
      true,
      local_destination,
      status};
}

SueTransaction SueTransaction::signal_command(
    TransactionId id,
    OperationId parent_operation,
    DeviceId source,
    DeviceId destination,
    std::uint8_t virtual_channel,
    OrderingDomainId ordering_domain) {
  return SueTransaction{
      id,
      parent_operation,
      source,
      destination,
      SueService::Atomic,
      SueMessageRole::Command,
      virtual_channel,
      ordering_domain,
      8,
      true,
      false,
      empty_slice(),
      false,
      empty_slice(),
      SueResponseStatus::NotApplicable};
}

ValidationErrors SueTransaction::validate() const {
  ValidationErrors errors;
  if (!id.valid() || !parent_operation.valid() ||
      !source_endpoint.valid() || !destination_endpoint.valid() ||
      !ordering_domain.valid()) {
    append_error(
        errors,
        ValidationCode::InvalidIdentity,
        "transaction identities and endpoints must be nonzero");
  }
  if (source_endpoint == destination_endpoint) {
    append_error(
        errors,
        ValidationCode::InvalidTransaction,
        "transaction endpoints must be distinct");
  }
  if (virtual_channel > 3) {
    append_error(
        errors,
        ValidationCode::InvalidTransaction,
        "virtual channel must be in the inclusive range zero through three");
  }
  if (logical_bytes > 256) {
    append_error(
        errors,
        ValidationCode::InvalidTransaction,
        "SUE service transaction data may not exceed 256 bytes");
  }
  if (has_source) {
    append_all(errors, source.validate());
  }
  if (has_destination) {
    append_all(errors, destination.validate());
  }
  if (has_source && has_destination &&
      source.length_bytes != destination.length_bytes) {
    append_error(
        errors,
        ValidationCode::InvalidTransaction,
        "transaction source and destination lengths must match");
  }

  if (role == SueMessageRole::Command &&
      response_status != SueResponseStatus::NotApplicable) {
    append_error(
        errors,
        ValidationCode::InvalidTransaction,
        "command may not carry response status");
  }
  if (role == SueMessageRole::Response &&
      response_status == SueResponseStatus::NotApplicable) {
    append_error(
        errors,
        ValidationCode::InvalidTransaction,
        "response requires explicit status");
  }

  if (service == SueService::Put && role == SueMessageRole::Command) {
    if (!has_source || !has_destination || !data_present ||
        logical_bytes == 0 || source.owner != source_endpoint ||
        destination.owner != destination_endpoint) {
      append_error(
          errors,
          ValidationCode::InvalidTransaction,
          "PUT command requires data and endpoint-owned source/destination slices");
    }
  } else if (service == SueService::Put && role == SueMessageRole::Response) {
    if (has_source || has_destination || data_present || logical_bytes != 0) {
      append_error(
          errors,
          ValidationCode::InvalidTransaction,
          "PUT response carries status only");
    }
  } else if (service == SueService::Get && role == SueMessageRole::Command) {
    if (!has_source || !has_destination || data_present ||
        logical_bytes == 0 || source.owner != destination_endpoint ||
        destination.owner != source_endpoint) {
      append_error(
          errors,
          ValidationCode::InvalidTransaction,
          "GET command requests remote source into requester-owned destination");
    }
  } else if (service == SueService::Get && role == SueMessageRole::Response) {
    const bool success = response_status == SueResponseStatus::Success;
    if (!has_source || !has_destination || logical_bytes == 0 ||
        data_present != success || source.owner != source_endpoint ||
        destination.owner != destination_endpoint) {
      append_error(
          errors,
          ValidationCode::InvalidTransaction,
          "GET response data presence must match success status and slice ownership");
    }
  } else if (service == SueService::Atomic &&
             role == SueMessageRole::Command) {
    if (has_source || has_destination || !data_present || logical_bytes != 8) {
      append_error(
          errors,
          ValidationCode::InvalidTransaction,
          "SIGNAL atomic command must carry one eight-byte semaphore value");
    }
  } else {
    append_error(
        errors,
        ValidationCode::InvalidTransaction,
        "unsupported SUE service and message-role combination");
  }
  return errors;
}

void SueTransaction::require_valid() const {
  require_no_errors(validate(), "SueTransaction");
}

}  // namespace contracts
}  // namespace simai
