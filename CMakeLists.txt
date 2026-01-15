cmake_minimum_required(VERSION 3.15)
project(alloc8
  VERSION 1.0.0
  LANGUAGES CXX C
  DESCRIPTION "Generic allocator interposition library"
)

# ─── DEFAULT BUILD TYPE ───────────────────────────────────────────────────────
# Default to Release build for optimal performance
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to 'Release' as none was specified.")
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# ─── OPTIONS ───────────────────────────────────────────────────────────────────
option(ALLOC8_BUILD_TESTS "Build alloc8 tests" OFF)
option(ALLOC8_BUILD_EXAMPLES "Build alloc8 examples" OFF)
option(ALLOC8_WINDOWS_USE_DETOURS "Use Microsoft Detours on Windows (recommended)" ON)
option(ALLOC8_WINDOWS_USE_SYSTEM_DETOURS "Use system-installed Detours instead of fetching" OFF)

set(ALLOC8_PREFIX "" CACHE STRING "Prefix for prefixed mode (e.g., 'hoard' -> hoard_malloc)")

# ─── PLATFORM DETECTION ────────────────────────────────────────────────────────
set(ALLOC8_PLATFORM_LINUX OFF)
set(ALLOC8_PLATFORM_MACOS OFF)
set(ALLOC8_PLATFORM_WINDOWS OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(ALLOC8_PLATFORM_LINUX ON)
elseif(APPLE)
  set(ALLOC8_PLATFORM_MACOS ON)
elseif(WIN32)
  set(ALLOC8_PLATFORM_WINDOWS ON)
endif()

# ─── C++ STANDARD ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ─── COMPILER FLAGS ────────────────────────────────────────────────────────────
if(NOT WIN32)
  add_compile_options(
    -fvisibility=hidden
    -fno-builtin-malloc
    -fno-builtin-free
    -fno-builtin-realloc
    -fno-builtin-calloc
  )
endif()

# Enable LTO if supported
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# ─── HEADER-ONLY INTERFACE LIBRARY ─────────────────────────────────────────────
add_library(alloc8_headers INTERFACE)
add_library(alloc8::headers ALIAS alloc8_headers)
target_include_directories(alloc8_headers INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(alloc8_headers INTERFACE cxx_std_20)

# ─── COMMON WRAPPER LIBRARY (OBJECT) ───────────────────────────────────────────
# On Windows, operator new/delete are hooked via Detours, not by redefining them
if(WIN32)
  add_library(alloc8_common OBJECT
    src/common/wrapper_common.cpp
  )
else()
  add_library(alloc8_common OBJECT
    src/common/wrapper_common.cpp
    src/common/new_delete.cpp
  )
endif()
target_link_libraries(alloc8_common PUBLIC alloc8_headers)
set_target_properties(alloc8_common PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

# ─── PLATFORM-SPECIFIC INTERPOSITION ───────────────────────────────────────────
if(ALLOC8_PLATFORM_LINUX)
  # Generate version script from template
  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/linux/version_script.map.in
    ${CMAKE_CURRENT_BINARY_DIR}/version_script.map
    @ONLY
  )

  add_library(alloc8_interpose INTERFACE)
  add_library(alloc8::interpose ALIAS alloc8_interpose)

  # Store paths for users to include in their targets
  set(ALLOC8_INTERPOSE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/linux/gnu_wrapper.cpp
    CACHE INTERNAL "Platform interpose sources"
  )
  # Optional thread lifecycle hooks - allocators can include this for pthread interposition
  set(ALLOC8_THREAD_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/linux/linux_threads.cpp
    CACHE INTERNAL "Optional thread interposition sources"
  )
  set(ALLOC8_VERSION_SCRIPT
    ${CMAKE_CURRENT_BINARY_DIR}/version_script.map
    CACHE INTERNAL "Version script path"
  )
  set(ALLOC8_COMMON_SOURCES
    $<TARGET_OBJECTS:alloc8_common>
    CACHE INTERNAL "Common wrapper object files"
  )

  target_link_libraries(alloc8_interpose INTERFACE
    alloc8_headers
    pthread
    dl
  )
  target_link_options(alloc8_interpose INTERFACE
    "LINKER:--version-script=${ALLOC8_VERSION_SCRIPT}"
    "-Bsymbolic"
  )

elseif(ALLOC8_PLATFORM_MACOS)
  add_library(alloc8_interpose INTERFACE)
  add_library(alloc8::interpose ALIAS alloc8_interpose)

  # Note: mac_zones.cpp is #included by mac_wrapper.cpp, not compiled separately
  set(ALLOC8_INTERPOSE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos/mac_wrapper.cpp
    CACHE INTERNAL "Platform interpose sources"
  )
  # Optional thread lifecycle hooks - allocators can include this for pthread interposition
  set(ALLOC8_THREAD_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos/mac_threads.cpp
    CACHE INTERNAL "Optional thread interposition sources"
  )
  set(ALLOC8_COMMON_SOURCES
    $<TARGET_OBJECTS:alloc8_common>
    CACHE INTERNAL "Common wrapper object files"
  )

  target_link_libraries(alloc8_interpose INTERFACE
    alloc8_headers
    pthread
  )
  target_compile_options(alloc8_interpose INTERFACE
    -ftls-model=initial-exec
  )

elseif(ALLOC8_PLATFORM_WINDOWS)
  # Fetch or find Microsoft Detours
  if(ALLOC8_WINDOWS_USE_DETOURS)
    if(ALLOC8_WINDOWS_USE_SYSTEM_DETOURS)
      find_package(Detours REQUIRED)
    else()
      include(FetchContent)
      FetchContent_Declare(
        Detours
        GIT_REPOSITORY https://github.com/microsoft/Detours.git
        GIT_TAG        main
        GIT_SHALLOW    TRUE
      )
      FetchContent_MakeAvailable(Detours)

      # Build Detours as static library
      set(DETOURS_SRC_DIR ${detours_SOURCE_DIR}/src)
      add_library(detours_lib STATIC
        ${DETOURS_SRC_DIR}/detours.cpp
        ${DETOURS_SRC_DIR}/modules.cpp
        ${DETOURS_SRC_DIR}/disasm.cpp
        ${DETOURS_SRC_DIR}/image.cpp
        ${DETOURS_SRC_DIR}/creatwth.cpp
        ${DETOURS_SRC_DIR}/disolx86.cpp
        ${DETOURS_SRC_DIR}/disolx64.cpp
        ${DETOURS_SRC_DIR}/disolia64.cpp
        ${DETOURS_SRC_DIR}/disolarm.cpp
        ${DETOURS_SRC_DIR}/disolarm64.cpp
      )
      target_include_directories(detours_lib PUBLIC ${DETOURS_SRC_DIR})
      target_compile_definitions(detours_lib PRIVATE WIN32_LEAN_AND_MEAN)
      add_library(Detours::Detours ALIAS detours_lib)

      # Build Detours utilities (withdll.exe, setdll.exe)
      set(DETOURS_SAMPLES_DIR ${detours_SOURCE_DIR}/samples)

      # withdll.exe - run a process with a DLL injected
      add_executable(withdll ${DETOURS_SAMPLES_DIR}/withdll/withdll.cpp)
      target_link_libraries(withdll PRIVATE detours_lib)
      target_compile_definitions(withdll PRIVATE WIN32_LEAN_AND_MEAN)

      # setdll.exe - add/remove DLL from PE import table
      add_executable(setdll ${DETOURS_SAMPLES_DIR}/setdll/setdll.cpp)
      target_link_libraries(setdll PRIVATE detours_lib)
      target_compile_definitions(setdll PRIVATE WIN32_LEAN_AND_MEAN)
    endif()

    set(ALLOC8_INTERPOSE_SOURCES
      ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows/win_wrapper_detours.cpp
      CACHE INTERNAL "Platform interpose sources"
    )
  else()
    set(ALLOC8_INTERPOSE_SOURCES
      ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows/win_wrapper_patch.cpp
      CACHE INTERNAL "Platform interpose sources"
    )
  endif()

  # Thread lifecycle hooks for Windows - uses DllMain integration
  set(ALLOC8_THREAD_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows/win_threads.cpp
    CACHE INTERNAL "Optional thread interposition sources"
  )

  set(ALLOC8_COMMON_SOURCES
    $<TARGET_OBJECTS:alloc8_common>
    CACHE INTERNAL "Common wrapper object files"
  )

  add_library(alloc8_interpose INTERFACE)
  add_library(alloc8::interpose ALIAS alloc8_interpose)
  target_link_libraries(alloc8_interpose INTERFACE
    alloc8_headers
    $<$<BOOL:${ALLOC8_WINDOWS_USE_DETOURS}>:Detours::Detours>
    psapi
  )
endif()

# ─── PREFIXED MODE LIBRARY ─────────────────────────────────────────────────────
if(ALLOC8_PREFIX)
  string(TOUPPER ${ALLOC8_PREFIX} ALLOC8_PREFIX_UPPER)

  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/prefixed/prefixed_api.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/alloc8/${ALLOC8_PREFIX}_malloc.h
    @ONLY
  )
  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/prefixed/prefixed_api.cpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/${ALLOC8_PREFIX}_malloc.cpp
    @ONLY
  )

  add_library(alloc8_prefixed_${ALLOC8_PREFIX} STATIC
    ${CMAKE_CURRENT_BINARY_DIR}/${ALLOC8_PREFIX}_malloc.cpp
  )
  target_link_libraries(alloc8_prefixed_${ALLOC8_PREFIX} PUBLIC alloc8_headers)
  target_include_directories(alloc8_prefixed_${ALLOC8_PREFIX} PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}/include
  )
  add_library(alloc8::prefixed ALIAS alloc8_prefixed_${ALLOC8_PREFIX})
endif()

# ─── TESTS ─────────────────────────────────────────────────────────────────────
if(ALLOC8_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ─── EXAMPLES ──────────────────────────────────────────────────────────────────
if(ALLOC8_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# ─── INSTALL ───────────────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(DIRECTORY include/alloc8
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(TARGETS alloc8_headers
  EXPORT alloc8Targets
)

install(EXPORT alloc8Targets
  FILE alloc8Targets.cmake
  NAMESPACE alloc8::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/alloc8
)

# Generate and install package config
include(CMakePackageConfigHelpers)
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/alloc8Config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/alloc8Config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/alloc8
)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/alloc8ConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/alloc8Config.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/alloc8ConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/alloc8
)
