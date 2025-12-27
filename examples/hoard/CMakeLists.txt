# alloc8/examples/hoard/CMakeLists.txt
# Example: Hoard allocator using alloc8 for interposition
#
# NOTE: This example demonstrates the integration pattern but has known
# initialization timing issues on macOS that need further investigation.
# The DieHard example works correctly and demonstrates the same pattern.
#
# This example requires Hoard and Heap-Layers source trees.
# Set HOARD_SOURCE_DIR and HEAPLAYERS_SOURCE_DIR if not using defaults.

# Allow user to specify source directories
set(HOARD_SOURCE_DIR "$ENV{HOME}/git/Hoard" CACHE PATH "Path to Hoard source")
set(HEAPLAYERS_SOURCE_DIR "$ENV{HOME}/git/Heap-Layers" CACHE PATH "Path to Heap-Layers source")

if(NOT EXISTS "${HOARD_SOURCE_DIR}/src/include/hoard/hoardtlab.h")
  message(FATAL_ERROR "Hoard source not found at ${HOARD_SOURCE_DIR}. Set HOARD_SOURCE_DIR.")
endif()

if(NOT EXISTS "${HEAPLAYERS_SOURCE_DIR}/heaplayers.h")
  message(FATAL_ERROR "Heap-Layers source not found at ${HEAPLAYERS_SOURCE_DIR}. Set HEAPLAYERS_SOURCE_DIR.")
endif()

# Build Hoard with alloc8
if(APPLE)
  set(HOARD_TLS_SOURCE ${HOARD_SOURCE_DIR}/src/source/mactls.cpp)
else()
  set(HOARD_TLS_SOURCE ${HOARD_SOURCE_DIR}/src/source/unixtls.cpp)
endif()

add_library(hoard_alloc8 SHARED
  hoard_alloc8.cpp
  ${HOARD_TLS_SOURCE}
  ${ALLOC8_INTERPOSE_SOURCES}
  ${ALLOC8_COMMON_SOURCES}
)

target_include_directories(hoard_alloc8 PRIVATE
  ${HOARD_SOURCE_DIR}
  ${HOARD_SOURCE_DIR}/src/include
  ${HOARD_SOURCE_DIR}/src/include/hoard
  ${HOARD_SOURCE_DIR}/src/include/util
  ${HOARD_SOURCE_DIR}/src/include/superblocks
  ${HEAPLAYERS_SOURCE_DIR}
)

target_compile_definitions(hoard_alloc8 PRIVATE
  NDEBUG
  _REENTRANT=1
)

target_compile_options(hoard_alloc8 PRIVATE
  -ftls-model=initial-exec
  -ftemplate-depth=1024
)

target_link_libraries(hoard_alloc8 PRIVATE
  alloc8::interpose
  pthread
)

set_target_properties(hoard_alloc8 PROPERTIES
  OUTPUT_NAME "hoard_alloc8"
)
