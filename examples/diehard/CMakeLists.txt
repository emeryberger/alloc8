# alloc8/examples/diehard/CMakeLists.txt
# Example: DieHard allocator using alloc8 for interposition
#
# Uses local DieHard and Heap-Layers repos, or fetches via FetchContent.
# Works on Linux, macOS, and Windows.

include(FetchContent)

# Check for local repos first (with Windows fixes applied)
set(LOCAL_HEAPLAYERS_DIR "Z:/git/Heap-Layers")
set(LOCAL_DIEHARD_DIR "Z:/git/DieHard")

if(EXISTS "${LOCAL_HEAPLAYERS_DIR}/heaps/utility/uniqueheap.h" AND EXISTS "${LOCAL_DIEHARD_DIR}/src/include")
  message(STATUS "Using local Heap-Layers from ${LOCAL_HEAPLAYERS_DIR}")
  message(STATUS "Using local DieHard from ${LOCAL_DIEHARD_DIR}")
  set(heaplayers_SOURCE_DIR "${LOCAL_HEAPLAYERS_DIR}")
  set(diehard_SOURCE_DIR "${LOCAL_DIEHARD_DIR}")
else()
  # Fetch Heap-Layers
  FetchContent_Declare(
    HeapLayers
    GIT_REPOSITORY https://github.com/emeryberger/Heap-Layers.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
  )

  # Fetch DieHard
  FetchContent_Declare(
    DieHard
    GIT_REPOSITORY https://github.com/emeryberger/DieHard.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
  )

  # Make Heap-Layers available first (DieHard depends on it)
  FetchContent_MakeAvailable(HeapLayers)

  # Patch Heap-Layers wrapper.cpp for C++20/23 compatibility
  # Fix std::align_val_t -> size_t conversion
  if(EXISTS "${heaplayers_SOURCE_DIR}/wrappers/wrapper.cpp")
    file(READ "${heaplayers_SOURCE_DIR}/wrappers/wrapper.cpp" WRAPPER_CPP_CONTENT)
    string(REPLACE "CUSTOM_MEMALIGN(n, al)" "CUSTOM_MEMALIGN(n, static_cast<size_t>(al))" WRAPPER_CPP_PATCHED "${WRAPPER_CPP_CONTENT}")
    if(NOT "${WRAPPER_CPP_CONTENT}" STREQUAL "${WRAPPER_CPP_PATCHED}")
      file(WRITE "${heaplayers_SOURCE_DIR}/wrappers/wrapper.cpp" "${WRAPPER_CPP_PATCHED}")
      message(STATUS "Patched Heap-Layers wrapper.cpp for C++20/23 compatibility")
    endif()
  endif()

  # Get DieHard source without building it
  FetchContent_GetProperties(DieHard)
  if(NOT diehard_POPULATED)
    FetchContent_Populate(DieHard)
  endif()
endif()

# Build DieHard with alloc8
# On Windows, use alloc8 interpose sources; on Unix, use header-only wrapper
if(WIN32)
  add_library(diehard_alloc8 SHARED
    diehard_alloc8.cpp
    Z:/git/printf/printf.cpp
    ${ALLOC8_INTERPOSE_SOURCES}
    ${ALLOC8_COMMON_SOURCES}
  )
else()
  add_library(diehard_alloc8 SHARED
    diehard_alloc8.cpp
  )
endif()

target_include_directories(diehard_alloc8 PRIVATE
  ${diehard_SOURCE_DIR}/src/include
  ${diehard_SOURCE_DIR}/src/include/layers
  ${diehard_SOURCE_DIR}/src/include/math
  ${diehard_SOURCE_DIR}/src/include/rng
  ${diehard_SOURCE_DIR}/src/include/static
  ${diehard_SOURCE_DIR}/src/include/util
  ${heaplayers_SOURCE_DIR}
  ${heaplayers_SOURCE_DIR}/wrappers
  ${CMAKE_SOURCE_DIR}/include  # alloc8 headers
  Z:/git/printf  # printf library for DieFast
)

target_compile_definitions(diehard_alloc8 PRIVATE
  NDEBUG
  DIEHARD_DIEFAST=0
  DIEHARD_DIEHARDER=0
  DIEHARD_SCALABLE=1  # Scalable with per-thread heaps (TheLargeHeap now has lock)
  _REENTRANT=1
)

# Use C++23 if available (matches original DieHard build)
include(CheckCXXCompilerFlag)
if(MSVC)
  check_cxx_compiler_flag("/std:c++23" COMPILER_SUPPORTS_CXX23)
else()
  check_cxx_compiler_flag("-std=c++23" COMPILER_SUPPORTS_CXX23)
endif()
if(COMPILER_SUPPORTS_CXX23)
  set_target_properties(diehard_alloc8 PROPERTIES CXX_STANDARD 23)
else()
  set_target_properties(diehard_alloc8 PROPERTIES CXX_STANDARD 20)
endif()

# Enable IPO/LTO for this target (matches original DieHard)
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported)
  set_target_properties(diehard_alloc8 PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Platform-specific compiler flags
if(MSVC)
  target_compile_options(diehard_alloc8 PRIVATE
    /W3
    /wd4996  # Disable deprecation warnings
    /wd4267  # Disable size_t conversion warnings
    /wd4244  # Disable conversion warnings
  )
else()
  target_compile_options(diehard_alloc8 PRIVATE
    -ftemplate-depth=1024
    -fvisibility=hidden
    -fno-builtin-malloc
    -fno-builtin-free
    -fno-builtin-realloc
    -fno-builtin-calloc
  )
endif()

# Platform-specific linking
if(WIN32)
  target_link_libraries(diehard_alloc8 PRIVATE
    alloc8::interpose
  )
else()
  target_link_libraries(diehard_alloc8 PRIVATE
    pthread
    dl
  )
endif()

# Platform-specific symbol visibility
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Write a version map for symbol visibility (matches original DieHard)
  set(DIEHARD_VERS_SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/diehard_vers.map)
  file(WRITE ${DIEHARD_VERS_SCRIPT}
"GLIBC_2.2.5 {
global:
    __libc_malloc;
    __libc_free;
    __libc_realloc;
    __libc_calloc;
    __libc_memalign;
    malloc;
    malloc_usable_size;
    cfree;
    free;
    realloc;
    calloc;
    reallocarray;
    memalign;
    aligned_alloc;
    valloc;
    pvalloc;
    posix_memalign;
    strdup;
    strndup;
    _Znwm*;
    _Znam*;
    _ZdlPv*;
    _ZdaPv*;
    _ZdlPvm*;
    _ZdaPvm*;
local:
    *;
  };
")
  target_link_options(diehard_alloc8 PRIVATE "LINKER:--version-script=${DIEHARD_VERS_SCRIPT}")
endif()

set_target_properties(diehard_alloc8 PROPERTIES
  OUTPUT_NAME "diehard_alloc8"
  CXX_STANDARD_REQUIRED ON
)
