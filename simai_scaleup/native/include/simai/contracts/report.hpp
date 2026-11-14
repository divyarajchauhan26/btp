#ifndef SIMAI_CONTRACTS_REPORT_HPP
#define SIMAI_CONTRACTS_REPORT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "simai/contracts/ids.hpp"
#include "simai/contracts/validation.hpp"

namespace simai {
namespace contracts {

struct LayerTiming {
  std::uint32_t layer_index;
  std::string name;
  SimTimeNs start_ns;
  SimTimeNs compute_complete_ns;
  SimTimeNs communication_issue_ns;
  SimTimeNs communication_complete_ns;
  SimTimeNs finish_ns;
  SimTimeNs compute_duration_ns;
  SimTimeNs exposed_communication_ns;
  std::uint64_t transferred_payload_bytes;
};

struct ConservationCounters {
  std::uint64_t issued_operations;
  std::uint64_t finalized_operations;
  std::uint64_t transactions;
  std::uint64_t logical_transaction_bytes;
  std::uint64_t pdus;
  std::uint64_t packets;
  std::uint64_t credits_consumed;
  std::uint64_t credits_returned;
  std::uint64_t retries;
  std::uint64_t completions;
  std::uint64_t logical_operation_bytes;
  std::uint64_t pdu_payload_bytes;
  std::uint64_t wire_bytes;
  std::uint64_t transmitted_wire_bytes;
};

struct RailUtilization {
  std::uint32_t rail_index;
  std::uint64_t pdus;
  std::uint64_t pdu_payload_bytes;
  std::uint64_t wire_bytes;
  SimTimeNs busy_time_ns;
  std::uint64_t transmission_attempts;
  std::uint64_t retries;
  std::uint64_t transmitted_wire_bytes;
};

struct SwitchQueueUtilization {
  std::uint32_t egress_endpoint;
  std::uint64_t accepted_pdus;
  std::uint64_t departed_pdus;
  std::uint64_t pdu_payload_bytes;
  std::uint64_t wire_bytes;
  std::uint64_t maximum_occupancy_pdus;
  std::uint64_t final_occupancy_pdus;
  std::uint64_t backpressure_events;
  SimTimeNs total_admission_delay_ns;
  SimTimeNs busy_time_ns;
  std::uint64_t initial_credits;
  std::uint64_t final_available_credits;
  std::uint64_t credits_consumed;
  std::uint64_t credits_returned;
  std::uint64_t maximum_outstanding_credits;
  std::uint64_t credit_starvation_events;
  SimTimeNs total_credit_starvation_ns;
  std::uint64_t duplicate_pdus;
};

struct ScheduleRecord {
  std::uint32_t layer_index;
  std::string source;
  std::string algorithm;
  std::string version;
};

struct GpuUtilization {
  std::uint32_t endpoint;
  std::uint32_t compute_units;
  std::uint32_t workgroup_slots_per_compute_unit;
  std::uint64_t workgroups_dispatched;
  std::uint64_t wavefronts_issued;
  std::uint64_t maximum_resident_workgroups;
  std::uint64_t resource_stall_events;
  SimTimeNs total_resource_stall_ns;
  SimTimeNs wavefront_issue_busy_ns;
  SimTimeNs reduction_busy_ns;
};

struct SimulationReport {
  std::uint64_t schema_version;
  std::uint64_t deterministic_seed;
  SimTimeNs end_to_end_ns;
  std::vector<LayerTiming> layers;
  ConservationCounters counters;
  std::vector<RailUtilization> rails;
  std::vector<SwitchQueueUtilization> switch_queues;
  std::vector<ScheduleRecord> schedules;
  std::vector<GpuUtilization> gpus;

  ValidationErrors validate() const {
    ValidationErrors errors;
    if (schema_version == 0) {
      append_error(
          errors,
          ValidationCode::InvalidReport,
          "report schema version must be nonzero");
    }
    for (const LayerTiming& layer : layers) {
      if (layer.name.empty() || layer.start_ns > layer.compute_complete_ns ||
          layer.start_ns > layer.communication_issue_ns ||
          layer.communication_issue_ns > layer.communication_complete_ns ||
          layer.compute_complete_ns > layer.finish_ns ||
          layer.communication_complete_ns > layer.finish_ns ||
          layer.finish_ns > end_to_end_ns) {
        append_error(
            errors,
            ValidationCode::InvalidReport,
            "layer timing must be monotonic and within end-to-end time");
      }
      const bool timestamps_monotonic =
          layer.start_ns <= layer.compute_complete_ns &&
          layer.compute_complete_ns <= layer.finish_ns;
      if (timestamps_monotonic &&
          (layer.compute_duration_ns !=
               layer.compute_complete_ns - layer.start_ns ||
           layer.exposed_communication_ns !=
               layer.finish_ns - layer.compute_complete_ns)) {
        append_error(
            errors,
            ValidationCode::InvalidReport,
            "layer derived durations must match recorded timestamps");
      }
    }
    for (const ScheduleRecord& schedule : schedules) {
      if (schedule.layer_index >= layers.size() || schedule.source.empty() ||
          schedule.algorithm.empty() || schedule.version.empty()) {
        append_error(
            errors,
            ValidationCode::InvalidReport,
            "schedule provenance must identify a valid layer and nonempty source");
      }
    }
    for (std::size_t index = 0; index < gpus.size(); ++index) {
      const GpuUtilization& gpu = gpus[index];
      const std::uint64_t capacity =
          static_cast<std::uint64_t>(gpu.compute_units) *
          gpu.workgroup_slots_per_compute_unit;
      if (gpu.endpoint != index + 1 || gpu.compute_units == 0 ||
          gpu.workgroup_slots_per_compute_unit == 0 ||
          gpu.maximum_resident_workgroups > capacity) {
        append_error(
            errors,
            ValidationCode::InvalidReport,
            "GPU reports must be ordered and respect resident capacity");
      }
    }
    if (counters.finalized_operations > counters.issued_operations ||
        counters.completions < counters.finalized_operations) {
      append_error(
          errors,
          ValidationCode::InvalidReport,
          "report completion counters violate conservation bounds");
    }
    if (counters.packets != counters.pdus + counters.retries ||
        counters.pdu_payload_bytes > counters.wire_bytes ||
        counters.transmitted_wire_bytes < counters.wire_bytes) {
      append_error(
          errors,
          ValidationCode::InvalidReport,
          "report PDU and wire counters violate conservation bounds");
    }
    std::uint64_t rail_pdus = 0;
    std::uint64_t rail_payload_bytes = 0;
    std::uint64_t rail_wire_bytes = 0;
    std::uint64_t rail_attempts = 0;
    std::uint64_t rail_retries = 0;
    std::uint64_t rail_transmitted_wire_bytes = 0;
    for (std::size_t index = 0; index < rails.size(); ++index) {
      const RailUtilization& rail = rails[index];
      if (rail.rail_index != index || rail.busy_time_ns > end_to_end_ns) {
        append_error(
            errors,
            ValidationCode::InvalidReport,
            "rail reports must be ordered and bounded by simulation time");
      }
      rail_pdus += rail.pdus;
      rail_payload_bytes += rail.pdu_payload_bytes;
      rail_wire_bytes += rail.wire_bytes;
      rail_attempts += rail.transmission_attempts;
      rail_retries += rail.retries;
      rail_transmitted_wire_bytes += rail.transmitted_wire_bytes;
    }
    if ((!rails.empty() || counters.pdus != 0) &&
        (rail_pdus != counters.pdus ||
         rail_payload_bytes != counters.pdu_payload_bytes ||
         rail_wire_bytes != counters.wire_bytes ||
         rail_attempts != counters.packets ||
         rail_retries != counters.retries ||
         rail_transmitted_wire_bytes != counters.transmitted_wire_bytes)) {
      append_error(
          errors,
          ValidationCode::InvalidReport,
          "per-rail counters must conserve global PDU and byte totals");
    }
    std::uint64_t switch_accepted = 0;
    std::uint64_t switch_departed = 0;
    std::uint64_t switch_payload_bytes = 0;
    std::uint64_t switch_wire_bytes = 0;
    for (std::size_t index = 0; index < switch_queues.size(); ++index) {
      const SwitchQueueUtilization& queue = switch_queues[index];
      if (queue.egress_endpoint != index + 1 ||
          queue.final_occupancy_pdus != 0 ||
          queue.departed_pdus > queue.accepted_pdus ||
          queue.busy_time_ns > end_to_end_ns ||
          queue.final_available_credits != queue.initial_credits ||
          queue.credits_consumed != queue.credits_returned ||
          queue.maximum_outstanding_credits > queue.initial_credits) {
        append_error(
            errors,
            ValidationCode::InvalidReport,
            "switch queue reports must be ordered, drained, and bounded");
      }
      switch_accepted += queue.accepted_pdus;
      switch_departed += queue.departed_pdus;
      switch_payload_bytes += queue.pdu_payload_bytes;
      switch_wire_bytes += queue.wire_bytes;
    }
    if (!switch_queues.empty() &&
        (switch_accepted != counters.pdus ||
         switch_departed != counters.pdus ||
         switch_payload_bytes != counters.pdu_payload_bytes ||
         switch_wire_bytes != counters.wire_bytes)) {
      append_error(
          errors,
          ValidationCode::InvalidReport,
          "switch queue totals must conserve global PDU and byte totals");
    }
    return errors;
  }

  void require_valid() const {
    require_no_errors(validate(), "SimulationReport");
  }
};

}  // namespace contracts
}  // namespace simai

#endif
