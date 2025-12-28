# alloc8/examples/hoard/CMakeLists.txt
# Example: Hoard allocator using alloc8 for interposition
#
# Demonstrates alloc8's thread lifecycle hooks for thread-aware allocators.
# Uses local Hoard and Heap-Layers repos, or fetches via FetchContent.
# Works on Linux, macOS, and Windows.

include(FetchContent)

# Check for local repos first (with Windows fixes applied)
set(LOCAL_HEAPLAYERS_DIR "Z:/git/Heap-Layers")
set(LOCAL_HOARD_DIR "Z:/git/Hoard")

if(EXISTS "${LOCAL_HEAPLAYERS_DIR}/heaps/utility/uniqueheap.h" AND EXISTS "${LOCAL_HOARD_DIR}/src/include")
  message(STATUS "Using local Heap-Layers from ${LOCAL_HEAPLAYERS_DIR}")
  message(STATUS "Using local Hoard from ${LOCAL_HOARD_DIR}")
  set(heaplayers_SOURCE_DIR "${LOCAL_HEAPLAYERS_DIR}")
  set(hoard_SOURCE_DIR "${LOCAL_HOARD_DIR}")
else()
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
endif()

# Build Hoard with alloc8
# Uses alloc8's thread lifecycle hooks (xxthread_init/xxthread_cleanup) for
# proper thread-aware operation instead of Hoard's own pthread interposition.

# Platform-specific source files
if(ALLOC8_PLATFORM_WINDOWS)
  # Windows uses DllMain for thread hooks
  # Note: hoard_thread_hooks_win.cpp provides its own DllMain and TLS management,
  # so we don't use ALLOC8_THREAD_SOURCES (which also has a DllMain)
  add_library(hoard_alloc8 SHARED
    hoard_alloc8.cpp
    hoard_thread_hooks_win.cpp
    ${ALLOC8_INTERPOSE_SOURCES}
    ${ALLOC8_COMMON_SOURCES}
  )
elseif(ALLOC8_PLATFORM_LINUX)
  # On Linux, gnu_wrapper.cpp includes new_delete.inc so COMMON_SOURCES not needed
  add_library(hoard_alloc8 SHARED
    hoard_alloc8.cpp
    hoard_thread_hooks.cpp
    ${ALLOC8_INTERPOSE_SOURCES}
    ${ALLOC8_THREAD_SOURCES}
  )
else()
  # macOS
  add_library(hoard_alloc8 SHARED
    hoard_alloc8.cpp
    hoard_thread_hooks.cpp
    ${ALLOC8_INTERPOSE_SOURCES}
    ${ALLOC8_THREAD_SOURCES}
    ${ALLOC8_COMMON_SOURCES}
  )
endif()

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
  $<$<BOOL:${WIN32}>:ALLOC8_NO_DLLMAIN>  # Hoard provides its own DllMain
)

# Platform-specific compiler flags
if(MSVC)
  target_compile_options(hoard_alloc8 PRIVATE
    /W3
    /wd4996  # Disable deprecation warnings
    /wd4267  # Disable size_t conversion warnings
    /wd4244  # Disable conversion warnings
  )
else()
  target_compile_options(hoard_alloc8 PRIVATE
    -ftls-model=initial-exec
    -ftemplate-depth=1024
  )
endif()

# Platform-specific linking
if(WIN32)
  target_link_libraries(hoard_alloc8 PRIVATE
    alloc8::interpose
  )
else()
  target_link_libraries(hoard_alloc8 PRIVATE
    alloc8::interpose
    pthread
  )
endif()

set_target_properties(hoard_alloc8 PROPERTIES
  OUTPUT_NAME "hoard_alloc8"
)
