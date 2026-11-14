#include "simai/runtime/fine_remote.hpp"

#include <stdexcept>

namespace simai {
namespace runtime {
namespace {

contracts::FineMemoryReference empty_reference() {
  return contracts::FineMemoryReference{};
}

contracts::BufferSlice buffer_slice(
    const contracts::FineMemoryReference& reference) {
  return contracts::BufferSlice{
      reference.owner,
      reference.buffer,
      reference.offset_bytes,
      reference.length_bytes,
      contracts::MemoryRegion::DeviceLocal};
}

contracts::MicroOp remote_micro_op(
    const contracts::FineRemoteRequest& request,
    contracts::BufferId semaphore_register_buffer) {
  const contracts::OperationId operation(request.id.value());
  const contracts::ThreadBlockId thread_block(request.workgroup_instance.value());
  const contracts::OrderingDomainId ordering(request.wavefront_instance.value());
  const std::vector<contracts::OperationId> dependencies;
  if (request.kind == contracts::FineMemoryRequestKind::RemoteStore) {
    return contracts::MicroOp::put(
        operation,
        request.issuer,
        thread_block,
        0,
        dependencies,
        buffer_slice(request.source),
        buffer_slice(request.destination),
        contracts::MemoryOrder::Ordered,
        ordering,
        request.completion_stage);
  }
  if (request.kind == contracts::FineMemoryRequestKind::RemoteLoadRequest) {
    return contracts::MicroOp::get(
        operation,
        request.issuer,
        thread_block,
        0,
        dependencies,
        buffer_slice(request.source),
        buffer_slice(request.destination),
        contracts::MemoryOrder::Ordered,
        ordering,
        contracts::CompletionStage::LocalComplete);
  }
  if (request.kind == contracts::FineMemoryRequestKind::SemaphoreRelease) {
    return contracts::MicroOp::signal(
        operation,
        request.issuer,
        thread_block,
        0,
        dependencies,
        request.semaphore_owner,
        request.semaphore,
        request.semaphore_generation,
        contracts::MemoryOrder::Ordered,
        ordering,
        request.completion_stage);
  }
  if (request.kind == contracts::FineMemoryRequestKind::SemaphoreAcquire) {
    contracts::FineMemoryReference destination{
        request.issuer,
        contracts::FineMemorySpace::RegisterFile,
        semaphore_register_buffer,
        0,
        request.source.length_bytes,
        request.source.alignment_bytes};
    destination.require_valid();
    return contracts::MicroOp::get(
        operation,
        request.issuer,
        thread_block,
        0,
        dependencies,
        buffer_slice(request.source),
        buffer_slice(destination),
        contracts::MemoryOrder::Ordered,
        ordering,
        contracts::CompletionStage::LocalComplete);
  }
  throw std::invalid_argument("fine remote request kind cannot be packetized");
}

}  // namespace

contracts::FineRemoteRequest lower_remote_request(
    contracts::MemoryRequestId request_id,
    const FineInstructionIssue& issue) {
  const contracts::FineGpuInstruction& instruction = issue.instruction;
  contracts::FineRemoteRequest request{
      request_id,
      issue.kernel_instance,
      issue.workgroup_instance,
      issue.wavefront_instance,
      instruction.id,
      issue.device,
      contracts::FineMemoryRequestKind::RemoteStore,
      instruction.has_source,
      instruction.has_source ? instruction.source : empty_reference(),
      instruction.has_destination,
      instruction.has_destination ? instruction.destination : empty_reference(),
      instruction.has_semaphore,
      instruction.semaphore_owner,
      instruction.semaphore,
      instruction.semaphore_generation,
      contracts::CompletionStage::RemoteVisible,
      issue.issue_ns};

  if (instruction.kind == contracts::FineGpuInstructionKind::Load) {
    request.kind = contracts::FineMemoryRequestKind::RemoteLoadRequest;
    request.completion_stage = contracts::CompletionStage::Finalized;
  } else if (instruction.kind == contracts::FineGpuInstructionKind::Store) {
    request.kind = contracts::FineMemoryRequestKind::RemoteStore;
  } else if (
      instruction.kind == contracts::FineGpuInstructionKind::SemaphoreAcquire) {
    request.kind = contracts::FineMemoryRequestKind::SemaphoreAcquire;
    request.completion_stage = contracts::CompletionStage::Finalized;
  } else if (
      instruction.kind == contracts::FineGpuInstructionKind::SemaphoreRelease) {
    request.kind = contracts::FineMemoryRequestKind::SemaphoreRelease;
  } else {
    throw std::invalid_argument(
        "fine instruction does not lower to a remote request");
  }
  request.require_valid();
  return request;
}

FineRemotePacketizer::FineRemotePacketizer(
    const LinkConfig& config,
    contracts::BufferId semaphore_register_buffer)
    : packetizer_(config),
      semaphore_register_buffer_(semaphore_register_buffer) {
  if (!semaphore_register_buffer_.valid()) {
    throw std::invalid_argument("fine remote semaphore register buffer is invalid");
  }
}

FineRemotePacketBatch FineRemotePacketizer::packetize(
    const contracts::FineRemoteRequest& request,
    std::uint64_t& next_transaction_id,
    std::uint64_t& next_pdu_id) const {
  request.require_valid();
  const contracts::MicroOp operation =
      remote_micro_op(request, semaphore_register_buffer_);
  std::vector<SuePdu> commands;
  std::vector<SuePdu> responses;
  if (operation.kind == contracts::MicroOpKind::Put) {
    commands = packetizer_.packetize_put(
        operation, next_transaction_id, next_pdu_id);
  } else if (operation.kind == contracts::MicroOpKind::Signal) {
    commands = packetizer_.packetize_signal(
        operation, next_transaction_id, next_pdu_id);
  } else if (operation.kind == contracts::MicroOpKind::Get) {
    commands = packetizer_.packetize_get_request(
        operation, next_transaction_id, next_pdu_id);
    responses = packetizer_.packetize_get_response(
        operation, next_transaction_id, next_pdu_id);
  } else {
    throw std::logic_error("fine remote adapter created unsupported MicroOp");
  }
  return FineRemotePacketBatch{request, commands, responses};
}

}  // namespace runtime
}  // namespace simai
