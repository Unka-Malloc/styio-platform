include(FetchContent)
include(GoogleTest)

set(STYIO_PLATFORM_PYTHON3 "" CACHE FILEPATH "Python 3 interpreter for styio-platform test scripts")

set(STYIO_PLATFORM_PYTHON3_CANDIDATES)
if(STYIO_PLATFORM_PYTHON3)
  list(APPEND STYIO_PLATFORM_PYTHON3_CANDIDATES "${STYIO_PLATFORM_PYTHON3}")
endif()
if(DEFINED ENV{STYIO_PLATFORM_PYTHON3} AND NOT "$ENV{STYIO_PLATFORM_PYTHON3}" STREQUAL "")
  list(APPEND STYIO_PLATFORM_PYTHON3_CANDIDATES "$ENV{STYIO_PLATFORM_PYTHON3}")
endif()
find_program(STYIO_PLATFORM_PATH_PYTHON3 NAMES python3)
foreach(candidate
    /usr/bin/python3
    /usr/local/bin/python3
    /opt/homebrew/bin/python3
    /Applications/Xcode.app/Contents/Developer/Library/Frameworks/Python3.framework/Versions/3.9/bin/python3.9
    ${STYIO_PLATFORM_PATH_PYTHON3})
  if(candidate)
    list(APPEND STYIO_PLATFORM_PYTHON3_CANDIDATES "${candidate}")
  endif()
endforeach()
list(REMOVE_DUPLICATES STYIO_PLATFORM_PYTHON3_CANDIDATES)

set(STYIO_PLATFORM_SELECTED_PYTHON3 "")
foreach(candidate IN LISTS STYIO_PLATFORM_PYTHON3_CANDIDATES)
  if(EXISTS "${candidate}")
    execute_process(
      COMMAND "${candidate}" -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 11) else 1)"
      RESULT_VARIABLE STYIO_PLATFORM_PYTHON3_STATUS
      OUTPUT_QUIET
      ERROR_QUIET
    )
    if(STYIO_PLATFORM_PYTHON3_STATUS EQUAL 0)
      set(STYIO_PLATFORM_SELECTED_PYTHON3 "${candidate}")
      break()
    endif()
  endif()
endforeach()

if(NOT STYIO_PLATFORM_SELECTED_PYTHON3)
  message(FATAL_ERROR "Python 3.11+ interpreter is required for styio-platform test scripts. Set STYIO_PLATFORM_PYTHON3 to a working interpreter.")
endif()
set(STYIO_PLATFORM_PYTHON3 "${STYIO_PLATFORM_SELECTED_PYTHON3}" CACHE FILEPATH "Python 3 interpreter for styio-platform test scripts" FORCE)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
  DOWNLOAD_EXTRACT_TIMESTAMP true
)

FetchContent_MakeAvailable(googletest)

add_executable(styio_platform_native_tests
  native/PlatformKernelTests.cpp
)
target_link_libraries(styio_platform_native_tests PRIVATE styio_platform_kernel GTest::gtest_main)

gtest_discover_tests(styio_platform_native_tests DISCOVERY_TIMEOUT 30)

add_test(
  NAME styio_platform_cloud_compile_stress_unit
  COMMAND "${STYIO_PLATFORM_PYTHON3}" "${CMAKE_SOURCE_DIR}/tests/unit/test_cloud_compile_stress.py"
)

add_test(
  NAME styio_platform_registry_v2_unit
  COMMAND "${STYIO_PLATFORM_PYTHON3}" "${CMAKE_SOURCE_DIR}/tests/unit/test_registry_v2.py"
)

add_test(
  NAME styio_platform_registry_vm_deploy_unit
  COMMAND "${STYIO_PLATFORM_PYTHON3}" "${CMAKE_SOURCE_DIR}/tests/unit/test_registry_vm_deploy.py"
)

add_test(
  NAME styio_platform_tool_release_unit
  COMMAND "${STYIO_PLATFORM_PYTHON3}" "${CMAKE_SOURCE_DIR}/tests/unit/test_tool_release.py"
)

add_test(
  NAME styio_platform_registry_v2_contract
  COMMAND "${STYIO_PLATFORM_PYTHON3}" "${CMAKE_SOURCE_DIR}/tests/interop/registry-v2-contract-gate.py"
)

add_test(
  NAME styio_platform_registry_control_plane_contract
  COMMAND "${STYIO_PLATFORM_PYTHON3}" "${CMAKE_SOURCE_DIR}/tests/interop/registry-control-plane-contract-gate.py"
)

add_test(
  NAME styio_platform_hosted_api_contract_unit
  COMMAND "${STYIO_PLATFORM_PYTHON3}" "${CMAKE_SOURCE_DIR}/tests/interop/hosted-control-plane-contract-gate.py" --mode unit
)

add_test(
  NAME styio_platform_hosted_api_contract_integration
  COMMAND "${STYIO_PLATFORM_PYTHON3}" "${CMAKE_SOURCE_DIR}/tests/interop/hosted-control-plane-contract-gate.py" --mode integration
)

add_test(
  NAME styio_platform_hosted_api_contract_regression
  COMMAND "${STYIO_PLATFORM_PYTHON3}" "${CMAKE_SOURCE_DIR}/tests/interop/hosted-control-plane-contract-gate.py" --mode regression
)

add_test(
  NAME styio_platform_hosted_api_contract_smoke
  COMMAND "${STYIO_PLATFORM_PYTHON3}" "${CMAKE_SOURCE_DIR}/tests/interop/hosted-control-plane-contract-gate.py" --mode smoke
)

add_test(
  NAME styio_platform_hosted_api_contract_fuzz
  COMMAND "${STYIO_PLATFORM_PYTHON3}" "${CMAKE_SOURCE_DIR}/tests/interop/hosted-control-plane-contract-gate.py" --mode fuzz
)

add_test(
  NAME styio_platform_control_plane_contract_unit
  COMMAND "${STYIO_PLATFORM_PYTHON3}" "${CMAKE_SOURCE_DIR}/tests/interop/platform-control-plane-contract-gate.py" --mode unit
)

add_test(
  NAME styio_platform_control_plane_contract_integration
  COMMAND "${STYIO_PLATFORM_PYTHON3}" "${CMAKE_SOURCE_DIR}/tests/interop/platform-control-plane-contract-gate.py" --mode integration
)

add_test(
  NAME styio_platform_control_plane_contract_regression
  COMMAND "${STYIO_PLATFORM_PYTHON3}" "${CMAKE_SOURCE_DIR}/tests/interop/platform-control-plane-contract-gate.py" --mode regression
)

add_test(
  NAME styio_platform_control_plane_contract_smoke
  COMMAND "${STYIO_PLATFORM_PYTHON3}" "${CMAKE_SOURCE_DIR}/tests/interop/platform-control-plane-contract-gate.py" --mode smoke
)

add_test(
  NAME styio_platform_control_plane_contract_fuzz
  COMMAND "${STYIO_PLATFORM_PYTHON3}" "${CMAKE_SOURCE_DIR}/tests/interop/platform-control-plane-contract-gate.py" --mode fuzz
)

add_test(
  NAME styio_platform_http_smoke
  COMMAND "${CMAKE_COMMAND}" -E env "STYIO_PLATFORMD_BIN=$<TARGET_FILE:styio-platformd>" "${STYIO_PLATFORM_PYTHON3}" "${CMAKE_SOURCE_DIR}/tests/interop/platform-http-smoke.py"
)

add_test(
  NAME styio_platform_native_contract_source_gate
  COMMAND "${STYIO_PLATFORM_PYTHON3}" "${CMAKE_SOURCE_DIR}/tests/interop/native-contract-source-gate.py"
)
