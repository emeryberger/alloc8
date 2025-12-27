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
add_library(diehard_alloc8 SHARED
  diehard_alloc8.cpp
  ${ALLOC8_INTERPOSE_SOURCES}
  ${ALLOC8_COMMON_SOURCES}
)

target_include_directories(diehard_alloc8 PRIVATE
  ${diehard_SOURCE_DIR}/src/include
  ${diehard_SOURCE_DIR}/src/include/layers
  ${diehard_SOURCE_DIR}/src/include/math
  ${diehard_SOURCE_DIR}/src/include/rng
  ${diehard_SOURCE_DIR}/src/include/static
  ${diehard_SOURCE_DIR}/src/include/util
  ${heaplayers_SOURCE_DIR}
)

target_compile_definitions(diehard_alloc8 PRIVATE
  NDEBUG
  DIEHARD_DIEFAST=0
  DIEHARD_DIEHARDER=0
  DIEHARD_SCALABLE=1
  _REENTRANT=1
)

target_compile_options(diehard_alloc8 PRIVATE
  -ftemplate-depth=1024
)

target_link_libraries(diehard_alloc8 PRIVATE
  alloc8::interpose
  pthread
)

set_target_properties(diehard_alloc8 PROPERTIES
  OUTPUT_NAME "diehard_alloc8"
)
