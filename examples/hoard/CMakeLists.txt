# alloc8/examples/hoard/CMakeLists.txt
# Example: Hoard allocator using alloc8 for interposition
#
# Demonstrates alloc8's thread lifecycle hooks for thread-aware allocators.
# Automatically fetches Hoard and Heap-Layers via FetchContent.

include(FetchContent)

# Fetch Heap-Layers
FetchContent_Declare(
  HeapLayers
  GIT_REPOSITORY https://github.com/emeryberger/Heap-Layers.git
  GIT_TAG        master
  GIT_SHALLOW    TRUE
)

# Fetch Hoard
FetchContent_Declare(
  Hoard
  GIT_REPOSITORY https://github.com/emeryberger/Hoard.git
  GIT_TAG        master
  GIT_SHALLOW    TRUE
)

# Make Heap-Layers available first (Hoard depends on it)
FetchContent_MakeAvailable(HeapLayers)

# Get Hoard source without building it
FetchContent_GetProperties(Hoard)
if(NOT hoard_POPULATED)
  FetchContent_Populate(Hoard)
endif()

# Build Hoard with alloc8
# Uses alloc8's thread lifecycle hooks (xxthread_init/xxthread_cleanup) for
# proper thread-aware operation instead of Hoard's own pthread interposition.
add_library(hoard_alloc8 SHARED
  hoard_alloc8.cpp
  hoard_thread_hooks.cpp
  ${ALLOC8_INTERPOSE_SOURCES}
  ${ALLOC8_THREAD_SOURCES}
  ${ALLOC8_COMMON_SOURCES}
)

target_include_directories(hoard_alloc8 PRIVATE
  ${hoard_SOURCE_DIR}
  ${hoard_SOURCE_DIR}/src/include
  ${hoard_SOURCE_DIR}/src/include/hoard
  ${hoard_SOURCE_DIR}/src/include/util
  ${hoard_SOURCE_DIR}/src/include/superblocks
  ${heaplayers_SOURCE_DIR}
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
