# alloc8/examples/diehard/CMakeLists.txt
# Example: DieHard allocator using alloc8 for interposition
#
# This example requires DieHard and Heap-Layers source trees.
# Set DIEHARD_SOURCE_DIR and HEAPLAYERS_SOURCE_DIR if not using defaults.

# Allow user to specify source directories
set(DIEHARD_SOURCE_DIR "$ENV{HOME}/git/DieHard" CACHE PATH "Path to DieHard source")
set(HEAPLAYERS_SOURCE_DIR "$ENV{HOME}/git/Heap-Layers" CACHE PATH "Path to Heap-Layers source")

if(NOT EXISTS "${DIEHARD_SOURCE_DIR}/src/include/diehardheap.h")
  message(FATAL_ERROR "DieHard source not found at ${DIEHARD_SOURCE_DIR}. Set DIEHARD_SOURCE_DIR.")
endif()

if(NOT EXISTS "${HEAPLAYERS_SOURCE_DIR}/heaplayers.h")
  message(FATAL_ERROR "Heap-Layers source not found at ${HEAPLAYERS_SOURCE_DIR}. Set HEAPLAYERS_SOURCE_DIR.")
endif()

# Build DieHard with alloc8
add_library(diehard_alloc8 SHARED
  diehard_alloc8.cpp
  ${ALLOC8_INTERPOSE_SOURCES}
  ${ALLOC8_COMMON_SOURCES}
)

target_include_directories(diehard_alloc8 PRIVATE
  ${DIEHARD_SOURCE_DIR}/src/include
  ${DIEHARD_SOURCE_DIR}/src/include/layers
  ${DIEHARD_SOURCE_DIR}/src/include/math
  ${DIEHARD_SOURCE_DIR}/src/include/rng
  ${DIEHARD_SOURCE_DIR}/src/include/static
  ${DIEHARD_SOURCE_DIR}/src/include/util
  ${HEAPLAYERS_SOURCE_DIR}
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
