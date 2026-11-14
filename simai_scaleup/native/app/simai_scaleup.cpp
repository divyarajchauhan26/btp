#include <exception>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

#include "simai/runtime/aicb_importer.hpp"
#include "simai/runtime/chakra_et_importer.hpp"
#include "simai/runtime/fine_chakra_manifest.hpp"
#include "simai/runtime/fine_simulator.hpp"
#include "simai/runtime/fine_synthetic_trace.hpp"
#include "simai/runtime/fine_synthetic_manifest.hpp"
#include "simai/runtime/mscclpp_plan.hpp"
#include "simai/runtime/report_writer.hpp"
#include "simai/runtime/simulator.hpp"
#include "simai/runtime/workload.hpp"

namespace {

enum class GpuModelMode { Coarse, Fine };

simai::runtime::FineSimulatorConfig fine_config(
    const simai::runtime::WorkloadSpec& workload) {
  if (workload.gpu.enabled != 1) {
    throw std::invalid_argument(
        "fine GPU model requires gpu_enabled=1 and explicit GPU resources");
  }
  const std::size_t memory_capacity =
      static_cast<std::size_t>(workload.gpu.compute_units_per_device) *
      workload.gpu.workgroup_slots_per_compute_unit *
      workload.gpu.wavefronts_per_workgroup * 64;
  return simai::runtime::FineSimulatorConfig{
      simai::runtime::FineGpuSchedulerConfig{
          workload.endpoint_count,
          workload.gpu.compute_units_per_device,
          workload.gpu.workgroup_slots_per_compute_unit,
          simai::contracts::FineSchedulingPolicy::RoundRobin,
          workload.deterministic_seed},
      simai::runtime::FineWavefrontEngineConfig{
          workload.endpoint_count,
          workload.gpu.compute_units_per_device,
          workload.gpu.wavefront_issue_width,
          workload.gpu.wavefront_issue_latency_ns,
          1,
          simai::runtime::FineInstructionGeneratorConfig{
              64, 4, simai::contracts::BufferId(9000001)}},
      simai::runtime::FineLocalMemorySystemConfig{
          workload.endpoint_count,
          {simai::runtime::FineLocalMemoryTierConfig{
               simai::contracts::FineMemorySpace::L1,
               1,
               64,
               memory_capacity,
               memory_capacity},
           simai::runtime::FineLocalMemoryTierConfig{
               simai::contracts::FineMemorySpace::L2,
               5,
               32,
               memory_capacity,
               memory_capacity},
           simai::runtime::FineLocalMemoryTierConfig{
               simai::contracts::FineMemorySpace::LocalHbm,
               20,
               16,
               memory_capacity,
               memory_capacity}}},
      workload.link,
      simai::contracts::BufferId(9000002)};
}

void print_usage() {
  std::cerr
      << "usage: simai_scaleup [--gpu-model coarse|fine] "
         "<native-workload-file>\n"
      << "   or: simai_scaleup [--gpu-model coarse|fine] --aicb "
         "<aicb-file> <native-base-config> <nanoseconds-per-tick>\n"
      << "   or: simai_scaleup [--gpu-model coarse|fine] --mscclpp "
         "<plan-json> <native-workload-file>\n"
      << "   or: simai_scaleup --chakra <trace.et> <device-id>\n"
      << "   or: simai_scaleup --gpu-model fine --chakra-fine "
         "<execution-manifest> <hardware-workload>\n"
      << "   or: simai_scaleup --gpu-model fine --synthetic-trace "
         "<trace-file> <hardware-workload>\n"
      << "   or: simai_scaleup --gpu-model fine --synthetic-manifest "
         "<manifest-file> <hardware-workload>\n"
      << "   or: simai_scaleup --inspect-synthetic-trace <trace-file>\n";
}

}  // namespace

int main(int argc, char** argv) {
  int command_index = 1;
  GpuModelMode gpu_model = GpuModelMode::Coarse;
  if (argc >= 3 && std::string(argv[1]) == "--gpu-model") {
    const std::string selected_model(argv[2]);
    if (selected_model == "coarse") {
      gpu_model = GpuModelMode::Coarse;
    } else if (selected_model == "fine") {
      gpu_model = GpuModelMode::Fine;
    } else {
      std::cerr << "error: GPU model must be coarse or fine\n";
      print_usage();
      return 2;
    }
    command_index = 3;
  }

  const int remaining = argc - command_index;
  const bool native_mode = remaining == 1;
  const bool aicb_mode =
      remaining == 4 && std::string(argv[command_index]) == "--aicb";
  const bool mscclpp_mode =
      remaining == 3 && std::string(argv[command_index]) == "--mscclpp";
  const bool chakra_mode =
      remaining == 3 && std::string(argv[command_index]) == "--chakra";
  const bool chakra_fine_mode =
      remaining == 3 && std::string(argv[command_index]) == "--chakra-fine";
  const bool synthetic_trace_mode =
      remaining == 3 &&
      std::string(argv[command_index]) == "--synthetic-trace";
  const bool synthetic_manifest_mode =
      remaining == 3 &&
      std::string(argv[command_index]) == "--synthetic-manifest";
  const bool inspect_synthetic_trace_mode =
      remaining == 2 &&
      std::string(argv[command_index]) == "--inspect-synthetic-trace";
  if (!native_mode && !aicb_mode && !mscclpp_mode && !chakra_mode &&
      !chakra_fine_mode && !synthetic_trace_mode && !synthetic_manifest_mode &&
      !inspect_synthetic_trace_mode) {
    print_usage();
    return 2;
  }
  try {
    if (inspect_synthetic_trace_mode) {
      const simai::runtime::FineSyntheticTrace trace =
          simai::runtime::parse_fine_synthetic_trace_file(
              argv[command_index + 1]);
      std::cout << simai::runtime::format_fine_synthetic_trace_summary(trace);
      return 0;
    }
    if (chakra_mode) {
      std::size_t consumed = 0;
      if (argv[command_index + 2][0] == '-') {
        throw std::invalid_argument(
            "Chakra device-id must be a nonzero unsigned integer");
      }
      const std::uint64_t device =
          std::stoull(argv[command_index + 2], &consumed, 10);
      if (consumed != std::string(argv[command_index + 2]).size() ||
          device == 0) {
        throw std::invalid_argument(
            "Chakra device-id must be a nonzero unsigned integer");
      }
      const simai::runtime::FineKernelGraph graph =
          simai::runtime::import_chakra_et_file(
              argv[command_index + 1],
              simai::runtime::ChakraEtImportOptions{
                  simai::contracts::DeviceId(device), 64ULL * 1024 * 1024});
      std::cout << simai::runtime::format_fine_kernel_graph(graph);
      return 0;
    }
    if (chakra_fine_mode) {
      if (gpu_model != GpuModelMode::Fine) {
        throw std::invalid_argument(
            "--chakra-fine requires --gpu-model fine");
      }
      const simai::runtime::WorkloadSpec hardware =
          simai::runtime::parse_workload_file(argv[command_index + 2]);
      const simai::runtime::FineChakraExecutionProgram program =
          simai::runtime::load_fine_chakra_execution_manifest(
              argv[command_index + 1], 64ULL * 1024 * 1024);
      for (const simai::runtime::FineKernelGraphNode& node :
           program.graph.nodes) {
        if (node.device.value() > hardware.endpoint_count) {
          throw std::invalid_argument(
              "Chakra trace device exceeds hardware endpoint count");
        }
      }
      std::cout << simai::runtime::format_fine_report(
          simai::runtime::FineScaleUpSimulator().run(
              program.execution, fine_config(hardware)));
      return 0;
    }
    if (synthetic_trace_mode) {
      if (gpu_model != GpuModelMode::Fine) {
        throw std::invalid_argument(
            "--synthetic-trace requires --gpu-model fine");
      }
      const simai::runtime::FineSyntheticTrace trace =
          simai::runtime::parse_fine_synthetic_trace_file(
              argv[command_index + 1]);
      const simai::runtime::WorkloadSpec hardware =
          simai::runtime::parse_workload_file(argv[command_index + 2]);
      if (trace.device.value() > hardware.endpoint_count) {
        throw std::invalid_argument(
            "synthetic trace device exceeds hardware endpoint count");
      }
      for (const simai::runtime::FineSyntheticTraceRecord& record :
           trace.records) {
        if (record.owner.value() > hardware.endpoint_count) {
          throw std::invalid_argument(
              "synthetic trace owner exceeds hardware endpoint count");
        }
      }
      const simai::runtime::FineGraphExecutionPlan execution{
          {simai::runtime::FineGraphExecutionNode{
              simai::contracts::WorkloadEventId(1),
              trace.kernel_name,
              {simai::contracts::WorkloadEventId(1)},
              {},
              {trace.kernel}}}};
      std::cout << simai::runtime::format_fine_report(
          simai::runtime::FineScaleUpSimulator().run(
              execution, fine_config(hardware)));
      return 0;
    }
    if (synthetic_manifest_mode) {
      if (gpu_model != GpuModelMode::Fine) {
        throw std::invalid_argument(
            "--synthetic-manifest requires --gpu-model fine");
      }
      const simai::runtime::FineSyntheticExecutionProgram program =
          simai::runtime::load_fine_synthetic_execution_manifest(
              argv[command_index + 1]);
      const simai::runtime::WorkloadSpec hardware =
          simai::runtime::parse_workload_file(argv[command_index + 2]);
      std::cout << simai::runtime::format_fine_report(
          simai::runtime::FineScaleUpSimulator().run(
              program.execution, fine_config(hardware)));
      return 0;
    }
    simai::runtime::WorkloadSpec workload;
    if (native_mode) {
      workload = simai::runtime::parse_workload_file(argv[command_index]);
    } else if (mscclpp_mode) {
      workload = simai::runtime::parse_workload_file(argv[command_index + 2]);
    } else {
      const simai::runtime::WorkloadSpec base =
          simai::runtime::parse_workload_file(argv[command_index + 2]);
      std::size_t consumed = 0;
      if (argv[command_index + 3][0] == '-') {
        throw std::invalid_argument(
            "nanoseconds-per-tick must be a nonzero unsigned integer");
      }
      const std::uint64_t nanoseconds_per_tick =
          std::stoull(argv[command_index + 3], &consumed, 10);
      if (consumed != std::string(argv[command_index + 3]).size() ||
          nanoseconds_per_tick == 0) {
        throw std::invalid_argument(
            "nanoseconds-per-tick must be a nonzero unsigned integer");
      }
      workload = simai::runtime::import_aicb_workload_file(
          argv[command_index + 1],
          simai::runtime::AicbImportOptions{
              base.schema_version,
              base.deterministic_seed,
              base.collective_algorithm,
              base.link,
              base.gpu,
              nanoseconds_per_tick});
    }
    if (gpu_model == GpuModelMode::Fine) {
      if (!mscclpp_mode) {
        throw std::invalid_argument(
            "fine GPU model currently requires --mscclpp plan and workload");
      }
      const simai::runtime::LayerSpec* collective_layer = nullptr;
      for (const simai::runtime::LayerSpec& layer : workload.layers) {
        if (layer.collective == simai::runtime::LayerCollectiveKind::None) {
          continue;
        }
        if (collective_layer != nullptr) {
          throw std::invalid_argument(
              "fine MSCCL++ CLI currently supports one collective layer");
        }
        collective_layer = &layer;
      }
      if (collective_layer == nullptr) {
        throw std::invalid_argument(
            "fine MSCCL++ CLI requires one collective layer");
      }
      const simai::runtime::MscclppPlan plan =
          simai::runtime::parse_mscclpp_plan_file(argv[command_index + 1]);
      const std::uint64_t reduce_cycles =
          collective_layer->payload_bytes /
              workload.gpu.reduction_bytes_per_ns +
          (collective_layer->payload_bytes %
                   workload.gpu.reduction_bytes_per_ns ==
               0
           ? 0
           : 1);
      const std::vector<simai::contracts::FineKernelDefinition> kernels =
          simai::runtime::lower_mscclpp_fine_kernels(
              plan,
              collective_layer->payload_bytes,
              simai::runtime::MscclppFineLoweringConfig{
                  1, 64, reduce_cycles});
      std::cout << simai::runtime::format_fine_report(
          simai::runtime::FineScaleUpSimulator().run(
              kernels, fine_config(workload)));
      return 0;
    }
    simai::contracts::SimulationReport report;
    if (mscclpp_mode) {
      const simai::runtime::MscclppScheduleProvider schedule_provider(
          argv[command_index + 1]);
      report = simai::runtime::ScaleUpSimulator().run(
          workload, schedule_provider);
    } else {
      report = simai::runtime::ScaleUpSimulator().run(workload);
    }
    std::cout << simai::runtime::format_report(report);
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
