#include "simai/runtime/fine_simulator.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace simai {
namespace runtime {
namespace {

enum class PendingKind { Wake, Instruction, LocalMemory, RemoteSemaphore };

struct PendingEvent {
  contracts::SimTimeNs time_ns;
  std::uint64_t sequence;
  PendingKind kind;
  FineInstructionIssue issue;
  contracts::MemoryRequestId memory_request;
};

struct LaterEvent {
  bool operator()(const PendingEvent& lhs, const PendingEvent& rhs) const {
    if (lhs.time_ns != rhs.time_ns) return lhs.time_ns > rhs.time_ns;
    return lhs.sequence > rhs.sequence;
  }
};

using DefinitionKey =
    std::pair<contracts::KernelId, contracts::WorkgroupId>;

contracts::SimTimeNs destination_visible(
    SueTransportBackend& backend,
    const SuePdu& pdu,
    contracts::SimTimeNs issue_ns) {
  const FabricIngressTiming ingress = backend.transmit_ingress(pdu, issue_ns);
  if (!backend.has_switch()) return ingress.ingress_arrival_ns;
  return backend.forward_from_switch(pdu, ingress.ingress_arrival_ns)
      .destination_arrival_ns;
}

bool is_local_memory_issue(const FineInstructionIssue& issue) {
  const contracts::FineGpuInstruction& instruction = issue.instruction;
  const contracts::FineMemoryReference& target =
      instruction.kind == contracts::FineGpuInstructionKind::Load
      ? instruction.source
      : instruction.destination;
  return target.owner == issue.device &&
      target.space != contracts::FineMemorySpace::RemoteHbm;
}

FineInstructionIssue empty_issue() {
  return FineInstructionIssue{};
}

std::uint64_t memory_instruction_bytes(
    const contracts::FineGpuInstruction& instruction) {
  if (instruction.kind == contracts::FineGpuInstructionKind::Load) {
    return instruction.source.length_bytes;
  }
  if (instruction.kind == contracts::FineGpuInstructionKind::Store) {
    return instruction.destination.length_bytes;
  }
  return 0;
}

void require_kernel_owners_in_domain(
    const contracts::FineKernelDefinition& kernel,
    std::uint32_t device_count) {
  for (const contracts::FineWorkgroupDefinition& workgroup :
       kernel.workgroups) {
    for (const contracts::FineWavefrontProgram& wavefront :
         workgroup.wavefronts) {
      for (const contracts::FineGpuOperation& operation :
           wavefront.operations) {
        if ((operation.has_source &&
             operation.source.owner.value() > device_count) ||
            (operation.has_destination &&
             operation.destination.owner.value() > device_count) ||
            (operation.has_semaphore &&
             operation.semaphore_owner.value() > device_count)) {
          throw std::invalid_argument(
              "fine operation memory owner exceeds simulator size");
        }
      }
    }
  }
}

}  // namespace

contracts::ValidationErrors FineSimulationReport::validate() const {
  contracts::ValidationErrors errors;
  if (completed_instructions != issued_instructions) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidReport,
        "fine report instruction counts must conserve");
  }
  for (const FineGraphNodeExecutionRecord& node : graph_nodes) {
    if (!node.node.valid() || node.name.empty() || node.source_node_count == 0 ||
        node.start_ns < node.ready_ns || node.completion_ns < node.start_ns ||
        node.completion_ns > end_to_end_ns) {
      contracts::append_error(
          errors,
          contracts::ValidationCode::InvalidReport,
          "fine graph execution record is invalid");
    }
  }
  for (const FineDispatchRecord& dispatch : dispatches) {
    if (!dispatch.kernel.valid() || !dispatch.workgroup.valid() ||
        !dispatch.device.valid() || !dispatch.compute_unit.valid() ||
        dispatch.completion_ns < dispatch.dispatch_ns ||
        dispatch.completion_ns > end_to_end_ns) {
      contracts::append_error(
          errors,
          contracts::ValidationCode::InvalidReport,
          "fine dispatch record is invalid");
    }
  }
  std::uint64_t rail_pdus = 0;
  std::uint64_t rail_payload = 0;
  std::uint64_t rail_wire = 0;
  for (const contracts::RailUtilization& rail : rails) {
    rail_pdus += rail.pdus;
    rail_payload += rail.pdu_payload_bytes;
    rail_wire += rail.wire_bytes;
  }
  if (rail_pdus != remote_command_pdus + remote_response_pdus ||
      rail_payload != remote_payload_bytes || rail_wire != remote_wire_bytes) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidReport,
        "fine remote counters must match rail counters");
  }
  return errors;
}

void FineSimulationReport::require_valid() const {
  contracts::require_no_errors(validate(), "FineSimulationReport");
}

contracts::ValidationErrors FineSimulatorConfig::validate() const {
  contracts::ValidationErrors errors = scheduler.validate();
  const contracts::ValidationErrors wavefront_errors = wavefront.validate();
  errors.insert(errors.end(), wavefront_errors.begin(), wavefront_errors.end());
  const contracts::ValidationErrors memory_errors = local_memory.validate();
  errors.insert(errors.end(), memory_errors.begin(), memory_errors.end());
  if (scheduler.device_count != wavefront.device_count ||
      scheduler.device_count != local_memory.device_count ||
      scheduler.compute_units_per_device !=
          wavefront.compute_units_per_device ||
      !semaphore_register_buffer.valid()) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidResourceRequirement,
        "fine simulator subsystem dimensions must agree");
  }
  return errors;
}

void FineSimulatorConfig::require_valid() const {
  contracts::require_no_errors(validate(), "FineSimulatorConfig");
}

FineSimulationReport FineScaleUpSimulator::run(
    const std::vector<contracts::FineKernelDefinition>& kernels,
    const FineSimulatorConfig& config) const {
  const NativeSueTransportBackendFactory factory;
  return run(kernels, config, factory);
}

FineSimulationReport FineScaleUpSimulator::run(
    const std::vector<contracts::FineKernelDefinition>& kernels,
    const FineSimulatorConfig& config,
    const SueTransportBackendFactory& backend_factory) const {
  if (kernels.empty()) {
    throw std::invalid_argument("fine simulation requires at least one kernel");
  }
  FineGraphExecutionPlan plan;
  for (std::size_t index = 0; index < kernels.size(); ++index) {
    plan.nodes.push_back(FineGraphExecutionNode{
        contracts::WorkloadEventId(index + 1),
        "direct_kernel_" + std::to_string(index + 1),
        {contracts::WorkloadEventId(index + 1)},
        {},
        {kernels[index]}});
  }
  return run(plan, config, backend_factory);
}

FineSimulationReport FineScaleUpSimulator::run(
    const FineGraphExecutionPlan& plan,
    const FineSimulatorConfig& config) const {
  const NativeSueTransportBackendFactory factory;
  return run(plan, config, factory);
}

FineSimulationReport FineScaleUpSimulator::run(
    const FineGraphExecutionPlan& plan,
    const FineSimulatorConfig& config,
    const SueTransportBackendFactory& backend_factory) const {
  config.require_valid();
  plan.require_valid();
  FineGpuScheduler scheduler(config.scheduler);
  FineWavefrontEngine engine(config.wavefront);
  FineLocalMemorySystem memory(config.local_memory);
  FineSynchronizationEngine synchronization;
  FineRemotePacketizer packetizer(
      config.link, config.semaphore_register_buffer);
  std::unique_ptr<SueTransportBackend> transport =
      backend_factory.create(config.link, config.scheduler.device_count);

  std::map<DefinitionKey, const contracts::FineWorkgroupDefinition*>
      definitions;
  std::map<contracts::WorkloadEventId, const FineGraphExecutionNode*>
      execution_nodes;
  std::map<contracts::KernelId, contracts::WorkloadEventId> kernel_nodes;
  std::map<contracts::KernelId, std::size_t> remaining_workgroups;
  std::map<contracts::WorkloadEventId, std::size_t> remaining_kernels;
  std::map<contracts::WorkloadEventId, std::size_t> unresolved_dependencies;
  std::map<contracts::WorkloadEventId,
           std::vector<contracts::WorkloadEventId>> dependents;
  std::map<contracts::WorkloadEventId, int> graph_states;
  FineSimulationReport report{};
  std::map<contracts::WorkgroupInstanceId, std::size_t> dispatch_records;
  std::map<contracts::WorkloadEventId, std::size_t> graph_records;
  std::map<contracts::KernelId, contracts::SimTimeNs> kernel_trace_origins;
  auto admit = [&](const FineWorkgroupDispatch& dispatch,
                   contracts::SimTimeNs now_ns) {
    const auto found = definitions.find(
        DefinitionKey(dispatch.kernel, dispatch.workgroup));
    if (found == definitions.end()) {
      throw std::logic_error("fine dispatch definition is missing");
    }
    engine.admit(
        dispatch,
        *found->second,
        now_ns,
        kernel_trace_origins.at(dispatch.kernel));
    report.dispatches.push_back(FineDispatchRecord{
        dispatch.kernel_instance,
        dispatch.workgroup_instance,
        dispatch.device,
        dispatch.compute_unit,
        now_ns,
        0});
    dispatch_records[dispatch.workgroup_instance] =
        report.dispatches.size() - 1;
  };

  for (const FineGraphExecutionNode& node : plan.nodes) {
    execution_nodes.emplace(node.id, &node);
    unresolved_dependencies.emplace(node.id, node.dependencies.size());
    graph_states.emplace(node.id, 0);
    remaining_kernels.emplace(node.id, node.kernels.size());
    report.graph_nodes.push_back(FineGraphNodeExecutionRecord{
        node.id,
        node.name,
        node.source_nodes.size(),
        node.kernels.size(),
        0,
        0,
        0});
    graph_records.emplace(node.id, report.graph_nodes.size() - 1);
    for (contracts::WorkloadEventId dependency : node.dependencies) {
      dependents[dependency].push_back(node.id);
    }
    for (const contracts::FineKernelDefinition& kernel : node.kernels) {
      kernel.require_valid();
      if (kernel.device.value() > config.scheduler.device_count) {
        throw std::invalid_argument("fine kernel device exceeds simulator size");
      }
      require_kernel_owners_in_domain(
          kernel, config.scheduler.device_count);
      if (!kernel_nodes.emplace(kernel.id, node.id).second) {
        throw std::invalid_argument(
            "fine simulation kernel definitions must be globally unique");
      }
      remaining_workgroups.emplace(kernel.id, kernel.workgroups.size());
      for (const contracts::FineWorkgroupDefinition& workgroup :
           kernel.workgroups) {
        if (!definitions.insert(
                std::make_pair(
                    DefinitionKey(kernel.id, workgroup.id), &workgroup))
                 .second) {
          throw std::invalid_argument(
              "fine simulation kernel/workgroup definitions must be unique");
        }
      }
    }
  }

  std::priority_queue<PendingEvent, std::vector<PendingEvent>, LaterEvent>
      events;
  std::uint64_t next_event = 1;
  std::uint64_t next_memory_request = 1;
  std::uint64_t next_transaction = 1;
  std::uint64_t next_pdu = 1;
  contracts::SimTimeNs now_ns = 0;
  std::queue<contracts::WorkloadEventId> ready_nodes;
  std::size_t completed_graph_nodes = 0;
  for (const auto& entry : unresolved_dependencies) {
    if (entry.second == 0) ready_nodes.push(entry.first);
  }

  auto schedule = [&](contracts::SimTimeNs time_ns,
                      PendingKind kind,
                      const FineInstructionIssue& issue,
                      contracts::MemoryRequestId request) {
    events.push(PendingEvent{time_ns, next_event++, kind, issue, request});
  };
  auto complete_sync = [&](const std::vector<FineSynchronizationCompletion>&
                               completions) {
    for (const FineSynchronizationCompletion& completion : completions) {
      engine.complete_instruction(
          completion.wavefront_instance, completion.instruction, now_ns);
      ++report.completed_instructions;
    }
  };
  auto complete_graph_node = [&](contracts::WorkloadEventId node_id,
                                 contracts::SimTimeNs completion_ns) {
    if (graph_states.at(node_id) != 1) {
      throw std::logic_error("fine graph node completion state is invalid");
    }
    graph_states[node_id] = 2;
    report.graph_nodes[graph_records.at(node_id)].completion_ns = completion_ns;
    ++completed_graph_nodes;
    for (contracts::WorkloadEventId dependent : dependents[node_id]) {
      std::size_t& unresolved = unresolved_dependencies.at(dependent);
      if (unresolved == 0) {
        throw std::logic_error("fine graph dependency count underflow");
      }
      --unresolved;
      if (unresolved == 0) {
        report.graph_nodes[graph_records.at(dependent)].ready_ns = completion_ns;
        ready_nodes.push(dependent);
      }
    }
  };
  auto launch_ready_nodes = [&]() {
    bool progressed = false;
    while (!ready_nodes.empty()) {
      const contracts::WorkloadEventId node_id = ready_nodes.front();
      ready_nodes.pop();
      if (graph_states.at(node_id) != 0) {
        throw std::logic_error("fine graph node was made ready twice");
      }
      graph_states[node_id] = 1;
      FineGraphNodeExecutionRecord& record =
          report.graph_nodes[graph_records.at(node_id)];
      record.start_ns = now_ns;
      const FineGraphExecutionNode& node = *execution_nodes.at(node_id);
      for (const contracts::FineKernelDefinition& kernel : node.kernels) {
        if (!kernel_trace_origins.emplace(kernel.id, now_ns).second) {
          throw std::logic_error("fine kernel trace origin was assigned twice");
        }
        for (const FineWorkgroupDispatch& dispatch : scheduler.submit(kernel)) {
          admit(dispatch, now_ns);
        }
      }
      if (node.kernels.empty()) complete_graph_node(node_id, now_ns);
      progressed = true;
    }
    return progressed;
  };
  auto retire_workgroups = [&]() {
    for (const FineWorkgroupCompletion& completion :
         engine.take_completed_workgroups()) {
      report.dispatches[dispatch_records.at(completion.workgroup_instance)]
          .completion_ns = completion.completion_ns;
      for (const FineWorkgroupDispatch& dispatch :
           scheduler.complete(completion.workgroup_instance)) {
        admit(dispatch, completion.completion_ns);
      }
      std::size_t& workgroups = remaining_workgroups.at(completion.kernel);
      if (workgroups == 0) {
        throw std::logic_error("fine kernel workgroup count underflow");
      }
      --workgroups;
      if (workgroups == 0) {
        const contracts::WorkloadEventId node_id =
            kernel_nodes.at(completion.kernel);
        std::size_t& kernels = remaining_kernels.at(node_id);
        if (kernels == 0) {
          throw std::logic_error("fine graph kernel count underflow");
        }
        --kernels;
        if (kernels == 0) {
          complete_graph_node(node_id, completion.completion_ns);
        }
      }
    }
  };

  while (completed_graph_nodes != plan.nodes.size() || !scheduler.drained() ||
         !engine.drained() || !events.empty()) {
    launch_ready_nodes();
    const std::vector<FineInstructionIssue> issues = engine.issue_ready(now_ns);
    if (!issues.empty()) {
      schedule(
          now_ns + config.wavefront.issue_latency_ns,
          PendingKind::Wake,
          empty_issue(),
          contracts::MemoryRequestId());
    }
    for (const FineInstructionIssue& issue : issues) {
      ++report.issued_instructions;
      report.trace.push_back(FineExecutionTraceRecord{
          now_ns,
          "issue",
          issue.device,
          issue.compute_unit,
          issue.workgroup_instance,
          issue.wavefront_instance,
          issue.instruction.id});
      const contracts::FineGpuInstructionKind kind = issue.instruction.kind;
      if (kind == contracts::FineGpuInstructionKind::TraceWaitUntil) {
        ++report.trace_wait_instructions;
      }
      if (kind == contracts::FineGpuInstructionKind::Reduce ||
          kind == contracts::FineGpuInstructionKind::TraceWaitUntil) {
        schedule(
            issue.modeled_completion_ns,
            PendingKind::Instruction,
            issue,
            contracts::MemoryRequestId());
      } else if (kind == contracts::FineGpuInstructionKind::Load ||
                 kind == contracts::FineGpuInstructionKind::Store) {
        const contracts::MemoryRequestId request_id(next_memory_request++);
        if (is_local_memory_issue(issue)) {
          const contracts::FineMemoryRequest request =
              lower_local_memory_request(request_id, issue);
          const FineMemorySubmitResult result = memory.submit(request);
          if (!result.accepted) {
            throw std::runtime_error(
                "fine runtime local-memory backpressure requires retry scheduling");
          }
          ++report.local_memory_requests;
          report.local_memory_bytes +=
              memory_instruction_bytes(issue.instruction);
          schedule(
              result.completion.completion_ns,
              PendingKind::LocalMemory,
              issue,
              request_id);
        } else {
          const contracts::FineRemoteRequest request =
              lower_remote_request(request_id, issue);
          const FineRemotePacketBatch batch = packetizer.packetize(
              request, next_transaction, next_pdu);
          contracts::SimTimeNs command_complete = now_ns;
          for (const SuePdu& pdu : batch.command_pdus) {
            command_complete = std::max(
                command_complete,
                destination_visible(*transport, pdu, now_ns));
            ++report.remote_command_pdus;
            report.remote_payload_bytes += pdu.payload_bytes;
            report.remote_wire_bytes += pdu.wire_bytes;
          }
          contracts::SimTimeNs completion_ns = command_complete;
          for (const SuePdu& pdu : batch.response_pdus) {
            completion_ns = std::max(
                completion_ns,
                destination_visible(*transport, pdu, command_complete));
            ++report.remote_response_pdus;
            report.remote_payload_bytes += pdu.payload_bytes;
            report.remote_wire_bytes += pdu.wire_bytes;
          }
          ++report.remote_requests;
          report.remote_memory_bytes +=
              memory_instruction_bytes(issue.instruction);
          schedule(
              completion_ns,
              PendingKind::Instruction,
              issue,
              request_id);
        }
      } else if (
          kind == contracts::FineGpuInstructionKind::SemaphoreAcquire ||
          kind == contracts::FineGpuInstructionKind::SemaphoreRelease) {
        if (issue.instruction.semaphore_owner == issue.device) {
          complete_sync(synchronization.process_semaphore(issue));
        } else {
          const contracts::FineRemoteRequest request = lower_remote_request(
              contracts::MemoryRequestId(next_memory_request++), issue);
          const FineRemotePacketBatch batch = packetizer.packetize(
              request, next_transaction, next_pdu);
          contracts::SimTimeNs command_complete = now_ns;
          for (const SuePdu& pdu : batch.command_pdus) {
            command_complete = std::max(
                command_complete,
                destination_visible(*transport, pdu, now_ns));
            ++report.remote_command_pdus;
            report.remote_payload_bytes += pdu.payload_bytes;
            report.remote_wire_bytes += pdu.wire_bytes;
          }
          contracts::SimTimeNs completion_ns = command_complete;
          for (const SuePdu& pdu : batch.response_pdus) {
            completion_ns = std::max(
                completion_ns,
                destination_visible(*transport, pdu, command_complete));
            ++report.remote_response_pdus;
            report.remote_payload_bytes += pdu.payload_bytes;
            report.remote_wire_bytes += pdu.wire_bytes;
          }
          ++report.remote_requests;
          schedule(
              completion_ns,
              PendingKind::RemoteSemaphore,
              issue,
              request.id);
        }
      }
    }
    retire_workgroups();
    const bool graph_progress = launch_ready_nodes();

    if (events.empty()) {
      if (completed_graph_nodes == plan.nodes.size() && scheduler.drained() &&
          engine.drained()) {
        break;
      }
      if (graph_progress) continue;
      report.diagnostics = synchronization.blocked_state_diagnostics();
      throw std::runtime_error("fine simulation reached a no-progress state");
    }
    const PendingEvent event = events.top();
    events.pop();
    now_ns = event.time_ns;
    if (event.kind == PendingKind::Wake) continue;
    if (event.kind == PendingKind::LocalMemory) {
      memory.complete(event.memory_request, now_ns);
      engine.complete_instruction(
          event.issue.wavefront_instance,
          event.issue.instruction.id,
          now_ns);
      ++report.completed_instructions;
    } else if (event.kind == PendingKind::Instruction) {
      engine.complete_instruction(
          event.issue.wavefront_instance,
          event.issue.instruction.id,
          now_ns);
      ++report.completed_instructions;
    } else if (event.kind == PendingKind::RemoteSemaphore) {
      complete_sync(synchronization.process_semaphore(event.issue));
    }
    report.trace.push_back(FineExecutionTraceRecord{
        now_ns,
        "complete",
        event.issue.device,
        event.issue.compute_unit,
        event.issue.workgroup_instance,
        event.issue.wavefront_instance,
        event.issue.instruction.id});
    retire_workgroups();
  }
  report.end_to_end_ns = now_ns;
  report.rails = transport->rail_counters();
  report.switch_queues = transport->finalize_switch(now_ns);
  report.memory_tiers = config.local_memory.tiers;
  report.diagnostics = synchronization.blocked_state_diagnostics();
  if (!memory.drained() || !synchronization.drained()) {
    throw std::runtime_error("fine simulation ended with undrained subsystems");
  }
  report.require_valid();
  return report;
}

std::string format_fine_report(const FineSimulationReport& report) {
  report.require_valid();
  std::ostringstream output;
  output << "fine_schema_version=1\n"
         << "end_to_end_ns=" << report.end_to_end_ns << '\n'
         << "issued_instructions=" << report.issued_instructions << '\n'
         << "completed_instructions=" << report.completed_instructions << '\n'
         << "trace_wait_instructions=" << report.trace_wait_instructions << '\n'
         << "local_memory_requests=" << report.local_memory_requests << '\n'
         << "local_memory_bytes=" << report.local_memory_bytes << '\n'
         << "remote_requests=" << report.remote_requests << '\n'
         << "remote_memory_bytes=" << report.remote_memory_bytes << '\n'
         << "remote_command_pdus=" << report.remote_command_pdus << '\n'
         << "remote_response_pdus=" << report.remote_response_pdus << '\n'
         << "remote_payload_bytes=" << report.remote_payload_bytes << '\n'
         << "remote_wire_bytes=" << report.remote_wire_bytes << '\n';
  for (const FineLocalMemoryTierConfig& tier : report.memory_tiers) {
    output << "memory_tier=" << static_cast<int>(tier.space)
           << ",base_latency_ns=" << tier.base_latency_ns
           << ",bytes_per_ns=" << tier.bytes_per_ns
           << ",queue_capacity=" << tier.queue_capacity
           << ",max_outstanding=" << tier.max_outstanding << '\n';
  }
  for (const FineGraphNodeExecutionRecord& node : report.graph_nodes) {
    output << "graph_node=id:" << node.node.value()
           << ",name:" << node.name
           << ",source_nodes:" << node.source_node_count
           << ",kernels:" << node.kernel_count
           << ",ready_ns:" << node.ready_ns
           << ",start_ns:" << node.start_ns
           << ",complete_ns:" << node.completion_ns << '\n';
  }
  for (const FineDispatchRecord& dispatch : report.dispatches) {
    output << "dispatch=kernel:" << dispatch.kernel.value()
           << ",workgroup:" << dispatch.workgroup.value()
           << ",device:" << dispatch.device.value()
           << ",cu:" << dispatch.compute_unit.value()
           << ",start_ns:" << dispatch.dispatch_ns
           << ",complete_ns:" << dispatch.completion_ns << '\n';
  }
  for (const contracts::RailUtilization& rail : report.rails) {
    output << "rail=" << rail.rail_index << ",pdus=" << rail.pdus
           << ",payload_bytes=" << rail.pdu_payload_bytes
           << ",wire_bytes=" << rail.wire_bytes
           << ",busy_ns=" << rail.busy_time_ns << '\n';
  }
  return output.str();
}

}  // namespace runtime
}  // namespace simai
