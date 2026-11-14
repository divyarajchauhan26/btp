#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "simai/runtime/allreduce_planner.hpp"
#include "simai/runtime/aicb_importer.hpp"
#include "simai/runtime/collective_planner.hpp"
#include "simai/runtime/finite_switch.hpp"
#include "simai/runtime/gpu_execution.hpp"
#include "simai/runtime/multi_rail_fabric.hpp"
#include "simai/runtime/mscclpp_plan.hpp"
#include "simai/runtime/report_writer.hpp"
#include "simai/runtime/reassembly.hpp"
#include "simai/runtime/simple_sue_link.hpp"
#include "simai/runtime/simulator.hpp"
#include "simai/runtime/sue_pdu.hpp"
#include "simai/runtime/workload.hpp"

#ifndef SIMAI_TEST_DATA_DIR
#define SIMAI_TEST_DATA_DIR "native/tests/data"
#endif

namespace {

using namespace simai::contracts;
using namespace simai::runtime;

int failures = 0;

class TestScheduleProvider final : public CollectiveScheduleProvider {
 public:
  TestScheduleProvider() : calls_(0) {}

  PlannedCollective plan(
      const CollectiveRequest& request,
      const PlannerCapabilities& capabilities) const override {
    ++calls_;
    PlannedCollective planned = native_.plan(request, capabilities);
    planned.provenance.source = "test_provider";
    planned.provenance.version = "injected-1";
    return planned;
  }

  std::size_t calls() const { return calls_; }

 private:
  mutable std::size_t calls_;
  NativeCollectivePlanner native_;
};

class SemaphoreScheduleProvider final : public CollectiveScheduleProvider {
 public:
  SemaphoreScheduleProvider(
      std::uint64_t signal_value = 2,
      std::uint64_t wait_value = 2)
      : signal_value_(signal_value), wait_value_(wait_value) {}

  PlannedCollective plan(
      const CollectiveRequest& request,
      const PlannerCapabilities&) const override {
    const DeviceId source = request.group.members.at(0);
    const DeviceId destination = request.group.members.at(1);
    const SemaphoreId semaphore(77);
    const std::vector<MicroOp> operations{
        MicroOp::signal(
            OperationId(1), source, ThreadBlockId(1), 0, {}, destination,
            semaphore, signal_value_, MemoryOrder::Ordered, OrderingDomainId(1),
            CompletionStage::RemoteVisible),
        MicroOp::wait(
            OperationId(2), destination, ThreadBlockId(1), 0, {}, destination,
            semaphore, wait_value_, MemoryOrder::Ordered, OrderingDomainId(1),
            CompletionStage::LocalComplete)};
    CollectivePlan plan{
        request.id, request.kind, request.group, request.payload_bytes,
        operations};
    plan.require_valid();
    return PlannedCollective{
        plan, ScheduleProvenance{"test_mscclpp", "signal_wait", "1"}};
  }

 private:
  std::uint64_t signal_value_;
  std::uint64_t wait_value_;
};

class GetScheduleProvider final : public CollectiveScheduleProvider {
 public:
  PlannedCollective plan(
      const CollectiveRequest& request,
      const PlannerCapabilities&) const override {
    const DeviceId requester = request.group.members.at(0);
    const DeviceId remote = request.group.members.at(1);
    const BufferSlice source{
        remote, BufferId(91), 0, request.payload_bytes,
        MemoryRegion::DeviceLocal};
    const BufferSlice destination{
        requester, BufferId(92), 0, request.payload_bytes,
        MemoryRegion::DeviceLocal};
    const MicroOp operation = MicroOp::get(
        OperationId(1), requester, ThreadBlockId(1), 0, {}, source,
        destination, MemoryOrder::Ordered, OrderingDomainId(1),
        CompletionStage::LocalComplete);
    CollectivePlan plan{
        request.id, request.kind, request.group, request.payload_bytes,
        {operation}};
    plan.require_valid();
    return PlannedCollective{
        plan, ScheduleProvenance{"test_mscclpp", "get", "1"}};
  }
};

class CountingTransportBackend final : public SueTransportBackend {
 public:
  CountingTransportBackend(
      const LinkConfig& config,
      std::uint32_t endpoint_count,
      std::size_t* transmissions)
      : native_(config, endpoint_count), transmissions_(transmissions) {}

  FabricIngressTiming transmit_ingress(
      const SuePdu& pdu, SimTimeNs issue_ns) override {
    ++*transmissions_;
    return native_.transmit_ingress(pdu, issue_ns);
  }

  SwitchForwardTiming forward_from_switch(
      const SuePdu& pdu, SimTimeNs arrival_ns) override {
    return native_.forward_from_switch(pdu, arrival_ns);
  }

  bool has_switch() const override { return native_.has_switch(); }

  const std::vector<RailUtilization>& rail_counters() const override {
    return native_.rail_counters();
  }

  std::vector<SwitchQueueUtilization> finalize_switch(
      SimTimeNs now_ns) override {
    return native_.finalize_switch(now_ns);
  }

 private:
  NativeSueTransportBackend native_;
  std::size_t* transmissions_;
};

class CountingTransportFactory final : public SueTransportBackendFactory {
 public:
  CountingTransportFactory() : creations_(0), transmissions_(0) {}

  std::unique_ptr<SueTransportBackend> create(
      const LinkConfig& config,
      std::uint32_t endpoint_count) const override {
    ++creations_;
    return std::unique_ptr<SueTransportBackend>(
        new CountingTransportBackend(
            config, endpoint_count, &transmissions_));
  }

  std::size_t creations() const { return creations_; }
  std::size_t transmissions() const { return transmissions_; }

 private:
  mutable std::size_t creations_;
  mutable std::size_t transmissions_;
};

void expect(bool condition, const std::string& name) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << name << '\n';
  }
}

void expect_invalid(
    const std::function<void()>& action,
    const std::string& name) {
  try {
    action();
    expect(false, name);
  } catch (const std::invalid_argument&) {
    expect(true, name);
  }
}

void expect_runtime_error(
    const std::function<void()>& action,
    const std::string& name) {
  try {
    action();
    expect(false, name);
  } catch (const std::runtime_error&) {
    expect(true, name);
  }
}

std::string read_file(const std::string& path) {
  std::ifstream input(path.c_str(), std::ios::binary);
  if (!input) {
    throw std::runtime_error("could not open test data: " + path);
  }
  std::ostringstream content;
  content << input.rdbuf();
  return content.str();
}

std::string data_path(const std::string& filename) {
  return std::string(SIMAI_TEST_DATA_DIR) + "/" + filename;
}

void test_typed_parser_and_rejections() {
  const WorkloadSpec workload =
      parse_workload_file(data_path("two_layer_golden.workload"));
  expect(workload.schema_version == 1, "versioned workload parsed");
  expect(workload.endpoint_count == 8, "eight endpoints parsed");
  expect(
      workload.tensor_parallel_group.members.size() == 8,
      "one eight-endpoint tensor group created");
  expect(workload.layers.size() == 2, "two layers parsed");
  expect(
      workload.layers[0].collective == LayerCollectiveKind::AllReduce,
      "typed AllReduce parsed");
  expect(
      workload.layers[1].collective == LayerCollectiveKind::None,
      "typed no-collective layer parsed");

  std::istringstream invalid(
      "schema_version=1\n"
      "deterministic_seed=1\n"
      "endpoint_count=1\n"
      "link_bandwidth_gbps=200\n"
      "propagation_delay_ns=10\n"
      "transaction_payload_bytes=256\n"
      "pdu_payload_bytes=4096\n"
      "pdu_overhead_bytes=0\n"
      "rail_count=1\n"
      "switch_enabled=0\n"
      "switch_queue_capacity_pdus=0\n"
      "switch_latency_ns=0\n"
      "switch_credits_per_egress=0\n"
      "credit_return_latency_ns=0\n"
      "layer=0,100,allreduce,1000\n"
      "layer=1,50,none,0\n");
  expect_invalid(
      [&invalid]() { parse_workload(invalid); },
      "single-endpoint workload rejected");
}

void test_allreduce_lowering() {
  const WorkloadSpec workload =
      parse_workload_file(data_path("two_layer_golden.workload"));
  const CollectivePlan plan = make_gather_reduce_broadcast_allreduce(
      CollectivePlanId(1),
      workload.tensor_parallel_group,
      workload.layers[0].payload_bytes);
  std::uint64_t puts = 0;
  std::uint64_t reductions = 0;
  for (const MicroOp& operation : plan.operations) {
    puts += operation.kind == MicroOpKind::Put ? 1 : 0;
    reductions += operation.kind == MicroOpKind::Reduce ? 1 : 0;
  }
  expect(plan.operations.size() == 21, "AllReduce lowers to 21 operations");
  expect(puts == 14, "AllReduce contains fourteen native PUTs");
  expect(reductions == 7, "AllReduce contains seven local reductions");
  expect(plan.validate().empty(), "lowered AllReduce DAG validates");
}

void test_collective_planner_selection_and_provenance() {
  const WorkloadSpec workload =
      parse_workload_file(data_path("two_layer_golden.workload"));
  const PlannerCapabilities capabilities{8, true, true};
  const CollectiveRequest baseline_request{
      CollectivePlanId(10),
      CollectiveKind::AllReduce,
      workload.tensor_parallel_group,
      1000,
      ReductionKind::Sum,
      CollectiveAlgorithmPolicy::GatherReduceBroadcast};
  const PlannedCollective baseline =
      NativeCollectivePlanner().plan(baseline_request, capabilities);
  expect(
      baseline.plan.operations.size() == 21,
      "explicit baseline planner emits 21 operations");
  expect(
      baseline.provenance.source == "native" &&
          baseline.provenance.algorithm == "gather_reduce_broadcast" &&
          baseline.provenance.version == "1",
      "baseline planner records stable provenance");

  CollectiveRequest ring_request = baseline_request;
  ring_request.id = CollectivePlanId(11);
  ring_request.algorithm = CollectiveAlgorithmPolicy::Ring;
  const PlannedCollective ring =
      NativeCollectivePlanner().plan(ring_request, capabilities);
  std::uint64_t puts = 0;
  std::uint64_t reductions = 0;
  std::uint64_t put_bytes = 0;
  for (const MicroOp& operation : ring.plan.operations) {
    if (operation.kind == MicroOpKind::Put) {
      ++puts;
      put_bytes += operation.source.length_bytes;
    } else if (operation.kind == MicroOpKind::Reduce) {
      ++reductions;
    }
  }
  expect(ring.plan.operations.size() == 168, "ring emits 168 micro-operations");
  expect(puts == 112, "ring emits 112 native PUT operations");
  expect(reductions == 56, "ring emits 56 local reductions");
  expect(put_bytes == 14000, "ring conserves expected network payload bytes");
  expect(ring.plan.validate().empty(), "ring dependency DAG validates");
  expect(
      ring.provenance.source == "native" &&
          ring.provenance.algorithm == "ring" &&
          ring.provenance.version == "1",
      "ring planner records stable provenance");

  CollectiveRequest automatic_request = ring_request;
  automatic_request.id = CollectivePlanId(12);
  automatic_request.algorithm = CollectiveAlgorithmPolicy::Auto;
  const PlannedCollective automatic =
      NativeCollectivePlanner().plan(automatic_request, capabilities);
  expect(
      automatic.provenance.algorithm == "ring",
      "automatic policy selects ring for evenly divisible payload");

  CollectiveRequest uneven_ring = ring_request;
  uneven_ring.id = CollectivePlanId(13);
  uneven_ring.payload_bytes = 1001;
  expect_invalid(
      [&uneven_ring, &capabilities]() {
        NativeCollectivePlanner().plan(uneven_ring, capabilities);
      },
      "ring explicitly rejects indivisible payload");

  CollectiveRequest unsupported = baseline_request;
  unsupported.id = CollectivePlanId(14);
  unsupported.kind = CollectiveKind::Broadcast;
  expect_invalid(
      [&unsupported, &capabilities]() {
        NativeCollectivePlanner().plan(unsupported, capabilities);
      },
      "planner explicitly rejects unsupported collective kind");

  CollectiveRequest unsupported_reduction = baseline_request;
  unsupported_reduction.id = CollectivePlanId(15);
  unsupported_reduction.reduction = ReductionKind::Minimum;
  expect_invalid(
      [&unsupported_reduction, &capabilities]() {
        NativeCollectivePlanner().plan(unsupported_reduction, capabilities);
      },
      "planner explicitly rejects unsupported reduction kind");

  const PlannerCapabilities missing_reduce{8, true, false};
  expect_invalid(
      [&baseline_request, &missing_reduce]() {
        NativeCollectivePlanner().plan(baseline_request, missing_reduce);
      },
      "planner enforces backend capability requirements");

  CollectiveRequest reduce_scatter_request = ring_request;
  reduce_scatter_request.id = CollectivePlanId(16);
  reduce_scatter_request.kind = CollectiveKind::ReduceScatter;
  const PlannedCollective reduce_scatter =
      NativeCollectivePlanner().plan(reduce_scatter_request, capabilities);
  std::uint64_t reduce_scatter_puts = 0;
  std::uint64_t reduce_scatter_reductions = 0;
  std::uint64_t reduce_scatter_bytes = 0;
  for (const MicroOp& operation : reduce_scatter.plan.operations) {
    if (operation.kind == MicroOpKind::Put) {
      ++reduce_scatter_puts;
      reduce_scatter_bytes += operation.source.length_bytes;
    } else if (operation.kind == MicroOpKind::Reduce) {
      ++reduce_scatter_reductions;
    }
  }
  expect(
      reduce_scatter.plan.operations.size() == 112 &&
          reduce_scatter_puts == 56 && reduce_scatter_reductions == 56 &&
          reduce_scatter_bytes == 7000,
      "ring ReduceScatter operation and byte counts are exact");
  expect(
      reduce_scatter.plan.validate().empty(),
      "ring ReduceScatter dependency DAG validates");

  CollectiveRequest allgather_request = ring_request;
  allgather_request.id = CollectivePlanId(17);
  allgather_request.kind = CollectiveKind::AllGather;
  allgather_request.reduction = ReductionKind::None;
  const PlannedCollective allgather =
      NativeCollectivePlanner().plan(allgather_request, capabilities);
  std::uint64_t allgather_bytes = 0;
  for (const MicroOp& operation : allgather.plan.operations) {
    allgather_bytes += operation.source.length_bytes;
  }
  expect(
      allgather.plan.operations.size() == 56 && allgather_bytes == 7000,
      "ring AllGather operation and byte counts are exact");
  expect(
      allgather.plan.validate().empty(),
      "ring AllGather dependency DAG validates");

  CollectiveRequest alltoall_request = allgather_request;
  alltoall_request.id = CollectivePlanId(18);
  alltoall_request.kind = CollectiveKind::AllToAll;
  alltoall_request.algorithm = CollectiveAlgorithmPolicy::Direct;
  const PlannedCollective alltoall =
      NativeCollectivePlanner().plan(alltoall_request, capabilities);
  std::uint64_t alltoall_bytes = 0;
  for (const MicroOp& operation : alltoall.plan.operations) {
    alltoall_bytes += operation.source.length_bytes;
  }
  expect(
      alltoall.plan.operations.size() == 56 && alltoall_bytes == 7000,
      "direct AllToAll operation and byte counts are exact");
  expect(
      alltoall.plan.validate().empty() &&
          alltoall.provenance.algorithm == "direct_alltoall",
      "direct AllToAll plan validates with stable provenance");

  alltoall_request.id = CollectivePlanId(19);
  alltoall_request.algorithm = CollectiveAlgorithmPolicy::Auto;
  expect(
      NativeCollectivePlanner()
              .plan(alltoall_request, capabilities)
              .provenance.algorithm == "direct_alltoall",
      "automatic policy selects direct AllToAll");

  const ParallelGroup tiny_group{
      ParallelGroupId(2), {DeviceId(1), DeviceId(2)}};
  CollectiveRequest tiny_request{
      CollectivePlanId(21),
      CollectiveKind::AllReduce,
      tiny_group,
      256,
      ReductionKind::Sum,
      CollectiveAlgorithmPolicy::Ring};
  const PlannerCapabilities tiny_capabilities{2, true, true};
  expect(
      NativeCollectivePlanner()
              .plan(tiny_request, tiny_capabilities)
              .plan.operations.size() == 6,
      "two-endpoint AllReduce emits four PUTs and two reductions");
  tiny_request.id = CollectivePlanId(27);
  tiny_request.kind = CollectiveKind::ReduceScatter;
  expect(
      NativeCollectivePlanner()
              .plan(tiny_request, tiny_capabilities)
              .plan.operations.size() == 4,
      "two-endpoint ReduceScatter emits two PUTs and two reductions");
  tiny_request.id = CollectivePlanId(22);
  tiny_request.kind = CollectiveKind::AllGather;
  tiny_request.reduction = ReductionKind::None;
  expect(
      NativeCollectivePlanner()
              .plan(tiny_request, tiny_capabilities)
              .plan.operations.size() == 2,
      "two-endpoint AllGather emits two PUTs");
  tiny_request.id = CollectivePlanId(23);
  tiny_request.kind = CollectiveKind::AllToAll;
  tiny_request.algorithm = CollectiveAlgorithmPolicy::Direct;
  expect(
      NativeCollectivePlanner()
              .plan(tiny_request, tiny_capabilities)
              .plan.operations.size() == 2,
      "two-endpoint AllToAll emits two PUTs");

  CollectiveRequest invalid_policy = allgather_request;
  invalid_policy.id = CollectivePlanId(24);
  invalid_policy.algorithm = CollectiveAlgorithmPolicy::Direct;
  expect_invalid(
      [&invalid_policy, &capabilities]() {
        NativeCollectivePlanner().plan(invalid_policy, capabilities);
      },
      "direct policy rejects AllGather");
  invalid_policy.id = CollectivePlanId(25);
  invalid_policy.kind = CollectiveKind::AllToAll;
  invalid_policy.algorithm = CollectiveAlgorithmPolicy::Ring;
  expect_invalid(
      [&invalid_policy, &capabilities]() {
        NativeCollectivePlanner().plan(invalid_policy, capabilities);
      },
      "ring policy rejects AllToAll");
  CollectiveRequest uneven_reduce_scatter = reduce_scatter_request;
  uneven_reduce_scatter.id = CollectivePlanId(26);
  uneven_reduce_scatter.payload_bytes = 1001;
  uneven_reduce_scatter.algorithm = CollectiveAlgorithmPolicy::Auto;
  expect_invalid(
      [&uneven_reduce_scatter, &capabilities]() {
        NativeCollectivePlanner().plan(
            uneven_reduce_scatter, capabilities);
      },
      "automatic ReduceScatter rejects indivisible payload");

  WorkloadSpec ring_workload = workload;
  ring_workload.collective_algorithm = CollectiveAlgorithmPolicy::Ring;
  const SimulationReport ring_report = ScaleUpSimulator().run(ring_workload);
  expect(ring_report.end_to_end_ns == 720, "ring end-to-end time is 720 ns");
  expect(
      ring_report.counters.issued_operations == 168 &&
          ring_report.counters.finalized_operations == 168 &&
          ring_report.counters.completions == 168,
      "ring operations complete exactly once");
  expect(
      ring_report.counters.pdus == 112 &&
          ring_report.counters.logical_operation_bytes == 14000 &&
          ring_report.counters.pdu_payload_bytes == 14000,
      "ring execution conserves PDU and payload totals");
  expect(
      ring_report.schedules.size() == 1 &&
          ring_report.schedules[0].algorithm == "ring",
      "runtime report identifies selected ring schedule");
  expect(
      format_report(ring_report) ==
          format_report(ScaleUpSimulator().run(ring_workload)),
      "ring execution is deterministic");

  ring_workload.link.rail_count = 2;
  const SimulationReport two_rail_ring =
      ScaleUpSimulator().run(ring_workload);
  expect(
      two_rail_ring.end_to_end_ns == 510 &&
          two_rail_ring.rails[0].pdus == 42 &&
          two_rail_ring.rails[1].pdus == 70,
      "mixed flow hash exercises both rails for ring traffic");
}

void test_serialization_hand_calculation() {
  expect(
      SimpleSueLink::serialization_delay_ns(1000, 200) == 40,
      "1000 bytes at 200 Gbps serializes in 40 ns");
  expect(
      SimpleSueLink::serialization_delay_ns(1, 3) == 3,
      "serialization division rounds up to integer nanoseconds");
}

void test_schedule_provider_injection() {
  const WorkloadSpec workload =
      parse_workload_file(data_path("ring_allreduce.workload"));
  TestScheduleProvider provider;
  const SimulationReport report = ScaleUpSimulator().run(workload, provider);
  expect(provider.calls() == 1, "injected provider plans the collective");
  expect(
      report.schedules.size() == 1 &&
          report.schedules[0].source == "test_provider" &&
          report.schedules[0].algorithm == "ring" &&
          report.schedules[0].version == "injected-1",
      "injected provider provenance reaches the report");
  expect(
      report.end_to_end_ns == ScaleUpSimulator().run(workload).end_to_end_ns,
      "provider injection preserves native execution behavior");
}

void test_transport_backend_injection() {
  const WorkloadSpec workload =
      parse_workload_file(data_path("two_layer_golden.workload"));
  const NativeCollectivePlanner planner;
  CountingTransportFactory backend_factory;
  const SimulationReport injected =
      ScaleUpSimulator().run(workload, planner, backend_factory);
  const SimulationReport baseline = ScaleUpSimulator().run(workload);
  expect(backend_factory.creations() == 1, "transport backend created once");
  expect(
      backend_factory.transmissions() == injected.counters.pdus,
      "injected transport backend receives every logical PDU");
  expect(
      format_report(injected) == format_report(baseline),
      "backend abstraction preserves golden native behavior");
}

void test_mscclpp_plan_translation() {
  const MscclppPlan parsed =
      parse_mscclpp_plan_file(data_path("mscclpp_two_gpu.json"));
  expect(parsed.name == "simai_two_gpu_put_signal_wait", "MSCCL++ plan name parsed");
  expect(parsed.gpus.size() == 2, "MSCCL++ GPU array parsed");
  expect(
      parsed.gpus[0].threadblocks[0].operations.size() == 4,
      "MSCCL++ threadblock operation list parsed");

  const ParallelGroup group{
      ParallelGroupId(1), {DeviceId(1), DeviceId(2)}};
  const CollectiveRequest request{
      CollectivePlanId(9), CollectiveKind::AllReduce, group, 2048,
      ReductionKind::Sum, CollectiveAlgorithmPolicy::Auto};
  const PlannerCapabilities capabilities{2, true, true};
  const PlannedCollective planned =
      MscclppScheduleProvider(parsed).plan(request, capabilities);
  expect(
      planned.provenance.source == "mscclpp" &&
          planned.provenance.algorithm == "simai_two_gpu_put_signal_wait",
      "MSCCL++ provenance retained");
  expect(planned.plan.operations.size() == 8, "MSCCL++ operations translated exactly");
  expect(
      planned.plan.operations[0].kind == MicroOpKind::Put &&
          planned.plan.operations[0].source.owner == DeviceId(1) &&
          planned.plan.operations[0].destination.owner == DeviceId(2) &&
          planned.plan.operations[0].source.length_bytes == 1024,
      "MSCCL++ PUT buffers map from chunks to bytes");
  expect(
      planned.plan.operations[1].kind == MicroOpKind::Signal &&
          planned.plan.operations[1].semaphore_owner == DeviceId(2) &&
          planned.plan.operations[1].semaphore_value == 1,
      "MSCCL++ SIGNAL maps to remote semaphore generation");
  expect(
      planned.plan.operations[2].kind == MicroOpKind::Wait &&
          planned.plan.operations[2].semaphore_owner == DeviceId(1) &&
          planned.plan.operations[2].semaphore_value == 1,
      "MSCCL++ WAIT maps to local semaphore generation");
  expect(
      planned.plan.operations[3].kind == MicroOpKind::Reduce &&
          planned.plan.operations[3].reduction == ReductionKind::Sum,
      "MSCCL++ reduce maps to local sum");
  expect(
      planned.plan.operations[3].dependencies.size() == 1 &&
          planned.plan.operations[3].dependencies[0] == OperationId(3),
      "MSCCL++ threadblock program order becomes dependencies");

  MscclppPlan unsupported = parsed;
  unsupported.gpus[0].threadblocks[0].operations[0].name = "rres";
  expect_invalid(
      [&unsupported, &request, &capabilities]() {
        MscclppScheduleProvider(unsupported).plan(request, capabilities);
      },
      "unsupported fused MSCCL++ operation rejected explicitly");

  std::istringstream malformed("{\"name\": \"broken\",");
  expect_invalid(
      [&malformed]() { parse_mscclpp_plan(malformed); },
      "malformed MSCCL++ JSON rejected explicitly");
}

void test_signal_wait_runtime() {
  WorkloadSpec workload =
      parse_workload_file(data_path("ring_allreduce.workload"));
  SemaphoreScheduleProvider provider;
  const SimulationReport report = ScaleUpSimulator().run(workload, provider);
  expect(
      report.counters.issued_operations == 2 &&
          report.counters.finalized_operations == 2 &&
          report.counters.completions == 2,
      "SIGNAL and blocked WAIT complete exactly once");
  expect(
      report.counters.logical_operation_bytes == 8 &&
          report.counters.pdus == 1 && report.counters.transactions == 1,
      "SIGNAL crosses the fabric as one eight-byte atomic transaction");
  expect(
      report.schedules.size() == 1 &&
          report.schedules[0].source == "test_mscclpp",
      "SIGNAL/WAIT plan retains provider provenance");

  SemaphoreScheduleProvider stale_provider(1, 2);
  bool stale_wait_failed = false;
  try {
    ScaleUpSimulator().run(workload, stale_provider);
  } catch (const std::logic_error&) {
    stale_wait_failed = true;
  }
  expect(stale_wait_failed, "stale SIGNAL generation cannot release WAIT");
}

void test_get_request_response_runtime() {
  WorkloadSpec workload =
      parse_workload_file(data_path("two_endpoint_allgather.workload"));
  const BufferSlice remote{
      DeviceId(2), BufferId(91), 0, 256, MemoryRegion::DeviceLocal};
  const BufferSlice local{
      DeviceId(1), BufferId(92), 0, 256, MemoryRegion::DeviceLocal};
  const MicroOp get = MicroOp::get(
      OperationId(1), DeviceId(1), ThreadBlockId(1), 0, {}, remote, local,
      MemoryOrder::Ordered, OrderingDomainId(1),
      CompletionStage::LocalComplete);
  std::uint64_t next_transaction = 1;
  std::uint64_t next_pdu = 1;
  const SuePacketizer packetizer(workload.link);
  const std::vector<SuePdu> requests = packetizer.packetize_get_request(
      get, next_transaction, next_pdu);
  const std::vector<SuePdu> responses = packetizer.packetize_get_response(
      get, next_transaction, next_pdu);
  expect(
      requests.size() == 1 && requests[0].payload_bytes == 8 &&
          !requests[0].transactions[0].data_present,
      "GET request carries one compact non-data descriptor");
  expect(
      responses.size() == 1 && responses[0].payload_bytes == 256 &&
          responses[0].transactions[0].data_present &&
          responses[0].source_endpoint == DeviceId(2) &&
          responses[0].destination_endpoint == DeviceId(1),
      "GET response carries data from remote endpoint to requester");

  const SimulationReport report =
      ScaleUpSimulator().run(workload, GetScheduleProvider());
  expect(
      report.counters.issued_operations == 1 &&
          report.counters.finalized_operations == 1 &&
          report.counters.completions == 1,
      "GET completes exactly once after its response");
  expect(
      report.counters.logical_operation_bytes == 256 &&
          report.counters.transactions == 2 && report.counters.pdus == 2 &&
          report.counters.pdu_payload_bytes == 264,
      "GET accounting separates logical data from request and response traffic");
  expect(
      report.schedules.size() == 1 &&
          report.schedules[0].algorithm == "get",
      "GET provider provenance reaches report");
}

void test_gpu_resource_scheduling() {
  const WorkloadSpec workload =
      parse_workload_file(data_path("two_layer_golden.workload"));
  const CollectivePlan plan = make_gather_reduce_broadcast_allreduce(
      CollectivePlanId(20),
      workload.tensor_parallel_group,
      workload.layers[0].payload_bytes);
  const GpuConfig config{1, 1, 1, 4, 2, 3, 100};
  GpuExecutionModel gpu(config, 8);
  const GpuKernel kernel = gpu.lower(plan.operations[0]);
  expect(
      kernel.workgroups.size() == 1 &&
          kernel.workgroups[0].wavefronts.size() == 4,
      "micro-operation lowers to one four-wavefront workgroup");

  const std::vector<GpuDispatch> first =
      gpu.admit(0, plan.operations[0], 0);
  expect(
      first.size() == 1 && first[0].dispatch_ns == 0 &&
          first[0].instruction_ready_ns == 6 &&
          first[0].local_execution_complete_ns == 6,
      "two wavefront issue rounds take six nanoseconds");
  expect(
      gpu.admit(1, plan.operations[0], 0).empty(),
      "second workgroup waits when the only resident slot is occupied");
  const std::vector<GpuDispatch> second = gpu.complete(0, 20);
  expect(
      second.size() == 1 && second[0].dispatch_ns == 20 &&
          second[0].instruction_ready_ns == 26 &&
          second[0].resource_stalled && second[0].resource_stall_ns == 20,
      "slot release dispatches waiting workgroup with exact stall time");
  expect(gpu.complete(1, 50).empty(), "second workgroup releases cleanly");

  const std::vector<GpuDispatch> reduction =
      gpu.admit(2, plan.operations[7], 50);
  expect(
      reduction.size() == 1 && reduction[0].instruction_ready_ns == 56 &&
          reduction[0].local_execution_complete_ns == 66,
      "1000-byte local reduction adds ten nanoseconds");
  expect(gpu.complete(2, 66).empty(), "reduction workgroup releases cleanly");

  const std::vector<GpuUtilization> utilization = gpu.utilization();
  expect(
      utilization[1].workgroups_dispatched == 2 &&
          utilization[1].wavefronts_issued == 8 &&
          utilization[1].maximum_resident_workgroups == 1 &&
          utilization[1].resource_stall_events == 1 &&
          utilization[1].total_resource_stall_ns == 20 &&
          utilization[1].wavefront_issue_busy_ns == 12,
      "endpoint two GPU resource counters are exact");
  expect(
      utilization[0].workgroups_dispatched == 1 &&
          utilization[0].wavefronts_issued == 4 &&
          utilization[0].reduction_busy_ns == 10,
      "endpoint one reduction counters are exact");

  const WorkloadSpec integrated =
      parse_workload_file(data_path("gpu_contended.workload"));
  const SimulationReport report = ScaleUpSimulator().run(integrated);
  expect(report.end_to_end_ns == 854, "GPU-contended pipeline ends at 854 ns");
  expect(
      report.layers[0].exposed_communication_ns == 704,
      "GPU-contended collective exposes 704 ns communication time");
  expect(report.gpus.size() == 8, "GPU report contains every endpoint");
  expect(
      report.gpus[0].workgroups_dispatched == 14 &&
          report.gpus[0].wavefronts_issued == 56 &&
          report.gpus[0].maximum_resident_workgroups == 1 &&
          report.gpus[0].resource_stall_events == 6 &&
          report.gpus[0].total_resource_stall_ns == 1176 &&
          report.gpus[0].wavefront_issue_busy_ns == 84 &&
          report.gpus[0].reduction_busy_ns == 70,
      "root GPU contention counters match analytical schedule");
  expect(
      report.counters.issued_operations == 21 &&
          report.counters.finalized_operations == 21 &&
          report.counters.completions == 21,
      "GPU scheduling preserves exactly-once collective completion");
  expect(
      format_report(report) == format_report(ScaleUpSimulator().run(integrated)),
      "GPU resource execution is deterministic");

  WorkloadSpec invalid_gpu = integrated;
  invalid_gpu.gpu.enabled = 0;
  expect(
      !invalid_gpu.validate().empty(),
      "disabled GPU model rejects nonzero resource parameters");
}

void test_collective_coverage_end_to_end() {
  const WorkloadSpec tiny =
      parse_workload_file(data_path("two_endpoint_allgather.workload"));
  const SimulationReport tiny_report = ScaleUpSimulator().run(tiny);
  expect(
      tiny.endpoint_count == 2 && tiny_report.end_to_end_ns == 172 &&
          tiny_report.counters.issued_operations == 2 &&
          tiny_report.counters.logical_operation_bytes == 256,
      "two-endpoint AllGather runs end to end with exact timing and bytes");

  const WorkloadSpec reduce_scatter =
      parse_workload_file(data_path("ring_reducescatter.workload"));
  const SimulationReport reduce_scatter_report =
      ScaleUpSimulator().run(reduce_scatter);
  expect(
      reduce_scatter_report.end_to_end_ns == 440 &&
          reduce_scatter_report.counters.issued_operations == 112 &&
          reduce_scatter_report.counters.pdus == 56 &&
          reduce_scatter_report.counters.logical_operation_bytes == 7000,
      "ReduceScatter executes with exact timing, operations, and bytes");

  const WorkloadSpec allgather =
      parse_workload_file(data_path("ring_allgather.workload"));
  const SimulationReport allgather_report = ScaleUpSimulator().run(allgather);
  expect(
      allgather_report.end_to_end_ns == 440 &&
          allgather_report.counters.issued_operations == 56 &&
          allgather_report.counters.pdus == 56 &&
          allgather_report.counters.logical_operation_bytes == 7000,
      "AllGather executes with exact timing, operations, and bytes");

  const WorkloadSpec alltoall =
      parse_workload_file(data_path("direct_alltoall.workload"));
  const SimulationReport alltoall_report = ScaleUpSimulator().run(alltoall);
  expect(
      alltoall_report.end_to_end_ns == 440 &&
          alltoall_report.counters.issued_operations == 56 &&
          alltoall_report.counters.pdus == 56 &&
          alltoall_report.counters.logical_operation_bytes == 7000 &&
          alltoall_report.schedules[0].algorithm == "direct_alltoall",
      "AllToAll executes with exact timing, operations, bytes, and provenance");
  expect(
      format_report(reduce_scatter_report) ==
              format_report(ScaleUpSimulator().run(reduce_scatter)) &&
          format_report(allgather_report) ==
              format_report(ScaleUpSimulator().run(allgather)) &&
          format_report(alltoall_report) ==
          format_report(ScaleUpSimulator().run(alltoall)),
      "all added collective executions are deterministic");

  const auto exercise_switch_contention = [](WorkloadSpec workload) {
    workload.link.rail_count = 2;
    workload.link.switch_enabled = 1;
    workload.link.switch_queue_capacity_pdus = 1;
    workload.link.switch_credits_per_egress = 1;
    workload.link.credit_return_latency_ns = 100;
    const SimulationReport report = ScaleUpSimulator().run(workload);
    std::uint64_t accepted = 0;
    std::uint64_t departed = 0;
    std::uint64_t backpressure = 0;
    for (const SwitchQueueUtilization& queue : report.switch_queues) {
      accepted += queue.accepted_pdus;
      departed += queue.departed_pdus;
      backpressure += queue.backpressure_events;
    }
    return accepted == report.counters.pdus &&
        departed == report.counters.pdus && backpressure != 0 &&
        report.counters.pdus == 56;
  };
  expect(
      exercise_switch_contention(reduce_scatter),
      "ReduceScatter contention conserves all switched PDUs");
  expect(
      exercise_switch_contention(allgather),
      "AllGather contention conserves all switched PDUs");
  expect(
      exercise_switch_contention(alltoall),
      "AllToAll contention conserves all switched PDUs");
}

void test_multi_layer_overlap_and_global_identity() {
  const WorkloadSpec overlap =
      parse_workload_file(data_path("multi_layer_overlap.workload"));
  const SimulationReport overlap_report = ScaleUpSimulator().run(overlap);
  expect(
      overlap.layers.size() == 4 && overlap_report.layers.size() == 4,
      "general workload accepts four typed layers");
  expect(
      overlap_report.layers[1].start_ns == 100 &&
          overlap_report.layers[1].finish_ns == 300,
      "second-layer compute overlaps first-layer communication");
  expect(
      overlap_report.layers[2].start_ns == 300 &&
          overlap_report.layers[2].communication_issue_ns == 670,
      "next compute overlaps while collective issue respects communication fence");
  expect(
      overlap_report.end_to_end_ns == 990 &&
          overlap_report.schedules.size() == 2 &&
          overlap_report.counters.issued_operations == 224 &&
          overlap_report.counters.logical_operation_bytes == 21000,
      "overlap trace has exact end-to-end, schedule, operation, and byte totals");

  WorkloadSpec serial = overlap;
  serial.layers[1].start_policy = LayerStartPolicy::AfterPreviousFinish;
  serial.layers[2].start_policy = LayerStartPolicy::AfterPreviousFinish;
  const SimulationReport serial_report = ScaleUpSimulator().run(serial);
  expect(
      serial_report.end_to_end_ns == 1240,
      "fully serial form of the trace ends at 1240 ns");
  expect(
      serial_report.end_to_end_ns - overlap_report.end_to_end_ns == 250,
      "explicit overlap hides exactly 250 ns of compute time");

  WorkloadSpec switched = overlap;
  switched.link.rail_count = 2;
  switched.link.switch_enabled = 1;
  switched.link.switch_queue_capacity_pdus = 1;
  const SimulationReport switched_report = ScaleUpSimulator().run(switched);
  std::uint64_t accepted = 0;
  std::uint64_t duplicate_pdus = 0;
  for (const SwitchQueueUtilization& queue : switched_report.switch_queues) {
    accepted += queue.accepted_pdus;
    duplicate_pdus += queue.duplicate_pdus;
  }
  expect(
      accepted == switched_report.counters.pdus && duplicate_pdus == 0,
      "multiple collectives use simulation-global PDU identities");
  expect(
      format_report(overlap_report) ==
          format_report(ScaleUpSimulator().run(overlap)),
      "multi-layer overlap execution is deterministic");
}

void test_aicb_subset_import() {
  const WorkloadSpec base =
      parse_workload_file(data_path("two_layer_golden.workload"));
  const AicbImportOptions options{
      1,
      20261005,
      CollectiveAlgorithmPolicy::Auto,
      base.link,
      base.gpu,
      1};
  const WorkloadSpec imported = import_aicb_workload_file(
      data_path("aicb_scaleup_subset.txt"), options);
  expect(
      imported.endpoint_count == 8 && imported.layers.size() == 6,
      "AICB subset expands two rows into six typed phases");
  expect(
      imported.layers[0].name == "attention.forward" &&
          imported.layers[0].collective == LayerCollectiveKind::AllReduce &&
          imported.layers[1].name == "attention.input_gradient" &&
          imported.layers[1].collective == LayerCollectiveKind::None &&
          imported.layers[2].name == "attention.weight_gradient" &&
          imported.layers[2].compute_duration_ns == 30 &&
          imported.layers[2].collective == LayerCollectiveKind::AllGather,
      "AICB phase names, compute, and collectives map exactly");
  const SimulationReport report = ScaleUpSimulator().run(imported);
  expect(
      report.end_to_end_ns == 1682 && report.layers.size() == 6 &&
          report.schedules.size() == 4 &&
          report.counters.issued_operations == 392 &&
          report.counters.logical_operation_bytes == 35000,
      "imported AICB subset executes through native planner and SUE pipeline");
  expect(
      format_report(report) == format_report(ScaleUpSimulator().run(imported)),
      "imported AICB subset execution is deterministic");

  std::istringstream unsupported_group(
      "HYBRID_TRANSFORMER model_parallel_NPU_group: 4 ep: 2 pp: 1 "
      "vpp: 1 ga: 1 all_gpus: 8\n"
      "1\n"
      "expert -1 1 ALLTOALL_EP 8 1 NONE 0 1 NONE 0 1\n");
  expect_invalid(
      [&unsupported_group, &options]() {
        import_aicb_workload(unsupported_group, options);
      },
      "AICB importer rejects a multi-group workload instead of mis-mapping it");
}

void test_transaction_segmentation_and_pdu_serialization() {
  const WorkloadSpec workload =
      parse_workload_file(data_path("two_layer_golden.workload"));
  const CollectivePlan plan = make_gather_reduce_broadcast_allreduce(
      CollectivePlanId(1),
      workload.tensor_parallel_group,
      workload.layers[0].payload_bytes);
  const LinkConfig with_overhead{
      200, 10, 256, 4096, 32, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  const SuePacketizer packetizer(with_overhead);
  std::uint64_t next_transaction = 1;
  std::uint64_t next_pdu = 1;
  const std::vector<SuePdu> pdus = packetizer.packetize_put(
      plan.operations.front(), next_transaction, next_pdu);
  expect(pdus.size() == 1, "1000-byte PUT packs into one PDU");
  expect(
      pdus[0].transactions.size() == 4,
      "1000-byte PUT segments into four bounded transactions");
  expect(pdus[0].payload_bytes == 1000, "PDU payload bytes conserved");
  expect(pdus[0].wire_bytes == 1032, "configured PDU overhead counted");
  expect(pdus[0].validate().empty(), "packed PDU validates");

  SimpleSueLink link(with_overhead);
  const LinkTransferTiming timing = link.transmit(pdus[0], 0);
  expect(
      timing.serialization_complete_ns == 42,
      "1032 wire bytes at 200 Gbps serialize in 42 ns");
  expect(timing.arrival_ns == 52, "PDU arrives after propagation delay");

  const CollectivePlan large_plan = make_gather_reduce_broadcast_allreduce(
      CollectivePlanId(2), workload.tensor_parallel_group, 9000);
  next_transaction = 1;
  next_pdu = 1;
  const std::vector<SuePdu> large_pdus = packetizer.packetize_put(
      large_plan.operations.front(), next_transaction, next_pdu);
  std::uint64_t transaction_count = 0;
  std::uint64_t payload_bytes = 0;
  std::uint64_t wire_bytes = 0;
  for (const SuePdu& pdu : large_pdus) {
    expect(pdu.payload_bytes <= 4096, "PDU payload stays within 4096 bytes");
    for (const SueTransaction& transaction : pdu.transactions) {
      expect(
          transaction.logical_bytes <= 256,
          "transaction payload stays within 256 bytes");
    }
    transaction_count += pdu.transactions.size();
    payload_bytes += pdu.payload_bytes;
    wire_bytes += pdu.wire_bytes;
  }
  expect(large_pdus.size() == 3, "9000-byte PUT requires three PDUs");
  expect(transaction_count == 36, "9000-byte PUT requires 36 transactions");
  expect(payload_bytes == 9000, "multi-PDU payload bytes conserved");
  expect(wire_bytes == 9096, "per-PDU overhead counted three times");
}

void test_multi_rail_timing_and_conservation() {
  WorkloadSpec workload =
      parse_workload_file(data_path("two_layer_golden.workload"));
  workload.link.rail_count = 2;
  const SimulationReport report = ScaleUpSimulator().run(workload);
  expect(report.end_to_end_ns == 530, "two rails reduce end-to-end time to 530 ns");
  expect(
      report.layers[0].exposed_communication_ns == 380,
      "two-rail AllReduce matches 380 ns hand calculation");
  expect(report.rails.size() == 2, "two rail reports emitted");
  expect(
      report.rails[0].pdus == 5 &&
          report.rails[0].pdu_payload_bytes == 5000 &&
          report.rails[0].wire_bytes == 5000 &&
          report.rails[0].busy_time_ns == 200,
      "rail zero exact utilization counters");
  expect(
      report.rails[1].pdus == 9 &&
          report.rails[1].pdu_payload_bytes == 9000 &&
          report.rails[1].wire_bytes == 9000 &&
          report.rails[1].busy_time_ns == 360,
      "rail one exact utilization counters");
  expect(
      report.rails[0].pdus + report.rails[1].pdus ==
          report.counters.pdus,
      "per-rail PDU counts conserve global count");
  expect(
      report.rails[0].wire_bytes + report.rails[1].wire_bytes ==
          report.counters.wire_bytes,
      "per-rail wire bytes conserve global bytes");
  expect(
      format_report(report) == format_report(ScaleUpSimulator().run(workload)),
      "multi-rail selection and timing are deterministic");
}

void test_finite_switch_queue_and_backpressure() {
  const WorkloadSpec workload =
      parse_workload_file(data_path("two_layer_golden.workload"));
  const CollectivePlan plan = make_gather_reduce_broadcast_allreduce(
      CollectivePlanId(3),
      workload.tensor_parallel_group,
      workload.layers[0].payload_bytes);
  const LinkConfig switch_config{
      200, 10, 256, 4096, 0, 2, 1, 1, 5, 0, 0, 0, 0, 0, 0};
  const SuePacketizer packetizer(switch_config);
  std::uint64_t next_transaction = 1;
  std::uint64_t next_pdu = 1;
  const SuePdu first = packetizer.packetize_put(
      plan.operations[0], next_transaction, next_pdu)[0];
  const SuePdu second = packetizer.packetize_put(
      plan.operations[1], next_transaction, next_pdu)[0];
  FiniteSwitch finite_switch(switch_config, 8);
  const SwitchForwardTiming first_timing = finite_switch.forward(first, 0);
  const SwitchForwardTiming second_timing = finite_switch.forward(second, 0);

  expect(first_timing.admission_ns == 0, "first PDU admitted immediately");
  expect(first_timing.service_start_ns == 5, "switch latency precedes service");
  expect(first_timing.departure_ns == 45, "first PDU departs at 45 ns");
  expect(first_timing.destination_arrival_ns == 55, "first PDU arrives at 55 ns");
  expect(!first_timing.backpressured, "first PDU is not backpressured");
  expect(second_timing.admission_ns == 45, "full queue delays second admission");
  expect(second_timing.service_start_ns == 50, "second service starts after latency");
  expect(second_timing.departure_ns == 90, "second PDU departs at 90 ns");
  expect(second_timing.destination_arrival_ns == 100, "second PDU arrives at 100 ns");
  expect(second_timing.backpressured, "second PDU records backpressure");
  const std::vector<PduId>& trace =
      finite_switch.arbitration_trace(DeviceId(1));
  expect(
      trace.size() == 2 && trace[0] == first.id && trace[1] == second.id,
      "equal-time arbitration preserves deterministic insertion order");
  const SwitchForwardTiming duplicate = finite_switch.forward(first, 0);
  expect(duplicate.duplicate, "switch rejects a duplicate PDU sequence");

  const std::vector<SwitchQueueUtilization> queues =
      finite_switch.finalize(100);
  const SwitchQueueUtilization& root = queues[0];
  expect(
      root.accepted_pdus == 2 && root.departed_pdus == 2,
      "switch accepted and departed counts conserve PDUs");
  expect(
      root.maximum_occupancy_pdus == 1 &&
          root.final_occupancy_pdus == 0,
      "finite queue respects capacity and drains to zero");
  expect(
      root.backpressure_events == 1 &&
          root.total_admission_delay_ns == 45,
      "switch reports exact admission backpressure");
  expect(
      root.pdu_payload_bytes == 2000 && root.wire_bytes == 2000 &&
          root.busy_time_ns == 80,
      "switch byte and service counters are exact");
  expect(root.duplicate_pdus == 1, "switch counts one rejected duplicate");
}

void test_integrated_switch_conservation() {
  WorkloadSpec workload =
      parse_workload_file(data_path("two_layer_golden.workload"));
  workload.link.rail_count = 2;
  workload.link.switch_enabled = 1;
  workload.link.switch_queue_capacity_pdus = 1;
  workload.link.switch_latency_ns = 0;
  const SimulationReport report = ScaleUpSimulator().run(workload);
  expect(report.end_to_end_ns == 710, "finite-switch end-to-end time is 710 ns");
  expect(
      report.layers[0].exposed_communication_ns == 560,
      "finite-switch communication matches hand calculation");
  expect(report.switch_queues.size() == 8, "one queue report per egress");
  const SwitchQueueUtilization& root = report.switch_queues[0];
  expect(
      root.accepted_pdus == 7 && root.departed_pdus == 7 &&
          root.maximum_occupancy_pdus == 1 &&
          root.final_occupancy_pdus == 0,
      "contended root egress respects capacity and drains");
  expect(
      root.backpressure_events == 6 &&
          root.total_admission_delay_ns == 400 &&
          root.busy_time_ns == 280,
      "integrated root queue exposes exact contention");
  std::uint64_t accepted = 0;
  std::uint64_t departed = 0;
  std::uint64_t wire_bytes = 0;
  for (const SwitchQueueUtilization& queue : report.switch_queues) {
    accepted += queue.accepted_pdus;
    departed += queue.departed_pdus;
    wire_bytes += queue.wire_bytes;
  }
  expect(
      accepted == report.counters.pdus &&
          departed == report.counters.pdus &&
          wire_bytes == report.counters.wire_bytes,
      "switch queues conserve global PDU and byte totals");
}

void test_credit_conservation_and_starvation() {
  const WorkloadSpec workload =
      parse_workload_file(data_path("two_layer_golden.workload"));
  const CollectivePlan plan = make_gather_reduce_broadcast_allreduce(
      CollectivePlanId(4),
      workload.tensor_parallel_group,
      workload.layers[0].payload_bytes);
  const LinkConfig credit_config{
      200, 10, 256, 4096, 0, 2, 1, 2, 0, 1, 5, 0, 0, 0, 0};
  const SuePacketizer packetizer(credit_config);
  std::uint64_t next_transaction = 1;
  std::uint64_t next_pdu = 1;
  const SuePdu first = packetizer.packetize_put(
      plan.operations[0], next_transaction, next_pdu)[0];
  const SuePdu second = packetizer.packetize_put(
      plan.operations[1], next_transaction, next_pdu)[0];
  FiniteSwitch finite_switch(credit_config, 8);
  const SwitchForwardTiming first_timing = finite_switch.forward(first, 0);
  const SwitchForwardTiming second_timing = finite_switch.forward(second, 0);
  expect(first_timing.admission_ns == 0, "first credit consumed immediately");
  expect(first_timing.departure_ns == 40, "first credited PDU departs at 40 ns");
  expect(
      second_timing.admission_ns == 45,
      "second PDU waits for five-nanosecond credit return");
  expect(second_timing.departure_ns == 85, "second credited PDU departs at 85 ns");
  const std::vector<SwitchQueueUtilization> queues =
      finite_switch.finalize(95);
  const SwitchQueueUtilization& root = queues[0];
  expect(
      root.initial_credits == 1 && root.final_available_credits == 1,
      "initial credit is fully restored");
  expect(
      root.credits_consumed == 2 && root.credits_returned == 2,
      "credit consumption and return are conserved");
  expect(
      root.maximum_outstanding_credits == 1,
      "outstanding credits never exceed configured depth");
  expect(
      root.credit_starvation_events == 1 &&
          root.total_credit_starvation_ns == 45,
      "credit starvation event and delay are exact");

  WorkloadSpec integrated = workload;
  integrated.link.rail_count = 2;
  integrated.link.switch_enabled = 1;
  integrated.link.switch_queue_capacity_pdus = 2;
  integrated.link.switch_credits_per_egress = 1;
  integrated.link.credit_return_latency_ns = 5;
  const SimulationReport report = ScaleUpSimulator().run(integrated);
  expect(report.end_to_end_ns == 740, "credit-controlled pipeline ends at 740 ns");
  expect(
      report.counters.credits_consumed == 14 &&
          report.counters.credits_returned == 14,
      "global credit counters conserve all switched PDUs");
  expect(
      report.switch_queues[0].credit_starvation_events == 6 &&
          report.switch_queues[0].total_credit_starvation_ns == 505,
      "integrated root egress exposes exact credit starvation");
}

void test_retry_reassembly_and_exactly_once_completion() {
  PduReassembly reassembly({PduId(1), PduId(2)});
  expect(reassembly.accept(PduId(1)), "first reassembly arrival accepted");
  expect(!reassembly.accept(PduId(1)), "duplicate reassembly arrival rejected");
  expect(!reassembly.complete(), "reassembly waits for every unique PDU");
  expect(reassembly.accept(PduId(2)), "second unique PDU accepted");
  expect(reassembly.complete(), "reassembly completes on unique set");
  expect(reassembly.duplicates() == 1, "reassembly duplicate count exact");

  WorkloadSpec workload =
      parse_workload_file(data_path("two_layer_golden.workload"));
  workload.link.error_injection_pdu_id = 1;
  workload.link.error_injection_attempts = 1;
  workload.link.maximum_retries = 2;
  workload.link.replay_latency_ns = 7;
  const SimulationReport report = ScaleUpSimulator().run(workload);
  expect(report.end_to_end_ns == 787, "single replay produces 787 ns end-to-end");
  expect(report.counters.pdus == 14, "retry does not duplicate logical PDUs");
  expect(
      report.counters.packets == 15 && report.counters.retries == 1,
      "one failed attempt produces one replay packet");
  expect(
      report.counters.wire_bytes == 14000 &&
          report.counters.transmitted_wire_bytes == 15000,
      "logical and physically transmitted wire bytes remain distinct");
  expect(
      report.counters.finalized_operations == 21 &&
          report.counters.completions == 21,
      "retry preserves exactly-once operation completion");
  expect(
      report.rails[0].transmission_attempts == 15 &&
          report.rails[0].retries == 1 &&
          report.rails[0].busy_time_ns == 600,
      "rail attempt, retry, and busy-time accounting is exact");

  WorkloadSpec exhausted = workload;
  exhausted.link.error_injection_attempts = 2;
  exhausted.link.maximum_retries = 1;
  const CollectivePlan plan = make_gather_reduce_broadcast_allreduce(
      CollectivePlanId(5),
      exhausted.tensor_parallel_group,
      exhausted.layers[0].payload_bytes);
  const SuePacketizer packetizer(exhausted.link);
  std::uint64_t next_transaction = 1;
  std::uint64_t next_pdu = 1;
  const SuePdu pdu = packetizer.packetize_put(
      plan.operations[0], next_transaction, next_pdu)[0];
  MultiRailFabric fabric(exhausted.link, 8);
  expect_runtime_error(
      [&fabric, &pdu]() { fabric.transmit_ingress(pdu, 0); },
      "retry exhaustion fails explicitly and terminates");
}

void test_golden_end_to_end() {
  const WorkloadSpec workload =
      parse_workload_file(data_path("two_layer_golden.workload"));
  const ScaleUpSimulator simulator;
  const SimulationReport report = simulator.run(workload);

  expect(report.end_to_end_ns == 730, "golden end-to-end time is 730 ns");
  expect(report.layers.size() == 2, "golden report has two layers");
  expect(report.layers[0].compute_duration_ns == 100, "layer zero compute time");
  expect(
      report.layers[0].communication_issue_ns == 100,
      "layer zero communication issue time");
  expect(
      report.layers[0].communication_complete_ns == 680,
      "layer zero communication completion time");
  expect(
      report.layers[0].exposed_communication_ns == 580,
      "layer zero exposed communication time");
  expect(
      report.layers[0].transferred_payload_bytes == 14000,
      "layer zero transferred payload bytes");
  expect(report.layers[1].start_ns == 680, "layer one starts after AllReduce");
  expect(report.layers[1].finish_ns == 730, "layer one finishes at 730 ns");
  expect(report.counters.issued_operations == 21, "21 operations issued");
  expect(report.counters.finalized_operations == 21, "21 operations finalized");
  expect(report.counters.transactions == 56, "56 bounded transactions executed");
  expect(
      report.counters.logical_transaction_bytes == 14000,
      "logical transaction bytes conserved");
  expect(report.counters.completions == 21, "21 completions recorded");
  expect(
      report.counters.pdus == 14 && report.counters.packets == 14,
      "one PDU and packet emitted per logical PUT");
  expect(
      report.counters.logical_operation_bytes == 14000 &&
          report.counters.pdu_payload_bytes == 14000 &&
          report.counters.wire_bytes == 14000,
      "operation, transaction, PDU, and wire bytes are conserved");
  expect(
      report.rails.size() == 1 && report.rails[0].pdus == 14 &&
          report.rails[0].wire_bytes == 14000 &&
          report.rails[0].busy_time_ns == 560,
      "one-rail golden utilization is exact");
  expect(
          report.counters.credits_consumed == 0 &&
          report.counters.credits_returned == 0 &&
          report.counters.retries == 0,
      "deferred mechanism counters remain zero");

  const std::string formatted = format_report(report);
  expect(
      formatted == read_file(data_path("two_layer_golden.report")),
      "formatted report matches golden file");
  expect(
      formatted == format_report(simulator.run(workload)),
      "repeated execution is deterministic");
}

}  // namespace

int main() {
  test_typed_parser_and_rejections();
  test_allreduce_lowering();
  test_collective_planner_selection_and_provenance();
  test_schedule_provider_injection();
  test_transport_backend_injection();
  test_mscclpp_plan_translation();
  test_signal_wait_runtime();
  test_get_request_response_runtime();
  test_serialization_hand_calculation();
  test_gpu_resource_scheduling();
  test_collective_coverage_end_to_end();
  test_multi_layer_overlap_and_global_identity();
  test_aicb_subset_import();
  test_transaction_segmentation_and_pdu_serialization();
  test_multi_rail_timing_and_conservation();
  test_finite_switch_queue_and_backpressure();
  test_integrated_switch_conservation();
  test_credit_conservation_and_starvation();
  test_retry_reassembly_and_exactly_once_completion();
  test_golden_end_to_end();

  if (failures != 0) {
    std::cerr << failures << " vertical-slice test(s) failed\n";
    return 1;
  }
  std::cout << "All native runtime tests passed\n";
  return 0;
}
