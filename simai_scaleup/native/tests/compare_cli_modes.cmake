if(NOT DEFINED SIMAI_EXECUTABLE OR NOT DEFINED SIMAI_DATA_DIR)
  message(FATAL_ERROR "SIMAI_EXECUTABLE and SIMAI_DATA_DIR are required")
endif()

set(workload "${SIMAI_DATA_DIR}/four_gpu_allgather.workload")
set(plan "${SIMAI_DATA_DIR}/mscclpp_four_gpu_allgather.json")
set(golden "${SIMAI_DATA_DIR}/four_gpu_mscclpp_golden.report")

execute_process(
  COMMAND "${SIMAI_EXECUTABLE}" --mscclpp "${plan}" "${workload}"
  RESULT_VARIABLE implicit_result
  OUTPUT_VARIABLE implicit_output
  ERROR_VARIABLE implicit_error)
if(NOT implicit_result EQUAL 0)
  message(FATAL_ERROR "implicit coarse run failed: ${implicit_error}")
endif()

execute_process(
  COMMAND "${SIMAI_EXECUTABLE}" --gpu-model coarse --mscclpp
    "${plan}" "${workload}"
  RESULT_VARIABLE explicit_result
  OUTPUT_VARIABLE explicit_output
  ERROR_VARIABLE explicit_error)
if(NOT explicit_result EQUAL 0)
  message(FATAL_ERROR "explicit coarse run failed: ${explicit_error}")
endif()

if(NOT implicit_output STREQUAL explicit_output)
  message(FATAL_ERROR "explicit coarse mode changed legacy output")
endif()

file(READ "${golden}" golden_output)
if(NOT implicit_output STREQUAL golden_output)
  message(FATAL_ERROR "four-GPU MSCCL++ report changed from golden baseline")
endif()
