#include "simai/runtime/allreduce_planner.hpp"

#include <stdexcept>
#include <vector>

namespace simai {
namespace runtime {
namespace {

contracts::BufferSlice buffer_slice(
    contracts::DeviceId owner,
    std::uint64_t buffer,
    std::uint64_t length) {
  return contracts::BufferSlice{
      owner,
      contracts::BufferId(buffer),
      0,
      length,
      contracts::MemoryRegion::DeviceLocal};
}

}  // namespace

contracts::CollectivePlan make_gather_reduce_broadcast_allreduce(
    contracts::CollectivePlanId plan_id,
    const contracts::ParallelGroup& group,
    std::uint64_t payload_bytes) {
  group.require_valid();
  if (payload_bytes == 0) {
    throw std::invalid_argument(
        "gather-reduce-broadcast AllReduce requires nonzero payload");
  }

  const contracts::DeviceId root = group.members.front();
  const contracts::BufferSlice accumulator =
      buffer_slice(root, 3001, payload_bytes);
  std::vector<contracts::MicroOp> operations;
  std::vector<contracts::OperationId> gather_operations;
  std::uint64_t next_operation = 1;

  for (std::size_t rank = 1; rank < group.members.size(); ++rank) {
    const contracts::DeviceId peer = group.members[rank];
    const contracts::OperationId operation(next_operation++);
    gather_operations.push_back(operation);
    operations.push_back(contracts::MicroOp::put(
        operation,
        peer,
        contracts::ThreadBlockId(1),
        0,
        {},
        buffer_slice(peer, 1000 + peer.value(), payload_bytes),
        buffer_slice(root, 2000 + peer.value(), payload_bytes),
        contracts::MemoryOrder::Ordered,
        contracts::OrderingDomainId(1),
        contracts::CompletionStage::RemoteVisible));
  }

  contracts::OperationId previous_reduce;
  for (std::size_t rank = 1; rank < group.members.size(); ++rank) {
    std::vector<contracts::OperationId> dependencies{
        gather_operations[rank - 1]};
    if (previous_reduce.valid()) {
      dependencies.push_back(previous_reduce);
    }
    const contracts::DeviceId peer = group.members[rank];
    const contracts::OperationId operation(next_operation++);
    operations.push_back(contracts::MicroOp::reduce(
        operation,
        root,
        contracts::ThreadBlockId(1),
        static_cast<std::uint32_t>(rank - 1),
        dependencies,
        buffer_slice(root, 2000 + peer.value(), payload_bytes),
        accumulator,
        contracts::ReductionKind::Sum,
        contracts::MemoryOrder::Ordered,
        contracts::OrderingDomainId(1),
        contracts::CompletionStage::LocalComplete));
    previous_reduce = operation;
  }

  for (std::size_t rank = 1; rank < group.members.size(); ++rank) {
    const contracts::DeviceId peer = group.members[rank];
    operations.push_back(contracts::MicroOp::put(
        contracts::OperationId(next_operation++),
        root,
        contracts::ThreadBlockId(100 + rank),
        0,
        {previous_reduce},
        accumulator,
        buffer_slice(peer, 4000 + peer.value(), payload_bytes),
        contracts::MemoryOrder::Ordered,
        contracts::OrderingDomainId(1),
        contracts::CompletionStage::RemoteVisible));
  }

  contracts::CollectivePlan plan{
      plan_id,
      contracts::CollectiveKind::AllReduce,
      group,
      payload_bytes,
      operations};
  plan.require_valid();
  return plan;
}

}  // namespace runtime
}  // namespace simai
