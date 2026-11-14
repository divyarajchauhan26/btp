#include "simai/runtime/collective_planner.hpp"

#include <stdexcept>
#include <vector>

#include "simai/runtime/allreduce_planner.hpp"

namespace simai {
namespace runtime {
namespace {

contracts::BufferSlice chunk_slice(
    contracts::DeviceId owner,
    std::uint64_t buffer,
    std::uint64_t chunk,
    std::uint64_t chunk_bytes) {
  return contracts::BufferSlice{
      owner,
      contracts::BufferId(buffer),
      chunk * chunk_bytes,
      chunk_bytes,
      contracts::MemoryRegion::DeviceLocal};
}

void append_dependency(
    std::vector<contracts::OperationId>& dependencies,
    contracts::OperationId dependency) {
  if (!dependency.valid()) {
    return;
  }
  for (const contracts::OperationId existing : dependencies) {
    if (existing == dependency) {
      return;
    }
  }
  dependencies.push_back(dependency);
}

contracts::CollectivePlan make_ring_collective(
    const CollectiveRequest& request) {
  const std::size_t count = request.group.members.size();
  if (request.payload_bytes % count != 0) {
    throw std::invalid_argument(
        "ring collective requires payload divisible by group size");
  }
  const std::uint64_t chunk_bytes = request.payload_bytes / count;
  std::vector<contracts::MicroOp> operations;
  std::vector<contracts::OperationId> last(count);
  std::uint64_t next_operation = 1;

  const bool has_reduce_scatter =
      request.kind == contracts::CollectiveKind::AllReduce ||
      request.kind == contracts::CollectiveKind::ReduceScatter;
  const bool has_allgather =
      request.kind == contracts::CollectiveKind::AllReduce ||
      request.kind == contracts::CollectiveKind::AllGather;

  if (has_reduce_scatter) {
    for (std::size_t step = 0; step + 1 < count; ++step) {
      const std::vector<contracts::OperationId> previous = last;
      std::vector<contracts::OperationId> next(count);
      for (std::size_t rank = 0; rank < count; ++rank) {
        const std::size_t peer_rank = (rank + 1) % count;
        const std::uint64_t chunk = (rank + count - step) % count;
        const contracts::DeviceId issuer = request.group.members[rank];
        const contracts::DeviceId peer = request.group.members[peer_rank];
        std::vector<contracts::OperationId> put_dependencies;
        append_dependency(put_dependencies, previous[rank]);
        const contracts::OperationId put_id(next_operation++);
        operations.push_back(contracts::MicroOp::put(
            put_id,
            issuer,
            contracts::ThreadBlockId(put_id.value()),
            0,
            put_dependencies,
            chunk_slice(issuer, 5000 + issuer.value(), chunk, chunk_bytes),
            chunk_slice(peer, 6000 + issuer.value(), chunk, chunk_bytes),
            contracts::MemoryOrder::Ordered,
            contracts::OrderingDomainId(1),
            contracts::CompletionStage::RemoteVisible));
        std::vector<contracts::OperationId> reduce_dependencies{put_id};
        append_dependency(reduce_dependencies, previous[peer_rank]);
        const contracts::OperationId reduce_id(next_operation++);
        operations.push_back(contracts::MicroOp::reduce(
            reduce_id,
            peer,
            contracts::ThreadBlockId(reduce_id.value()),
            0,
            reduce_dependencies,
            chunk_slice(peer, 6000 + issuer.value(), chunk, chunk_bytes),
            chunk_slice(peer, 5000 + peer.value(), chunk, chunk_bytes),
            request.reduction,
            contracts::MemoryOrder::Ordered,
            contracts::OrderingDomainId(1),
            contracts::CompletionStage::LocalComplete));
        next[peer_rank] = reduce_id;
      }
      last = next;
    }
  }

  if (has_allgather) {
    const std::uint64_t initial_offset = has_reduce_scatter ? 1 : 0;
    for (std::size_t step = 0; step + 1 < count; ++step) {
      const std::vector<contracts::OperationId> previous = last;
      std::vector<contracts::OperationId> next(count);
      for (std::size_t rank = 0; rank < count; ++rank) {
        const std::size_t peer_rank = (rank + 1) % count;
        const std::uint64_t chunk =
            (rank + count - step + initial_offset) % count;
        const contracts::DeviceId issuer = request.group.members[rank];
        const contracts::DeviceId peer = request.group.members[peer_rank];
        std::vector<contracts::OperationId> dependencies;
        append_dependency(dependencies, previous[rank]);
        const contracts::OperationId put_id(next_operation++);
        operations.push_back(contracts::MicroOp::put(
            put_id,
            issuer,
            contracts::ThreadBlockId(put_id.value()),
            0,
            dependencies,
            chunk_slice(issuer, 5000 + issuer.value(), chunk, chunk_bytes),
            chunk_slice(peer, 5000 + peer.value(), chunk, chunk_bytes),
            contracts::MemoryOrder::Ordered,
            contracts::OrderingDomainId(1),
            contracts::CompletionStage::RemoteVisible));
        next[peer_rank] = put_id;
      }
      last = next;
    }
  }

  contracts::CollectivePlan plan{
      request.id,
      request.kind,
      request.group,
      request.payload_bytes,
      operations};
  plan.require_valid();
  return plan;
}

contracts::CollectivePlan make_direct_alltoall(
    const CollectiveRequest& request) {
  const std::size_t count = request.group.members.size();
  if (request.payload_bytes % count != 0) {
    throw std::invalid_argument(
        "direct AllToAll requires payload divisible by group size");
  }
  const std::uint64_t chunk_bytes = request.payload_bytes / count;
  std::vector<contracts::MicroOp> operations;
  std::uint64_t next_operation = 1;
  for (std::size_t rank = 0; rank < count; ++rank) {
    const contracts::DeviceId issuer = request.group.members[rank];
    for (std::size_t peer_rank = 0; peer_rank < count; ++peer_rank) {
      if (peer_rank == rank) {
        continue;
      }
      const contracts::DeviceId peer = request.group.members[peer_rank];
      const contracts::OperationId operation(next_operation++);
      operations.push_back(contracts::MicroOp::put(
          operation,
          issuer,
          contracts::ThreadBlockId(operation.value()),
          0,
          {},
          chunk_slice(issuer, 7000 + issuer.value(), peer_rank, chunk_bytes),
          chunk_slice(peer, 8000 + peer.value(), rank, chunk_bytes),
          contracts::MemoryOrder::Ordered,
          contracts::OrderingDomainId(1),
          contracts::CompletionStage::RemoteVisible));
    }
  }
  contracts::CollectivePlan plan{
      request.id,
      request.kind,
      request.group,
      request.payload_bytes,
      operations};
  plan.require_valid();
  return plan;
}

}  // namespace

PlannedCollective NativeCollectivePlanner::plan(
    const CollectiveRequest& request,
    const PlannerCapabilities& capabilities) const {
  request.group.require_valid();
  const bool reduction_collective =
      request.kind == contracts::CollectiveKind::AllReduce ||
      request.kind == contracts::CollectiveKind::ReduceScatter;
  const bool movement_collective =
      request.kind == contracts::CollectiveKind::AllGather ||
      request.kind == contracts::CollectiveKind::AllToAll;
  if (!request.id.valid() || request.payload_bytes == 0 ||
      (!reduction_collective && !movement_collective) ||
      (reduction_collective &&
       request.reduction != contracts::ReductionKind::Sum) ||
      (movement_collective &&
       request.reduction != contracts::ReductionKind::None)) {
    throw std::invalid_argument("native planner received an unsupported request");
  }
  if (request.group.members.size() > capabilities.maximum_endpoints ||
      !capabilities.native_put ||
      (reduction_collective && !capabilities.native_reduce)) {
    throw std::invalid_argument("native planner capabilities do not satisfy request");
  }

  CollectiveAlgorithmPolicy selected = request.algorithm;
  if (selected == CollectiveAlgorithmPolicy::Auto) {
    if (request.kind == contracts::CollectiveKind::AllToAll) {
      selected = CollectiveAlgorithmPolicy::Direct;
    } else if (request.kind == contracts::CollectiveKind::AllReduce &&
               request.payload_bytes % request.group.members.size() != 0) {
      selected = CollectiveAlgorithmPolicy::GatherReduceBroadcast;
    } else {
      selected = CollectiveAlgorithmPolicy::Ring;
    }
  }
  if (selected == CollectiveAlgorithmPolicy::GatherReduceBroadcast) {
    if (request.kind != contracts::CollectiveKind::AllReduce) {
      throw std::invalid_argument(
          "gather-reduce-broadcast supports AllReduce only");
    }
    return PlannedCollective{
        make_gather_reduce_broadcast_allreduce(
            request.id, request.group, request.payload_bytes),
        ScheduleProvenance{"native", "gather_reduce_broadcast", "1"}};
  }
  if (selected == CollectiveAlgorithmPolicy::Ring) {
    if (request.kind == contracts::CollectiveKind::AllToAll) {
      throw std::invalid_argument("ring policy does not support AllToAll");
    }
    return PlannedCollective{
        make_ring_collective(request),
        ScheduleProvenance{"native", "ring", "1"}};
  }
  if (selected == CollectiveAlgorithmPolicy::Direct &&
      request.kind == contracts::CollectiveKind::AllToAll) {
    return PlannedCollective{
        make_direct_alltoall(request),
        ScheduleProvenance{"native", "direct_alltoall", "1"}};
  }
  throw std::invalid_argument("native planner algorithm is unsupported");
}

}  // namespace runtime
}  // namespace simai
