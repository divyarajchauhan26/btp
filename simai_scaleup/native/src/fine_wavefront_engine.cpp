#include "simai/runtime/fine_wavefront_engine.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace simai {
namespace runtime {
namespace {

bool is_memory_instruction(contracts::FineGpuInstructionKind kind) {
  return kind == contracts::FineGpuInstructionKind::Load ||
      kind == contracts::FineGpuInstructionKind::Store;
}

contracts::SimTimeNs checked_reduce_completion(
    contracts::SimTimeNs issue_ns,
    std::uint64_t cycles,
    contracts::SimTimeNs cycle_ns) {
  if (cycles > std::numeric_limits<contracts::SimTimeNs>::max() / cycle_ns) {
    throw std::overflow_error("fine Reduce duration overflow");
  }
  const contracts::SimTimeNs duration = cycles * cycle_ns;
  if (issue_ns > std::numeric_limits<contracts::SimTimeNs>::max() - duration) {
    throw std::overflow_error("fine Reduce completion timestamp overflow");
  }
  return issue_ns + duration;
}

contracts::SimTimeNs checked_trace_completion(
    contracts::SimTimeNs origin_ns,
    contracts::SimTimeNs offset_ns,
    contracts::SimTimeNs issue_ns) {
  if (origin_ns >
      std::numeric_limits<contracts::SimTimeNs>::max() - offset_ns) {
    throw std::overflow_error("fine trace timestamp overflow");
  }
  return std::max(issue_ns, origin_ns + offset_ns);
}

}  // namespace

contracts::ValidationErrors FineWavefrontEngineConfig::validate() const {
  contracts::ValidationErrors errors = instruction_generator.validate();
  if (device_count == 0 || compute_units_per_device == 0 || issue_width == 0 ||
      issue_latency_ns == 0 || reduce_cycle_ns == 0) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidResourceRequirement,
        "fine wavefront engine resource and timing values must be nonzero");
  }
  return errors;
}

void FineWavefrontEngineConfig::require_valid() const {
  contracts::require_no_errors(validate(), "FineWavefrontEngineConfig");
}

FineWavefrontEngine::FineWavefrontEngine(
    const FineWavefrontEngineConfig& config)
    : config_(config),
      compute_units_(),
      workgroups_(),
      wavefronts_(),
      wavefront_locations_(),
      completed_workgroups_(),
      next_wavefront_instance_(1),
      next_instruction_id_(1) {
  config_.require_valid();
  const std::uint64_t compute_unit_count =
      static_cast<std::uint64_t>(config_.device_count) *
      config_.compute_units_per_device;
  if (compute_unit_count > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument("fine wavefront engine CU count is too large");
  }
  compute_units_.assign(
      static_cast<std::size_t>(compute_unit_count),
      ComputeUnitState{{}, 0, 0});
}

std::size_t FineWavefrontEngine::compute_unit_index(
    contracts::DeviceId device,
    contracts::ComputeUnitId compute_unit) const {
  if (!device.valid() || device.value() > config_.device_count ||
      !compute_unit.valid() ||
      compute_unit.value() > config_.compute_units_per_device) {
    throw std::invalid_argument("fine wavefront engine CU identity is out of range");
  }
  return static_cast<std::size_t>(
      (device.value() - 1) * config_.compute_units_per_device +
      compute_unit.value() - 1);
}

void FineWavefrontEngine::admit(
    const FineWorkgroupDispatch& dispatch,
    const contracts::FineWorkgroupDefinition& workgroup,
    contracts::SimTimeNs ready_ns) {
  admit(dispatch, workgroup, ready_ns, ready_ns);
}

void FineWavefrontEngine::admit(
    const FineWorkgroupDispatch& dispatch,
    const contracts::FineWorkgroupDefinition& workgroup,
    contracts::SimTimeNs ready_ns,
    contracts::SimTimeNs trace_origin_ns) {
  workgroup.require_valid();
  if (trace_origin_ns > ready_ns) {
    throw std::invalid_argument(
        "fine trace origin may not follow workgroup readiness");
  }
  if (!dispatch.kernel_instance.valid() ||
      !dispatch.workgroup_instance.valid() ||
      dispatch.workgroup != workgroup.id ||
      dispatch.residency_slots != workgroup.resources.residency_slots ||
      workgroups_.count(dispatch.workgroup_instance) != 0) {
    throw std::invalid_argument("fine workgroup dispatch does not match definition");
  }
  const std::size_t unit_index =
      compute_unit_index(dispatch.device, dispatch.compute_unit);
  WorkgroupState state{dispatch, workgroup.id, {}, false};
  state.wavefronts.reserve(workgroup.wavefronts.size());
  for (const contracts::FineWavefrontProgram& definition :
       workgroup.wavefronts) {
    const contracts::WavefrontInstanceId instance(next_wavefront_instance_++);
    WavefrontState wavefront_state{
        instance,
        definition.id,
        definition.operations,
        0,
        std::unique_ptr<FineInstructionStream>(),
        trace_origin_ns,
        ready_ns,
        false,
        contracts::GpuInstructionId(),
        0,
        0,
        {},
        0,
        0,
        false,
        contracts::GpuInstructionId(),
        0,
        false,
        contracts::FineStallReason::None};
    const std::size_t wavefront_index = state.wavefronts.size();
    state.wavefronts.push_back(instance);
    wavefronts_.insert(std::make_pair(instance, std::move(wavefront_state)));
    wavefront_locations_.insert(std::make_pair(
        instance,
        WavefrontLocation{dispatch.workgroup_instance, wavefront_index}));
    compute_units_[unit_index].wavefronts.push_back(instance);
  }
  workgroups_.insert(
      std::make_pair(dispatch.workgroup_instance, std::move(state)));
}

FineWavefrontEngine::WavefrontState& FineWavefrontEngine::wavefront(
    contracts::WavefrontInstanceId instance) {
  const auto found = wavefronts_.find(instance);
  if (found == wavefronts_.end()) {
    throw std::invalid_argument("fine wavefront instance is unknown");
  }
  return found->second;
}

const FineWavefrontEngine::WavefrontState& FineWavefrontEngine::wavefront(
    contracts::WavefrontInstanceId instance) const {
  const auto found = wavefronts_.find(instance);
  if (found == wavefronts_.end()) {
    throw std::invalid_argument("fine wavefront instance is unknown");
  }
  return found->second;
}

bool FineWavefrontEngine::prepare_stream(WavefrontState& state) {
  while (!state.completed) {
    if (state.stream) {
      if (state.stream->emitted_count() < state.stream->instruction_count()) {
        return true;
      }
      if (state.instruction_pending || state.waitcnt_pending) {
        return false;
      }
      state.stream.reset();
      ++state.operation_index;
    }
    if (state.operation_index == state.operations.size()) {
      if (!state.outstanding_instructions.empty()) {
        state.stall_reason = contracts::FineStallReason::MemoryBackpressure;
        return false;
      }
      state.completed = true;
      state.stall_reason = contracts::FineStallReason::None;
      return false;
    }
    const contracts::FineGpuOperation& operation =
        state.operations[state.operation_index];
    if (operation.kind == contracts::FineGpuOperationKind::WorkgroupBarrier ||
        operation.kind == contracts::FineGpuOperationKind::KernelBarrier) {
      throw std::logic_error(
          "fine barrier execution belongs to the synchronization phase");
    }
    std::unique_ptr<FineInstructionStream> stream(
        new FineInstructionStream(
            operation,
            config_.instruction_generator,
            contracts::GpuInstructionId(next_instruction_id_)));
    const std::uint64_t count = stream->instruction_count();
    if (count == 0) {
      ++state.operation_index;
      continue;
    }
    if (next_instruction_id_ >
        std::numeric_limits<std::uint64_t>::max() - count) {
      throw std::overflow_error("fine dynamic instruction identity overflow");
    }
    next_instruction_id_ += count;
    state.stream = std::move(stream);
  }
  return false;
}

contracts::WavefrontInstanceId FineWavefrontEngine::select_ready_wavefront(
    ComputeUnitState& compute_unit, contracts::SimTimeNs now_ns) {
  if (compute_unit.wavefronts.empty()) return contracts::WavefrontInstanceId();
  for (std::size_t offset = 0; offset < compute_unit.wavefronts.size(); ++offset) {
    const std::size_t index =
        (compute_unit.next_wavefront + offset) % compute_unit.wavefronts.size();
    WavefrontState& candidate = wavefront(compute_unit.wavefronts[index]);
    if (now_ns >= candidate.ready_ns && !candidate.completed &&
        !candidate.instruction_pending && !candidate.waitcnt_pending &&
        prepare_stream(candidate)) {
      compute_unit.next_wavefront =
          (index + 1) % compute_unit.wavefronts.size();
      return candidate.instance;
    }
  }
  return contracts::WavefrontInstanceId();
}

std::vector<FineInstructionIssue> FineWavefrontEngine::issue_ready(
    contracts::SimTimeNs now_ns) {
  std::vector<FineInstructionIssue> issues;
  for (ComputeUnitState& compute_unit : compute_units_) {
    if (now_ns < compute_unit.next_issue_ns) continue;
    std::uint32_t issued = 0;
    while (issued < config_.issue_width) {
      const contracts::WavefrontInstanceId selected =
          select_ready_wavefront(compute_unit, now_ns);
      if (!selected.valid()) break;
      WavefrontState& state = wavefront(selected);
      contracts::FineGpuInstruction instruction{};
      if (!state.stream || !state.stream->next(instruction)) {
        throw std::logic_error("fine ready wavefront did not produce instruction");
      }
      const WavefrontLocation location = wavefront_locations_.at(selected);
      WorkgroupState& workgroup = workgroups_.at(location.workgroup);
      if (instruction.kind == contracts::FineGpuInstructionKind::Waitcnt) {
        if (state.outstanding_instructions.size() >
            instruction.wait_threshold) {
          state.waitcnt_pending = true;
          state.waitcnt_instruction = instruction.id;
          state.wait_threshold = instruction.wait_threshold;
          state.stall_reason = contracts::FineStallReason::Waitcnt;
        } else {
          state.stall_reason = contracts::FineStallReason::None;
          prepare_stream(state);
          queue_workgroup_completion(workgroup, now_ns);
        }
      } else {
        if (is_memory_instruction(instruction.kind)) {
          const bool inserted = state.outstanding_instructions.insert(
              std::make_pair(
                  instruction.id,
                  OutstandingInstruction{now_ns, instruction.kind})).second;
          if (!inserted) {
            throw std::logic_error("duplicate fine outstanding instruction");
          }
          if (instruction.kind == contracts::FineGpuInstructionKind::Load) {
            ++state.outstanding_loads;
          } else {
            ++state.outstanding_stores;
          }
          state.stall_reason = contracts::FineStallReason::None;
        } else {
          state.instruction_pending = true;
          state.pending_instruction = instruction.id;
          state.pending_issue_ns = now_ns;
          state.pending_completion_ns =
              instruction.kind == contracts::FineGpuInstructionKind::Reduce
              ? checked_reduce_completion(
                    now_ns, instruction.reduce_cycles, config_.reduce_cycle_ns)
              : instruction.kind ==
                      contracts::FineGpuInstructionKind::TraceWaitUntil
              ? checked_trace_completion(
                    state.trace_origin_ns,
                    instruction.trace_timestamp_ns,
                    now_ns)
              : now_ns;
          state.stall_reason =
              instruction.kind ==
                      contracts::FineGpuInstructionKind::SemaphoreAcquire ||
                  instruction.kind ==
                      contracts::FineGpuInstructionKind::SemaphoreRelease
              ? contracts::FineStallReason::Semaphore
              : instruction.kind ==
                      contracts::FineGpuInstructionKind::TraceWaitUntil
              ? contracts::FineStallReason::TraceReplay
              : contracts::FineStallReason::InstructionIssue;
        }
        const bool modeled_completion =
            instruction.kind == contracts::FineGpuInstructionKind::Reduce ||
            instruction.kind ==
                contracts::FineGpuInstructionKind::TraceWaitUntil;
        issues.push_back(FineInstructionIssue{
            workgroup.dispatch.kernel_instance,
            workgroup.dispatch.workgroup_instance,
            selected,
            state.definition,
            workgroup.dispatch.device,
            workgroup.dispatch.compute_unit,
            now_ns,
            modeled_completion,
            modeled_completion ? state.pending_completion_ns : 0,
            instruction});
      }
      ++issued;
    }
    if (issued != 0) {
      if (now_ns > std::numeric_limits<contracts::SimTimeNs>::max() -
              config_.issue_latency_ns) {
        throw std::overflow_error("fine CU issue timestamp overflow");
      }
      compute_unit.next_issue_ns = now_ns + config_.issue_latency_ns;
    }
  }
  return issues;
}

void FineWavefrontEngine::queue_workgroup_completion(
    WorkgroupState& workgroup,
    contracts::SimTimeNs completion_ns) {
  if (workgroup.completion_queued) return;
  for (const contracts::WavefrontInstanceId instance : workgroup.wavefronts) {
    if (!wavefront(instance).completed) return;
  }
  workgroup.completion_queued = true;
  completed_workgroups_.push_back(FineWorkgroupCompletion{
      workgroup.dispatch.kernel_instance,
      workgroup.dispatch.workgroup_instance,
      workgroup.dispatch.kernel,
      workgroup.definition,
      workgroup.dispatch.device,
      workgroup.dispatch.compute_unit,
      completion_ns});
}

void FineWavefrontEngine::complete_instruction(
    contracts::WavefrontInstanceId wavefront_instance,
    contracts::GpuInstructionId instruction,
    contracts::SimTimeNs completion_ns) {
  WavefrontState& state = wavefront(wavefront_instance);
  const auto outstanding = state.outstanding_instructions.find(instruction);
  if (outstanding != state.outstanding_instructions.end()) {
    if (completion_ns < outstanding->second.issue_ns) {
      throw std::invalid_argument(
          "fine memory completion precedes its issue time");
    }
    if (outstanding->second.kind == contracts::FineGpuInstructionKind::Load) {
      if (state.outstanding_loads == 0) {
        throw std::logic_error("fine outstanding Load accounting underflow");
      }
      --state.outstanding_loads;
    } else {
      if (state.outstanding_stores == 0) {
        throw std::logic_error("fine outstanding Store accounting underflow");
      }
      --state.outstanding_stores;
    }
    state.outstanding_instructions.erase(outstanding);
    if (state.waitcnt_pending &&
        state.outstanding_instructions.size() <= state.wait_threshold) {
      state.waitcnt_pending = false;
      state.waitcnt_instruction = contracts::GpuInstructionId();
      state.wait_threshold = 0;
      state.stall_reason = contracts::FineStallReason::None;
    }
    prepare_stream(state);
    const WavefrontLocation location =
        wavefront_locations_.at(wavefront_instance);
    queue_workgroup_completion(
        workgroups_.at(location.workgroup), completion_ns);
    return;
  }
  if (!state.instruction_pending || state.pending_instruction != instruction ||
      completion_ns < state.pending_completion_ns) {
    throw std::invalid_argument("fine instruction completion does not match pending state");
  }
  state.instruction_pending = false;
  state.pending_instruction = contracts::GpuInstructionId();
  state.pending_completion_ns = 0;
  state.stall_reason = contracts::FineStallReason::None;
  prepare_stream(state);
  const WavefrontLocation location = wavefront_locations_.at(wavefront_instance);
  queue_workgroup_completion(workgroups_.at(location.workgroup), completion_ns);
}

void FineWavefrontEngine::erase_workgroup(
    contracts::WorkgroupInstanceId workgroup_instance) {
  const auto found = workgroups_.find(workgroup_instance);
  if (found == workgroups_.end()) {
    throw std::logic_error("completed fine workgroup state is missing");
  }
  const WorkgroupState& workgroup = found->second;
  const std::size_t unit_index = compute_unit_index(
      workgroup.dispatch.device, workgroup.dispatch.compute_unit);
  ComputeUnitState& compute_unit = compute_units_[unit_index];
  for (const contracts::WavefrontInstanceId instance : workgroup.wavefronts) {
    compute_unit.wavefronts.erase(
        std::remove(
            compute_unit.wavefronts.begin(),
            compute_unit.wavefronts.end(),
            instance),
        compute_unit.wavefronts.end());
    wavefront_locations_.erase(instance);
    wavefronts_.erase(instance);
  }
  if (compute_unit.wavefronts.empty()) {
    compute_unit.next_wavefront = 0;
  } else {
    compute_unit.next_wavefront %= compute_unit.wavefronts.size();
  }
  workgroups_.erase(found);
}

std::vector<FineWorkgroupCompletion>
FineWavefrontEngine::take_completed_workgroups() {
  std::vector<FineWorkgroupCompletion> completed;
  while (!completed_workgroups_.empty()) {
    const FineWorkgroupCompletion completion = completed_workgroups_.front();
    completed_workgroups_.pop_front();
    completed.push_back(completion);
    erase_workgroup(completion.workgroup_instance);
  }
  return completed;
}

FineWavefrontSnapshot FineWavefrontEngine::snapshot(
    contracts::WavefrontInstanceId wavefront_instance) const {
  const WavefrontState& state = wavefront(wavefront_instance);
  return FineWavefrontSnapshot{
      state.instance,
      state.definition,
      state.operation_index,
      state.instruction_pending,
      state.outstanding_instructions.size(),
      state.outstanding_loads,
      state.outstanding_stores,
      state.waitcnt_pending,
      state.wait_threshold,
      state.completed,
      state.stall_reason};
}

bool FineWavefrontEngine::drained() const {
  return workgroups_.empty() && wavefronts_.empty() &&
      wavefront_locations_.empty() && completed_workgroups_.empty();
}

}  // namespace runtime
}  // namespace simai
