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

set(STYIO_PLATFORM_SYSTEM_SOURCES
    PlatformCore/Config.cpp
    PlatformCore/Core/Paths.cpp
    PlatformCore/Core/Process.cpp
    PlatformCore/Core/Sha256.cpp
    PlatformCore/System/OperatingSystemAdapter.cpp
)

set(STYIO_PLATFORM_CORE_SOURCES
    PlatformCore/Manifest/Lockfile.cpp
    PlatformCore/Manifest/Manifest.cpp
    PlatformCore/RegistryClient/Client.cpp
    PlatformCore/Resolve/Resolver.cpp
    PlatformCore/Toolchain/SourceBuild.cpp
    PlatformCore/Toolchain/State.cpp
)

set(STYIO_PLATFORM_SECURITY_SOURCES
    PlatformSecurity/PlatformCA/CertificateAuthority.cpp
    PlatformSecurity/PlatformClientAuth/Authorization.cpp
    PlatformSecurity/PlatformClientAuth/Identity.cpp
    PlatformSecurity/SecurityHardening/RegistrySecurity.cpp
)

set(STYIO_PLATFORM_PROTOCOL_SOURCES
    SpioPlatformProtocols/CompilePlan.cpp
    SpioPlatformProtocols/Contract.cpp
    SpioPlatformProtocols/Execution.cpp
    SpioPlatformProtocols/Job.cpp
)

set(STYIO_PLATFORM_STORAGE_SOURCES
    PlatformStorage/PlatformCache/CachePaths.cpp
    PlatformStorage/PlatformPersistence/StateRecords.cpp
    PlatformStorage/PlatformPersistence/MemoryStateStore.cpp
    PlatformStorage/PlatformPersistence/ObjectStore.cpp
    PlatformStorage/PlatformPersistence/PostgresStore.cpp
)

set(STYIO_PLATFORM_PACKAGE_REGISTRY_SOURCES
    PlatformCloud/PackageRegistry/ControlPlane/RegistryRoutes.cpp
    PlatformCloud/PackageRegistry/MirrorSync/MirrorSync.cpp
)

set(STYIO_PLATFORM_DEVELOPER_WORKSPACE_SOURCES
    PlatformCloud/DeveloperWorkspace/JobQueue.cpp
    PlatformCloud/DeveloperWorkspace/Worker.cpp
    PlatformCloud/DeveloperWorkspace/WorkerRuntimeFactory.cpp
)

set(STYIO_PLATFORM_SERVICE_SOURCES
    PlatformService/BeastServer.cpp
    PlatformService/Http.cpp
    PlatformService/Router.cpp
)

styio_platform_add_static_library(styio_platform_system ${STYIO_PLATFORM_SYSTEM_SOURCES})
target_link_libraries(styio_platform_system PUBLIC OpenSSL::Crypto)

styio_platform_add_static_library(styio_platform_security ${STYIO_PLATFORM_SECURITY_SOURCES})
target_link_libraries(styio_platform_security PUBLIC styio_platform_system)

styio_platform_add_static_library(styio_platform_storage ${STYIO_PLATFORM_STORAGE_SOURCES})
target_link_libraries(styio_platform_storage PUBLIC styio_platform_system Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
if(PostgreSQL_FOUND)
  target_compile_definitions(styio_platform_storage PUBLIC STYIO_PLATFORM_HAS_LIBPQ=1)
  target_include_directories(styio_platform_storage PUBLIC ${PostgreSQL_INCLUDE_DIRS})
  target_link_libraries(styio_platform_storage PUBLIC ${PostgreSQL_LIBRARIES})
else()
  target_compile_definitions(styio_platform_storage PUBLIC STYIO_PLATFORM_HAS_LIBPQ=0)
endif()

styio_platform_add_static_library(styio_platform_core ${STYIO_PLATFORM_CORE_SOURCES})
target_link_libraries(styio_platform_core PUBLIC styio_platform_storage styio_platform_security styio_platform_system)

styio_platform_add_static_library(styio_platform_protocols ${STYIO_PLATFORM_PROTOCOL_SOURCES})
target_link_libraries(styio_platform_protocols PUBLIC styio_platform_core)

styio_platform_add_static_library(styio_platform_package_registry ${STYIO_PLATFORM_PACKAGE_REGISTRY_SOURCES})
target_link_libraries(styio_platform_package_registry PUBLIC styio_platform_core styio_platform_storage Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

styio_platform_add_static_library(styio_platform_developer_workspace ${STYIO_PLATFORM_DEVELOPER_WORKSPACE_SOURCES})
target_link_libraries(styio_platform_developer_workspace PUBLIC styio_platform_core styio_platform_protocols styio_platform_storage Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

add_library(styio_platform_cloud INTERFACE)
target_link_libraries(styio_platform_cloud INTERFACE
  styio_platform_package_registry
  styio_platform_developer_workspace
)

styio_platform_add_static_library(styio_platform_service ${STYIO_PLATFORM_SERVICE_SOURCES})
target_link_libraries(styio_platform_service PUBLIC styio_platform_cloud styio_platform_storage styio_platform_protocols styio_platform_security Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

add_library(styio_platform_kernel INTERFACE)
target_link_libraries(styio_platform_kernel INTERFACE
  styio_platform_system
  styio_platform_core
  styio_platform_security
  styio_platform_protocols
  styio_platform_storage
  styio_platform_cloud
  styio_platform_service
)

add_executable(styio-platformd PlatformService/Daemon.cpp)
target_link_libraries(styio-platformd PRIVATE styio_platform_service)
