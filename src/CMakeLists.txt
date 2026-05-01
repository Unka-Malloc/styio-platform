add_library(styio_platform_build_config INTERFACE)
target_include_directories(styio_platform_build_config INTERFACE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(styio_platform_build_config INTERFACE
  "SPIO_PROJECT_ROOT=\"${CMAKE_SOURCE_DIR}\""
  "SPIO_PROJECT_VERSION=\"${PROJECT_VERSION}-platform-dev\""
  "STYIO_PLATFORM_PROJECT_ROOT=\"${CMAKE_SOURCE_DIR}\""
)
target_link_libraries(styio_platform_build_config INTERFACE tomlplusplus::tomlplusplus nlohmann_json::nlohmann_json)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PostgreSQL QUIET)

function(styio_platform_add_static_library target)
  add_library(${target} STATIC ${ARGN})
  target_link_libraries(${target} PUBLIC styio_platform_build_config)
endfunction()

set(STYIO_PLATFORM_FOUNDATION_SOURCES
    SpioCore/Paths.cpp
    SpioCore/Process.cpp
    SpioCore/Sha256.cpp
)

set(STYIO_PLATFORM_MANIFEST_SOURCES
    SpioManifest/Lockfile.cpp
    SpioManifest/Manifest.cpp
)

set(STYIO_PLATFORM_RESOLUTION_SOURCES
    SpioRegistryClient/Client.cpp
    SpioResolve/Resolver.cpp
    SpioSecurity/RegistrySecurity.cpp
)

set(STYIO_PLATFORM_COMPILE_SOURCES
    SpioPlan/CompilePlan.cpp
    SpioToolchain/State.cpp
)

set(STYIO_PLATFORM_CLOUD_SOURCES
    SpioCloud/Contract.cpp
    SpioCloud/Execution.cpp
    SpioCloud/Job.cpp
)

set(STYIO_PLATFORM_SERVICE_SOURCES
    PlatformService/BeastServer.cpp
    PlatformService/Config.cpp
    PlatformService/Http.cpp
    PlatformService/Identity.cpp
    PlatformService/JobQueue.cpp
    PlatformService/MirrorSync.cpp
    PlatformService/ObjectStore.cpp
    PlatformService/PostgresStore.cpp
    PlatformService/Router.cpp
    PlatformService/Worker.cpp
)

styio_platform_add_static_library(styio_platform_foundation ${STYIO_PLATFORM_FOUNDATION_SOURCES})

styio_platform_add_static_library(styio_platform_manifest ${STYIO_PLATFORM_MANIFEST_SOURCES})
target_link_libraries(styio_platform_manifest PUBLIC styio_platform_foundation)

styio_platform_add_static_library(styio_platform_resolution ${STYIO_PLATFORM_RESOLUTION_SOURCES})
target_link_libraries(styio_platform_resolution PUBLIC styio_platform_foundation styio_platform_manifest)

styio_platform_add_static_library(styio_platform_compile ${STYIO_PLATFORM_COMPILE_SOURCES})
target_link_libraries(styio_platform_compile PUBLIC styio_platform_foundation styio_platform_manifest styio_platform_resolution)

styio_platform_add_static_library(styio_platform_cloud ${STYIO_PLATFORM_CLOUD_SOURCES})
target_link_libraries(styio_platform_cloud PUBLIC styio_platform_compile)

styio_platform_add_static_library(styio_platform_service ${STYIO_PLATFORM_SERVICE_SOURCES})
target_link_libraries(styio_platform_service PUBLIC styio_platform_cloud Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
if(PostgreSQL_FOUND)
  target_compile_definitions(styio_platform_service PUBLIC STYIO_PLATFORM_HAS_LIBPQ=1)
  target_include_directories(styio_platform_service PUBLIC ${PostgreSQL_INCLUDE_DIRS})
  target_link_libraries(styio_platform_service PUBLIC ${PostgreSQL_LIBRARIES})
else()
  target_compile_definitions(styio_platform_service PUBLIC STYIO_PLATFORM_HAS_LIBPQ=0)
endif()

add_library(styio_platform_kernel INTERFACE)
target_link_libraries(styio_platform_kernel INTERFACE
  styio_platform_foundation
  styio_platform_manifest
  styio_platform_resolution
  styio_platform_compile
  styio_platform_cloud
  styio_platform_service
)

add_executable(styio-platformd PlatformService/Daemon.cpp)
target_link_libraries(styio-platformd PRIVATE styio_platform_service)
