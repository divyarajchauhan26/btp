#include "simai/runtime/simulator.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "simai/contracts/completion.hpp"
#include "simai/contracts/event_queue.hpp"
#include "simai/contracts/workload.hpp"
#include "simai/runtime/collective_planner.hpp"
#include "simai/runtime/gpu_execution.hpp"
#include "simai/runtime/reassembly.hpp"
#include "simai/runtime/transport_backend.hpp"

namespace simai {
namespace runtime {
namespace {

using contracts::CompletionStage;
using contracts::MicroOp;
using contracts::MicroOpKind;
using contracts::SimTimeNs;

contracts::CollectiveKind collective_kind(LayerCollectiveKind kind) {
  if (kind == LayerCollectiveKind::AllReduce) {
    return contracts::CollectiveKind::AllReduce;
  }
  if (kind == LayerCollectiveKind::ReduceScatter) {
    return contracts::CollectiveKind::ReduceScatter;
  }
  if (kind == LayerCollectiveKind::AllGather) {
    return contracts::CollectiveKind::AllGather;
  }
  if (kind == LayerCollectiveKind::AllToAll) {
    return contracts::CollectiveKind::AllToAll;
  }
  throw std::invalid_argument("layer does not contain a supported collective");
}

SimTimeNs checked_add(SimTimeNs lhs, SimTimeNs rhs, const char* message) {
  if (lhs > std::numeric_limits<SimTimeNs>::max() - rhs) {
    throw std::overflow_error(message);
  }
  return lhs + rhs;
}

struct PendingEvent {
  std::size_t operation_index;
  std::size_t pdu_index;
};

struct OperationExecutionState {
  SimTimeNs issued_ns;
  SimTimeNs gpu_local_complete_ns;
  SimTimeNs local_complete_ns;
  SimTimeNs remote_arrival_ns;
  std::size_t pending_switch_pdus;
  bool completion_scheduled;
};

SimTimeNs execute_collective(
    const contracts::CollectivePlan& plan,
    SimTimeNs issue_ns,
    const SuePacketizer& packetizer,
    SueTransportBackend& fabric,
    GpuExecutionModel& gpu,
    std::uint64_t& next_transaction,
    std::uint64_t& next_pdu,
    contracts::ConservationCounters& counters) {
  plan.require_valid();
  contracts::DeterministicEventQueue queue;
  std::unordered_map<contracts::OperationId, std::size_t> operation_index;
  std::vector<std::vector<std::size_t>> dependents(plan.operations.size());
  std::vector<std::size_t> remaining_dependencies(plan.operations.size(), 0);
  std::vector<bool> finalized(plan.operations.size(), false);
  std::vector<bool> blocked_waits(plan.operations.size(), false);
  std::map<std::pair<contracts::DeviceId, contracts::SemaphoreId>,
           std::uint64_t> semaphores;
  std::vector<OperationExecutionState> operation_states(
      plan.operations.size(),
      OperationExecutionState{0, 0, 0, 0, 0, false});
  std::vector<std::unique_ptr<PduReassembly>> reassembly(
      plan.operations.size());
  std::vector<std::vector<SuePdu>> get_responses(plan.operations.size());
  std::vector<bool> get_response_started(plan.operations.size(), false);
  std::vector<SuePdu> in_flight_pdus;
  const std::size_t no_pdu = std::numeric_limits<std::size_t>::max();

  for (std::size_t index = 0; index < plan.operations.size(); ++index) {
    operation_index.insert(std::make_pair(plan.operations[index].id, index));
    remaining_dependencies[index] = plan.operations[index].dependencies.size();
  }
  for (std::size_t index = 0; index < plan.operations.size(); ++index) {
    for (const contracts::OperationId dependency :
         plan.operations[index].dependencies) {
      dependents[operation_index.at(dependency)].push_back(index);
    }
  }

  std::unordered_map<contracts::EventId, PendingEvent> pending;
  std::uint64_t next_event = 1;
  auto schedule = [&](std::size_t operation,
                      SimTimeNs time,
                      contracts::EventKind kind,
                      std::size_t pdu_index) {
    const contracts::EventId event(next_event++);
    pending.insert(
        std::make_pair(event, PendingEvent{operation, pdu_index}));
    queue.schedule(event, time, kind);
  };
  const auto schedule_completion = [&](std::size_t index) {
    const MicroOp& operation = plan.operations[index];
    const OperationExecutionState& state = operation_states[index];
    contracts::CompletionTimeline timeline(
        operation.id, operation.release_stage);
    timeline.record(CompletionStage::Issued, state.issued_ns);
    timeline.record(
        CompletionStage::LocalComplete, state.local_complete_ns);
    timeline.record(
        CompletionStage::RemoteArrived, state.remote_arrival_ns);
    timeline.record(
        CompletionStage::RemoteVisible, state.remote_arrival_ns);
    timeline.record(CompletionStage::Finalized, state.remote_arrival_ns);
    if (!timeline.finalized()) {
      throw std::logic_error("operation timeline did not finalize");
    }
    schedule(
        index,
        state.remote_arrival_ns,
        contracts::EventKind::CompletionReady,
        no_pdu);
  };
  const auto schedule_gpu_dispatch = [&](const GpuDispatch& dispatch) {
    OperationExecutionState& state =
        operation_states[dispatch.operation_index];
    state.gpu_local_complete_ns = dispatch.local_execution_complete_ns;
    schedule(
        dispatch.operation_index,
        dispatch.instruction_ready_ns,
        contracts::EventKind::GpuInstructionReady,
        no_pdu);
  };
  const auto schedule_wait_completion = [&](std::size_t index, SimTimeNs time) {
    OperationExecutionState& state = operation_states[index];
    state.gpu_local_complete_ns = time;
    state.local_complete_ns = time;
    state.remote_arrival_ns = time;
    state.completion_scheduled = true;
    blocked_waits[index] = false;
    schedule_completion(index);
  };
  const auto transmit_pdu = [&](std::size_t index,
                                const SuePdu& pdu,
                                SimTimeNs time,
                                bool update_local_completion) {
    OperationExecutionState& state = operation_states[index];
    const FabricIngressTiming timing = fabric.transmit_ingress(pdu, time);
    if (update_local_completion) {
      state.local_complete_ns = std::max(
          state.local_complete_ns, timing.local_complete_ns);
    }
    in_flight_pdus.push_back(pdu);
    const std::size_t pdu_index = in_flight_pdus.size() - 1;
    if (fabric.has_switch()) {
      ++state.pending_switch_pdus;
      schedule(index, timing.ingress_arrival_ns,
               contracts::EventKind::SueTransactionReady, pdu_index);
    } else {
      schedule(index, timing.ingress_arrival_ns,
               contracts::EventKind::PduArrivalReady, pdu_index);
    }
    counters.transactions += pdu.transactions.size();
    counters.logical_transaction_bytes = checked_add(
        counters.logical_transaction_bytes, pdu.payload_bytes,
        "logical transaction byte counter overflow");
    ++counters.pdus;
    counters.packets += timing.transmission_attempts;
    counters.retries += timing.retries;
    counters.pdu_payload_bytes = checked_add(
        counters.pdu_payload_bytes, pdu.payload_bytes,
        "PDU payload byte counter overflow");
    counters.wire_bytes = checked_add(
        counters.wire_bytes, pdu.wire_bytes,
        "wire byte counter overflow");
    counters.transmitted_wire_bytes = checked_add(
        counters.transmitted_wire_bytes, timing.transmitted_wire_bytes,
        "transmitted wire byte counter overflow");
  };

  for (std::size_t index = 0; index < plan.operations.size(); ++index) {
    if (remaining_dependencies[index] == 0) {
      schedule(
          index,
          issue_ns,
          contracts::EventKind::MicroOpReady,
          no_pdu);
    }
  }

  SimTimeNs collective_complete_ns = issue_ns;
  while (!queue.empty()) {
    const contracts::ScheduledEvent event = queue.pop();
    const auto found = pending.find(event.id);
    if (found == pending.end()) {
      throw std::logic_error("scheduled event has no execution payload");
    }
    const PendingEvent payload = found->second;
    const std::size_t index = payload.operation_index;
    pending.erase(found);
    const MicroOp& operation = plan.operations[index];

    if (event.kind == contracts::EventKind::MicroOpReady) {
      const std::vector<GpuDispatch> dispatches =
          gpu.admit(index, operation, event.time_ns);
      for (const GpuDispatch& dispatch : dispatches) {
        schedule_gpu_dispatch(dispatch);
      }
      continue;
    }

    if (event.kind == contracts::EventKind::GpuInstructionReady) {
      ++counters.issued_operations;
      OperationExecutionState& state = operation_states[index];
      state.issued_ns = event.time_ns;
      state.local_complete_ns = event.time_ns;
      state.remote_arrival_ns = event.time_ns;
      state.pending_switch_pdus = 0;
      state.completion_scheduled = false;

      if (operation.kind == MicroOpKind::Put ||
          operation.kind == MicroOpKind::Get ||
          operation.kind == MicroOpKind::Signal) {
        const std::vector<SuePdu> pdus =
            operation.kind == MicroOpKind::Put
                ? packetizer.packetize_put(
                      operation, next_transaction, next_pdu)
                : operation.kind == MicroOpKind::Get
                    ? packetizer.packetize_get_request(
                          operation, next_transaction, next_pdu)
                    : packetizer.packetize_signal(
                          operation, next_transaction, next_pdu);
        if (operation.kind == MicroOpKind::Get) {
          get_responses[index] = packetizer.packetize_get_response(
              operation, next_transaction, next_pdu);
        }
        std::vector<contracts::PduId> expected_pdus;
        for (const SuePdu& pdu : pdus) {
          expected_pdus.push_back(pdu.id);
        }
        reassembly[index].reset(new PduReassembly(expected_pdus));
        counters.logical_operation_bytes = checked_add(
            counters.logical_operation_bytes,
            operation.kind == MicroOpKind::Put ||
                    operation.kind == MicroOpKind::Get
                ? operation.source.length_bytes
                : static_cast<std::uint64_t>(8),
            "logical operation byte counter overflow");
        for (const SuePdu& pdu : pdus) {
          transmit_pdu(
              index, pdu, event.time_ns, operation.kind != MicroOpKind::Get);
        }
      } else if (operation.kind == MicroOpKind::Wait) {
        const auto key = std::make_pair(
            operation.semaphore_owner, operation.semaphore);
        if (semaphores[key] >= operation.semaphore_value) {
          schedule_wait_completion(index, event.time_ns);
        } else {
          blocked_waits[index] = true;
        }
      } else if (operation.kind == MicroOpKind::Reduce) {
        state.local_complete_ns = state.gpu_local_complete_ns;
        state.remote_arrival_ns = state.gpu_local_complete_ns;
        state.completion_scheduled = true;
        schedule_completion(index);
      } else {
        throw std::invalid_argument(
            "executor supports PUT and REDUCE operations only");
      }
      continue;
    }

    if (event.kind == contracts::EventKind::SueTransactionReady) {
      if (payload.pdu_index == no_pdu ||
          payload.pdu_index >= in_flight_pdus.size()) {
        throw std::logic_error("switch arrival has no PDU payload");
      }
      OperationExecutionState& state = operation_states[index];
      if (state.pending_switch_pdus == 0) {
        throw std::logic_error("switch PDU completion count underflow");
      }
      const SwitchForwardTiming timing = fabric.forward_from_switch(
          in_flight_pdus[payload.pdu_index], event.time_ns);
      --state.pending_switch_pdus;
      if (!timing.duplicate) {
        schedule(
            index,
            timing.destination_arrival_ns,
            contracts::EventKind::PduArrivalReady,
            payload.pdu_index);
      }
      continue;
    }

    if (event.kind == contracts::EventKind::PduArrivalReady) {
      if (payload.pdu_index == no_pdu ||
          payload.pdu_index >= in_flight_pdus.size() ||
          !reassembly[index]) {
        throw std::logic_error("destination arrival has no reassembly state");
      }
      OperationExecutionState& state = operation_states[index];
      if (reassembly[index]->accept(in_flight_pdus[payload.pdu_index].id)) {
        state.remote_arrival_ns = std::max(
            state.remote_arrival_ns, event.time_ns);
      }
      if (reassembly[index]->complete() && !state.completion_scheduled) {
        if (operation.kind == MicroOpKind::Get &&
            !get_response_started[index]) {
          get_response_started[index] = true;
          std::vector<contracts::PduId> expected_responses;
          for (const SuePdu& response : get_responses[index]) {
            expected_responses.push_back(response.id);
          }
          reassembly[index].reset(new PduReassembly(expected_responses));
          for (const SuePdu& response : get_responses[index]) {
            transmit_pdu(index, response, event.time_ns, false);
          }
          continue;
        }
        if (operation.kind == MicroOpKind::Signal) {
          const auto key = std::make_pair(
              operation.semaphore_owner, operation.semaphore);
          semaphores[key] = std::max(
              semaphores[key], operation.semaphore_value);
          for (std::size_t wait_index = 0;
               wait_index < plan.operations.size(); ++wait_index) {
            const MicroOp& wait = plan.operations[wait_index];
            if (blocked_waits[wait_index] &&
                wait.kind == MicroOpKind::Wait &&
                wait.semaphore_owner == operation.semaphore_owner &&
                wait.semaphore == operation.semaphore &&
                semaphores[key] >= wait.semaphore_value) {
              schedule_wait_completion(wait_index, event.time_ns);
            }
          }
        }
        if (operation.kind == MicroOpKind::Get) {
          state.local_complete_ns = event.time_ns;
          state.remote_arrival_ns = event.time_ns;
        }
        state.completion_scheduled = true;
        schedule_completion(index);
      }
      continue;
    }

    if (event.kind != contracts::EventKind::CompletionReady ||
        finalized[index]) {
      throw std::logic_error("invalid operation completion event");
    }
    finalized[index] = true;
    ++counters.finalized_operations;
    ++counters.completions;
    collective_complete_ns = event.time_ns;
    const std::vector<GpuDispatch> dispatched =
        gpu.complete(index, event.time_ns);
    for (const GpuDispatch& dispatch : dispatched) {
      schedule_gpu_dispatch(dispatch);
    }
    for (const std::size_t dependent : dependents[index]) {
      if (remaining_dependencies[dependent] == 0) {
        throw std::logic_error("dependency count underflow");
      }
      --remaining_dependencies[dependent];
      if (remaining_dependencies[dependent] == 0) {
        schedule(
            dependent,
            event.time_ns,
            contracts::EventKind::MicroOpReady,
            no_pdu);
      }
    }
  }

  for (const bool operation_finalized : finalized) {
    if (!operation_finalized) {
      throw std::logic_error("collective execution left an operation pending");
    }
  }
  return collective_complete_ns;
}

}  // namespace

contracts::SimulationReport ScaleUpSimulator::run(
    const WorkloadSpec& workload) const {
  return run(workload, NativeCollectivePlanner());
}

contracts::SimulationReport ScaleUpSimulator::run(
    const WorkloadSpec& workload,
    const CollectiveScheduleProvider& schedule_provider) const {
  return run(
      workload, schedule_provider, NativeSueTransportBackendFactory());
}

contracts::SimulationReport ScaleUpSimulator::run(
    const WorkloadSpec& workload,
    const CollectiveScheduleProvider& schedule_provider,
    const SueTransportBackendFactory& backend_factory) const {
  workload.require_valid();
  const SuePacketizer packetizer(workload.link);
  std::unique_ptr<SueTransportBackend> fabric =
      backend_factory.create(workload.link, workload.endpoint_count);
  if (!fabric) {
    throw std::invalid_argument("transport backend factory returned null");
  }
  GpuExecutionModel gpu(workload.gpu, workload.endpoint_count);
  contracts::ConservationCounters counters{
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  std::vector<contracts::LayerTiming> layer_timings;
  std::vector<contracts::ScheduleRecord> schedule_records;
  SimTimeNs end_to_end_ns = 0;
  SimTimeNs previous_compute_complete_ns = 0;
  SimTimeNs previous_finish_ns = 0;
  SimTimeNs communication_fence_ns = 0;
  contracts::WorkloadEventId previous_compute_event;
  contracts::WorkloadEventId previous_terminal_event;
  contracts::WorkloadEventId previous_collective_event;
  std::uint64_t next_workload_event = 1;
  std::uint64_t next_plan = 1;
  std::uint64_t next_transaction = 1;
  std::uint64_t next_pdu = 1;

  for (const LayerSpec& layer : workload.layers) {
    SimTimeNs layer_start = 0;
    std::vector<contracts::WorkloadEventId> compute_dependencies;
    if (layer.index != 0) {
      if (layer.start_policy == LayerStartPolicy::AfterPreviousCompute) {
        layer_start = previous_compute_complete_ns;
        compute_dependencies.push_back(previous_compute_event);
      } else {
        layer_start = previous_finish_ns;
        compute_dependencies.push_back(previous_terminal_event);
      }
    }
    const contracts::WorkloadEvent compute =
        contracts::WorkloadEvent::compute(
            contracts::WorkloadEventId(next_workload_event++),
            layer.index,
            compute_dependencies,
            workload.tensor_parallel_group.members.front(),
            layer.compute_duration_ns);
    compute.require_valid();
    const SimTimeNs compute_complete = checked_add(
        layer_start,
        layer.compute_duration_ns,
        "layer compute timestamp overflow");
    SimTimeNs communication_issue = compute_complete;
    SimTimeNs communication_complete = compute_complete;
    const std::uint64_t bytes_before = counters.logical_operation_bytes;
    contracts::WorkloadEventId terminal_event = compute.id;

    if (layer.collective != LayerCollectiveKind::None) {
      communication_issue = std::max(
          compute_complete, communication_fence_ns);
      const contracts::CollectiveKind kind =
          collective_kind(layer.collective);
      const bool reduction_collective =
          kind == contracts::CollectiveKind::AllReduce ||
          kind == contracts::CollectiveKind::ReduceScatter;
      const CollectiveRequest request{
          contracts::CollectivePlanId(next_plan++),
          kind,
          workload.tensor_parallel_group,
          layer.payload_bytes,
          reduction_collective ? contracts::ReductionKind::Sum
                               : contracts::ReductionKind::None,
          workload.collective_algorithm};
      const PlannedCollective planned = schedule_provider.plan(
          request,
          PlannerCapabilities{workload.endpoint_count, true, true});
      const contracts::CollectivePlan& plan = planned.plan;
      schedule_records.push_back(contracts::ScheduleRecord{
          layer.index,
          planned.provenance.source,
          planned.provenance.algorithm,
          planned.provenance.version});
      std::vector<contracts::WorkloadEventId> collective_dependencies{
          compute.id};
      if (previous_collective_event.valid()) {
        collective_dependencies.push_back(previous_collective_event);
      }
      const contracts::WorkloadEvent collective =
          contracts::WorkloadEvent::collective(
              contracts::WorkloadEventId(next_workload_event++),
              layer.index,
              collective_dependencies,
              plan.id);
      collective.require_valid();
      communication_complete = execute_collective(
          plan,
          communication_issue,
          packetizer,
          *fabric,
          gpu,
          next_transaction,
          next_pdu,
          counters);
      communication_fence_ns = communication_complete;
      previous_collective_event = collective.id;
      terminal_event = collective.id;
    }

    const SimTimeNs layer_finish = std::max(
        compute_complete, communication_complete);
    end_to_end_ns = std::max(end_to_end_ns, layer_finish);
    layer_timings.push_back(contracts::LayerTiming{
        layer.index,
        layer.name,
        layer_start,
        compute_complete,
        communication_issue,
        communication_complete,
        layer_finish,
        layer.compute_duration_ns,
        layer_finish - compute_complete,
        counters.logical_operation_bytes - bytes_before});
    previous_compute_complete_ns = compute_complete;
    previous_finish_ns = layer_finish;
    previous_compute_event = compute.id;
    previous_terminal_event = terminal_event;
  }

  const std::vector<contracts::SwitchQueueUtilization> switch_queues =
      fabric->finalize_switch(end_to_end_ns);
  for (const contracts::SwitchQueueUtilization& queue : switch_queues) {
    counters.credits_consumed += queue.credits_consumed;
    counters.credits_returned += queue.credits_returned;
  }
  contracts::SimulationReport report{
      workload.schema_version,
      workload.deterministic_seed,
      end_to_end_ns,
      layer_timings,
      counters,
      fabric->rail_counters(),
      switch_queues,
      schedule_records,
      gpu.utilization()};
  report.require_valid();
  return report;
}

}  // namespace runtime
}  // namespace simai
