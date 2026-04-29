include(FetchContent)
include(GoogleTest)

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
  COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/unit/test_cloud_compile_stress.py"
)

add_test(
  NAME styio_platform_registry_v2_unit
  COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/unit/test_registry_v2.py"
)

add_test(
  NAME styio_platform_registry_vm_deploy_unit
  COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/unit/test_registry_vm_deploy.py"
)

add_test(
  NAME styio_platform_registry_v2_contract
  COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/interop/registry-v2-contract-gate.py"
)

add_test(
  NAME styio_platform_registry_control_plane_contract
  COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/interop/registry-control-plane-contract-gate.py"
)

add_test(
  NAME styio_platform_hosted_api_contract_unit
  COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/interop/hosted-control-plane-contract-gate.py" --mode unit
)

add_test(
  NAME styio_platform_hosted_api_contract_integration
  COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/interop/hosted-control-plane-contract-gate.py" --mode integration
)

add_test(
  NAME styio_platform_hosted_api_contract_regression
  COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/interop/hosted-control-plane-contract-gate.py" --mode regression
)

add_test(
  NAME styio_platform_hosted_api_contract_smoke
  COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/interop/hosted-control-plane-contract-gate.py" --mode smoke
)

add_test(
  NAME styio_platform_hosted_api_contract_fuzz
  COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/interop/hosted-control-plane-contract-gate.py" --mode fuzz
)

add_test(
  NAME styio_platform_control_plane_contract_unit
  COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/interop/platform-control-plane-contract-gate.py" --mode unit
)

add_test(
  NAME styio_platform_control_plane_contract_integration
  COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/interop/platform-control-plane-contract-gate.py" --mode integration
)

add_test(
  NAME styio_platform_control_plane_contract_regression
  COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/interop/platform-control-plane-contract-gate.py" --mode regression
)

add_test(
  NAME styio_platform_control_plane_contract_smoke
  COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/interop/platform-control-plane-contract-gate.py" --mode smoke
)

add_test(
  NAME styio_platform_control_plane_contract_fuzz
  COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/interop/platform-control-plane-contract-gate.py" --mode fuzz
)

add_test(
  NAME styio_platform_http_smoke
  COMMAND "${CMAKE_COMMAND}" -E env "STYIO_PLATFORMD_BIN=$<TARGET_FILE:styio-platformd>" python3 "${CMAKE_SOURCE_DIR}/tests/interop/platform-http-smoke.py"
)

add_test(
  NAME styio_platform_native_contract_source_gate
  COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/interop/native-contract-source-gate.py"
)
