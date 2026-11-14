add_library(simai_contracts
  src/collective.cpp
  src/completion.cpp
  src/event_queue.cpp
  src/fine_gpu.cpp
  src/fine_memory.cpp
  src/fine_remote.cpp
  src/sue_transaction.cpp
)

add_library(simai_runtime
  src/aicb_importer.cpp
  src/allreduce_planner.cpp
  src/chakra_et_importer.cpp
  src/collective_planner.cpp
  src/finite_switch.cpp
  src/fine_compute.cpp
  src/fine_chakra_manifest.cpp
  src/fine_graph_execution.cpp
  src/fine_instruction_stream.cpp
  src/fine_kernel_graph.cpp
  src/fine_local_memory.cpp
  src/fine_microtrace.cpp
  src/fine_remote_adapter.cpp
  src/fine_simulator.cpp
  src/fine_synchronization.cpp
  src/fine_synthetic_trace.cpp
  src/fine_synthetic_manifest.cpp
  src/fine_gpu_scheduler.cpp
  src/gpu_execution.cpp
  src/fine_wavefront_engine.cpp
  src/multi_rail_fabric.cpp
  src/mscclpp_plan.cpp
  src/reassembly.cpp
  src/report_writer.cpp
  src/simple_sue_link.cpp
  src/simulator.cpp
  src/sue_pdu.cpp
  src/transport_backend.cpp
  src/workload_parser.cpp
)

option(SIMAI_ENABLE_SUE_SIM "Build the concrete SUE-Sim/ns-3 backend" OFF)
set(SIMAI_SUE_SIM_INCLUDE_DIRS "" CACHE STRING
    "Configured ns-3/SUE-Sim include directories")
set(SIMAI_SUE_SIM_LIBRARIES "" CACHE STRING
    "Configured ns-3/SUE-Sim libraries or targets")
if(SIMAI_ENABLE_SUE_SIM)
  if(NOT SIMAI_SUE_SIM_INCLUDE_DIRS OR NOT SIMAI_SUE_SIM_LIBRARIES)
    message(FATAL_ERROR
      "SIMAI_ENABLE_SUE_SIM requires configured include directories and libraries")
  endif()
  target_sources(simai_runtime PRIVATE src/sue_sim_transport_backend.cpp)
  target_include_directories(simai_runtime SYSTEM PUBLIC
    ${SIMAI_SUE_SIM_INCLUDE_DIRS})
  target_link_libraries(simai_runtime PUBLIC ${SIMAI_SUE_SIM_LIBRARIES})
  target_compile_features(simai_runtime PUBLIC cxx_std_20)
  target_compile_definitions(simai_runtime PUBLIC SIMAI_ENABLE_SUE_SIM=1)
endif()

target_include_directories(simai_runtime PUBLIC include)
target_link_libraries(simai_runtime PUBLIC simai_contracts)
target_compile_features(simai_runtime PUBLIC cxx_std_14)
set_target_properties(simai_runtime PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(simai_contracts PUBLIC include)
target_compile_features(simai_contracts PUBLIC cxx_std_14)
set_target_properties(simai_contracts PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
  target_compile_options(simai_contracts PRIVATE /W4 /WX)
else()
  target_compile_options(simai_contracts PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(MSVC)
  target_compile_options(simai_runtime PRIVATE /W4 /WX)
else()
  target_compile_options(simai_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(simai_contracts_test tests/contracts_test.cpp)
target_link_libraries(simai_contracts_test PRIVATE simai_contracts)
set_target_properties(simai_contracts_test PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
  target_compile_options(simai_contracts_test PRIVATE /W4 /WX)
else()
  target_compile_options(simai_contracts_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME simai_contracts_test COMMAND simai_contracts_test)

add_executable(simai_vertical_slice_test tests/vertical_slice_test.cpp)
target_link_libraries(simai_vertical_slice_test PRIVATE simai_runtime)
target_compile_definitions(simai_vertical_slice_test PRIVATE
  SIMAI_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
set_target_properties(simai_vertical_slice_test PROPERTIES CXX_EXTENSIONS OFF)

add_executable(simai_fine_instruction_test tests/fine_instruction_test.cpp)
target_link_libraries(simai_fine_instruction_test PRIVATE simai_runtime)
set_target_properties(simai_fine_instruction_test PROPERTIES CXX_EXTENSIONS OFF)

add_executable(simai_fine_gpu_scheduler_test tests/fine_gpu_scheduler_test.cpp)
target_link_libraries(simai_fine_gpu_scheduler_test PRIVATE simai_runtime)
set_target_properties(simai_fine_gpu_scheduler_test PROPERTIES CXX_EXTENSIONS OFF)

add_executable(simai_fine_wavefront_engine_test
  tests/fine_wavefront_engine_test.cpp)
target_link_libraries(simai_fine_wavefront_engine_test PRIVATE simai_runtime)
set_target_properties(simai_fine_wavefront_engine_test PROPERTIES
  CXX_EXTENSIONS OFF)

add_executable(simai_fine_local_memory_test tests/fine_local_memory_test.cpp)
target_link_libraries(simai_fine_local_memory_test PRIVATE simai_runtime)
set_target_properties(simai_fine_local_memory_test PROPERTIES CXX_EXTENSIONS OFF)

add_executable(simai_fine_synchronization_test
  tests/fine_synchronization_test.cpp)
target_link_libraries(simai_fine_synchronization_test PRIVATE simai_runtime)
set_target_properties(simai_fine_synchronization_test PROPERTIES
  CXX_EXTENSIONS OFF)

add_executable(simai_fine_remote_test tests/fine_remote_test.cpp)
target_link_libraries(simai_fine_remote_test PRIVATE simai_runtime)
set_target_properties(simai_fine_remote_test PROPERTIES CXX_EXTENSIONS OFF)

add_executable(simai_fine_mscclpp_lowering_test
  tests/fine_mscclpp_lowering_test.cpp)
target_link_libraries(simai_fine_mscclpp_lowering_test PRIVATE simai_runtime)
target_compile_definitions(simai_fine_mscclpp_lowering_test PRIVATE
  SIMAI_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
set_target_properties(simai_fine_mscclpp_lowering_test PROPERTIES
  CXX_EXTENSIONS OFF)

add_executable(simai_fine_compute_test tests/fine_compute_test.cpp)
target_link_libraries(simai_fine_compute_test PRIVATE simai_runtime)
set_target_properties(simai_fine_compute_test PROPERTIES CXX_EXTENSIONS OFF)

add_executable(simai_fine_simulator_test tests/fine_simulator_test.cpp)
target_link_libraries(simai_fine_simulator_test PRIVATE simai_runtime)
target_compile_definitions(simai_fine_simulator_test PRIVATE
  SIMAI_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
set_target_properties(simai_fine_simulator_test PROPERTIES CXX_EXTENSIONS OFF)

add_executable(simai_fine_kernel_graph_test tests/fine_kernel_graph_test.cpp)
target_link_libraries(simai_fine_kernel_graph_test PRIVATE simai_runtime)
set_target_properties(simai_fine_kernel_graph_test PROPERTIES CXX_EXTENSIONS OFF)

add_executable(simai_fine_graph_execution_test
  tests/fine_graph_execution_test.cpp)
target_link_libraries(simai_fine_graph_execution_test PRIVATE simai_runtime)
target_compile_definitions(simai_fine_graph_execution_test PRIVATE
  SIMAI_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
set_target_properties(simai_fine_graph_execution_test PROPERTIES
  CXX_EXTENSIONS OFF)

add_executable(simai_chakra_et_importer_test
  tests/chakra_et_importer_test.cpp)
target_link_libraries(simai_chakra_et_importer_test PRIVATE simai_runtime)
set_target_properties(simai_chakra_et_importer_test PROPERTIES
  CXX_EXTENSIONS OFF)

add_executable(simai_fine_chakra_manifest_test
  tests/fine_chakra_manifest_test.cpp)
target_link_libraries(simai_fine_chakra_manifest_test PRIVATE simai_runtime)
target_compile_definitions(simai_fine_chakra_manifest_test PRIVATE
  SIMAI_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
set_target_properties(simai_fine_chakra_manifest_test PROPERTIES
  CXX_EXTENSIONS OFF)

add_executable(simai_fine_microtrace_test tests/fine_microtrace_test.cpp)
target_link_libraries(simai_fine_microtrace_test PRIVATE simai_runtime)
set_target_properties(simai_fine_microtrace_test PROPERTIES CXX_EXTENSIONS OFF)

add_executable(simai_fine_synthetic_trace_test
  tests/fine_synthetic_trace_test.cpp)
target_link_libraries(simai_fine_synthetic_trace_test PRIVATE simai_runtime)
set_target_properties(simai_fine_synthetic_trace_test PROPERTIES
  CXX_EXTENSIONS OFF)

add_executable(simai_fine_synthetic_manifest_test
  tests/fine_synthetic_manifest_test.cpp)
target_link_libraries(simai_fine_synthetic_manifest_test PRIVATE simai_runtime)
target_compile_definitions(simai_fine_synthetic_manifest_test PRIVATE
  SIMAI_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
set_target_properties(simai_fine_synthetic_manifest_test PROPERTIES
  CXX_EXTENSIONS OFF)

add_executable(simai_scaleup app/simai_scaleup.cpp)
target_link_libraries(simai_scaleup PRIVATE simai_runtime)
set_target_properties(simai_scaleup PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
  target_compile_options(simai_vertical_slice_test PRIVATE /W4 /WX)
  target_compile_options(simai_fine_instruction_test PRIVATE /W4 /WX)
  target_compile_options(simai_fine_gpu_scheduler_test PRIVATE /W4 /WX)
  target_compile_options(simai_fine_wavefront_engine_test PRIVATE /W4 /WX)
  target_compile_options(simai_fine_local_memory_test PRIVATE /W4 /WX)
  target_compile_options(simai_fine_synchronization_test PRIVATE /W4 /WX)
  target_compile_options(simai_fine_remote_test PRIVATE /W4 /WX)
  target_compile_options(simai_fine_mscclpp_lowering_test PRIVATE /W4 /WX)
  target_compile_options(simai_fine_compute_test PRIVATE /W4 /WX)
  target_compile_options(simai_fine_simulator_test PRIVATE /W4 /WX)
  target_compile_options(simai_fine_kernel_graph_test PRIVATE /W4 /WX)
  target_compile_options(simai_fine_graph_execution_test PRIVATE /W4 /WX)
  target_compile_options(simai_chakra_et_importer_test PRIVATE /W4 /WX)
  target_compile_options(simai_fine_chakra_manifest_test PRIVATE /W4 /WX)
  target_compile_options(simai_fine_microtrace_test PRIVATE /W4 /WX)
  target_compile_options(simai_fine_synthetic_trace_test PRIVATE /W4 /WX)
  target_compile_options(simai_fine_synthetic_manifest_test PRIVATE /W4 /WX)
  target_compile_options(simai_scaleup PRIVATE /W4 /WX)
else()
  target_compile_options(simai_vertical_slice_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(simai_fine_instruction_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(simai_fine_gpu_scheduler_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(simai_fine_wavefront_engine_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(simai_fine_local_memory_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(simai_fine_synchronization_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(simai_fine_remote_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(simai_fine_mscclpp_lowering_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(simai_fine_compute_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(simai_fine_simulator_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(simai_fine_kernel_graph_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(simai_fine_graph_execution_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(simai_chakra_et_importer_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(simai_fine_chakra_manifest_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(simai_fine_microtrace_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(simai_fine_synthetic_trace_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(simai_fine_synthetic_manifest_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(simai_scaleup PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME simai_vertical_slice_test COMMAND simai_vertical_slice_test)
add_test(NAME simai_fine_instruction_test COMMAND simai_fine_instruction_test)
add_test(NAME simai_fine_gpu_scheduler_test COMMAND simai_fine_gpu_scheduler_test)
add_test(NAME simai_fine_wavefront_engine_test
  COMMAND simai_fine_wavefront_engine_test)
add_test(NAME simai_fine_local_memory_test COMMAND simai_fine_local_memory_test)
add_test(NAME simai_fine_synchronization_test
  COMMAND simai_fine_synchronization_test)
add_test(NAME simai_fine_remote_test COMMAND simai_fine_remote_test)
add_test(NAME simai_fine_mscclpp_lowering_test
  COMMAND simai_fine_mscclpp_lowering_test)
add_test(NAME simai_fine_compute_test COMMAND simai_fine_compute_test)
add_test(NAME simai_fine_simulator_test COMMAND simai_fine_simulator_test)
add_test(NAME simai_fine_kernel_graph_test COMMAND simai_fine_kernel_graph_test)
add_test(NAME simai_fine_graph_execution_test
  COMMAND simai_fine_graph_execution_test)
add_test(NAME simai_chakra_et_importer_test
  COMMAND simai_chakra_et_importer_test)
add_test(NAME simai_fine_chakra_manifest_test
  COMMAND simai_fine_chakra_manifest_test)
add_test(NAME simai_fine_microtrace_test COMMAND simai_fine_microtrace_test)
add_test(NAME simai_fine_synthetic_trace_test
  COMMAND simai_fine_synthetic_trace_test)
add_test(NAME simai_fine_synthetic_manifest_test
  COMMAND simai_fine_synthetic_manifest_test)
add_test(
  NAME simai_fine_synthetic_trace_cli_test
  COMMAND simai_scaleup --gpu-model fine --synthetic-trace
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/synthetic_two_gpu.trace
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/fine_two_gpu.workload)
set_tests_properties(simai_fine_synthetic_trace_cli_test PROPERTIES
  PASS_REGULAR_EXPRESSION "remote_requests=3")
add_test(
  NAME simai_fine_synthetic_trace_inspect_cli_test
  COMMAND simai_scaleup --inspect-synthetic-trace
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/synthetic_v2.trace)
set_tests_properties(simai_fine_synthetic_trace_inspect_cli_test PROPERTIES
  PASS_REGULAR_EXPRESSION "synthetic_trace_schema_version=2")
add_test(
  NAME simai_fine_synthetic_manifest_cli_test
  COMMAND simai_scaleup --gpu-model fine --synthetic-manifest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/synthetic_two_gpu.manifest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/fine_two_gpu.workload)
set_tests_properties(simai_fine_synthetic_manifest_cli_test PROPERTIES
  PASS_REGULAR_EXPRESSION "remote_requests=4")
set(SIMAI_FINE_CHAKRA_MANIFEST
  "${CMAKE_CURRENT_BINARY_DIR}/fine_chakra_cli.manifest")
set(SIMAI_FINE_CHAKRA_TRACE_0
  "${CMAKE_CURRENT_BINARY_DIR}/fine_chakra_cli.0.et")
set(SIMAI_FINE_CHAKRA_TRACE_1
  "${CMAKE_CURRENT_BINARY_DIR}/fine_chakra_cli.1.et")
add_test(NAME simai_fine_chakra_fixture_writer
  COMMAND simai_fine_chakra_manifest_test --write-fixture
    ${SIMAI_FINE_CHAKRA_MANIFEST}
    ${SIMAI_FINE_CHAKRA_TRACE_0}
    ${SIMAI_FINE_CHAKRA_TRACE_1})
set_tests_properties(simai_fine_chakra_fixture_writer PROPERTIES
  FIXTURES_SETUP fine_chakra_fixture)
add_test(NAME simai_fine_chakra_cli_test
  COMMAND simai_scaleup --gpu-model fine --chakra-fine
    ${SIMAI_FINE_CHAKRA_MANIFEST}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/fine_two_gpu.workload)
set_tests_properties(simai_fine_chakra_cli_test PROPERTIES
  FIXTURES_REQUIRED fine_chakra_fixture
  PASS_REGULAR_EXPRESSION "remote_requests=9")
set(SIMAI_CHAKRA_TEST_ET
  "${CMAKE_CURRENT_BINARY_DIR}/chakra_import_test.et")
add_test(NAME simai_chakra_fixture_writer
  COMMAND simai_chakra_et_importer_test --write-fixture
    ${SIMAI_CHAKRA_TEST_ET})
set_tests_properties(simai_chakra_fixture_writer PROPERTIES
  FIXTURES_SETUP chakra_et_fixture)
add_test(NAME simai_chakra_cli_test
  COMMAND simai_scaleup --chakra ${SIMAI_CHAKRA_TEST_ET} 1)
set_tests_properties(simai_chakra_cli_test PROPERTIES
  FIXTURES_REQUIRED chakra_et_fixture
  PASS_REGULAR_EXPRESSION
    "source_format=chakra_et.*node_count=3.*topological_order=1,2,3")
add_test(
  NAME simai_mscclpp_four_gpu_cli_test
  COMMAND simai_scaleup --mscclpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/mscclpp_four_gpu_allgather.json
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/four_gpu_allgather.workload)
add_test(
  NAME simai_coarse_mode_golden_test
  COMMAND ${CMAKE_COMMAND}
    -DSIMAI_EXECUTABLE=$<TARGET_FILE:simai_scaleup>
    -DSIMAI_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/data
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_cli_modes.cmake)
add_test(
  NAME simai_fine_mode_unavailable_test
  COMMAND simai_scaleup --gpu-model fine
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/two_layer_golden.workload)
set_tests_properties(simai_fine_mode_unavailable_test PROPERTIES WILL_FAIL TRUE)
add_test(
  NAME simai_fine_mscclpp_cli_test
  COMMAND simai_scaleup --gpu-model fine --mscclpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/mscclpp_two_gpu.json
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/fine_two_gpu.workload)
