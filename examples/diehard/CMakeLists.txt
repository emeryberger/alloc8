# alloc8/examples/diehard/CMakeLists.txt
# Example: DieHard allocator using alloc8 for interposition
#
# Automatically fetches DieHard and Heap-Layers via FetchContent.

include(FetchContent)

# Fetch Heap-Layers
FetchContent_Declare(
  HeapLayers
  GIT_REPOSITORY https://github.com/emeryberger/Heap-Layers.git
  GIT_TAG        master
  GIT_SHALLOW    TRUE
  # Patch to fix std::align_val_t -> size_t conversion for C++20/23
  PATCH_COMMAND sed -i.bak "s/CUSTOM_MEMALIGN(n, al)/CUSTOM_MEMALIGN(n, static_cast<size_t>(al))/g" wrappers/wrapper.cpp
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

# Get DieHard source without building it
FetchContent_GetProperties(DieHard)
if(NOT diehard_POPULATED)
  FetchContent_Populate(DieHard)
endif()

# Build DieHard with alloc8
# Uses header-only wrapper for zero-overhead (no separate wrapper compilation unit)
add_library(diehard_alloc8 SHARED
  diehard_alloc8.cpp
)

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
)

target_compile_definitions(diehard_alloc8 PRIVATE
  NDEBUG
  DIEHARD_DIEFAST=0
  DIEHARD_DIEHARDER=0
  DIEHARD_SCALABLE=1
  _REENTRANT=1
)

# Use C++23 if available (matches original DieHard build)
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-std=c++23" COMPILER_SUPPORTS_CXX23)
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

target_compile_options(diehard_alloc8 PRIVATE
  -ftemplate-depth=1024
  -fvisibility=hidden
  -fno-builtin-malloc
  -fno-builtin-free
  -fno-builtin-realloc
  -fno-builtin-calloc
)

# Note: We're now using Heap-Layers wrappers directly instead of alloc8::interpose
# This eliminates the xxmalloc indirection for maximum performance
target_link_libraries(diehard_alloc8 PRIVATE
  pthread
  dl
)

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

set_target_properties(diehard_alloc8 PROPERTIES
  OUTPUT_NAME "diehard_alloc8"
  CXX_STANDARD_REQUIRED ON
)
