# alloc8/examples/hoard/CMakeLists.txt
# Example: the Hoard allocator built on alloc8.
#
# Hoard itself now uses alloc8 for interposition, so this example compiles
# Hoard's own sources (libhoard.cpp + the platform TLS file) against this
# alloc8 tree -- the same set Hoard's CMakeLists.txt builds. It used to carry
# private copies of that glue code (hoard_alloc8.cpp, hoard_thread_hooks*.cpp),
# which drifted out of sync with Hoard: on macOS they stopped linking once
# Hoard's ownership map (Hoard::ownershipdetail::bits, defined in libhoard.cpp)
# was added.
#
# Uses local Hoard / Heap-Layers checkouts if given, else fetches them.

include(FetchContent)

set(LOCAL_HEAPLAYERS_DIR "" CACHE PATH "Local Heap-Layers directory (optional)")
set(LOCAL_HOARD_DIR "" CACHE PATH "Local Hoard directory (optional)")

if(LOCAL_HEAPLAYERS_DIR AND EXISTS "${LOCAL_HEAPLAYERS_DIR}/heaps/utility/uniqueheap.h"
   AND LOCAL_HOARD_DIR AND EXISTS "${LOCAL_HOARD_DIR}/src/source/libhoard.cpp")
  message(STATUS "Using local Heap-Layers from ${LOCAL_HEAPLAYERS_DIR}")
  message(STATUS "Using local Hoard from ${LOCAL_HOARD_DIR}")
  set(heaplayers_SOURCE_DIR "${LOCAL_HEAPLAYERS_DIR}")
  set(hoard_SOURCE_DIR "${LOCAL_HOARD_DIR}")
else()
  FetchContent_Declare(
    HeapLayers
    GIT_REPOSITORY https://github.com/emeryberger/Heap-Layers.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(HeapLayers)

  # Hoard's sources only: Hoard's own CMakeLists.txt would fetch and add a
  # second copy of alloc8, so populate without add_subdirectory.
  FetchContent_Declare(
    Hoard
    GIT_REPOSITORY https://github.com/emeryberger/Hoard.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
  )
  FetchContent_GetProperties(Hoard)
  if(NOT hoard_POPULATED)
    FetchContent_Populate(Hoard)
  endif()
endif()

# Allocation-free printf, compiled into Hoard (as Hoard's own build does).
FetchContent_Declare(
  hoard_printf
  GIT_REPOSITORY https://github.com/emeryberger/printf.git
  GIT_TAG        master
  GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(hoard_printf)
if(NOT hoard_printf_POPULATED)
  FetchContent_Populate(hoard_printf)
endif()

# Same per-platform source set as Hoard's CMakeLists.txt.
if(ALLOC8_PLATFORM_WINDOWS)
  set(HOARD_TLS_SOURCE ${hoard_SOURCE_DIR}/src/source/wintls.cpp)
elseif(ALLOC8_PLATFORM_MACOS)
  set(HOARD_TLS_SOURCE ${hoard_SOURCE_DIR}/src/source/mactls.cpp)
else()
  set(HOARD_TLS_SOURCE ${hoard_SOURCE_DIR}/src/source/unixtls.cpp)
endif()

add_library(hoard_alloc8 SHARED
  ${hoard_SOURCE_DIR}/src/source/libhoard.cpp
  ${HOARD_TLS_SOURCE}
  ${hoard_printf_SOURCE_DIR}/printf.cpp
  ${ALLOC8_INTERPOSE_SOURCES}
)

target_include_directories(hoard_alloc8 PRIVATE
  ${hoard_SOURCE_DIR}
  ${hoard_SOURCE_DIR}/src/include
  ${hoard_SOURCE_DIR}/src/include/hoard
  ${hoard_SOURCE_DIR}/src/include/superblocks
  ${hoard_SOURCE_DIR}/src/include/util
  ${heaplayers_SOURCE_DIR}
  ${hoard_printf_SOURCE_DIR}
)

target_compile_definitions(hoard_alloc8 PRIVATE
  _REENTRANT=1
  # Hoard never reads alloc8's caller-return-address hint.
  ALLOC8_NO_CALLER_RA
  $<$<BOOL:${WIN32}>:_CRT_SECURE_NO_WARNINGS>
  $<$<BOOL:${WIN32}>:WIN32_LEAN_AND_MEAN>
  # Hoard provides its own DllMain (wintls.cpp).
  $<$<BOOL:${WIN32}>:ALLOC8_NO_DLLMAIN>
)

if(MSVC)
  target_compile_options(hoard_alloc8 PRIVATE /W3 /WX-)
  target_link_options(hoard_alloc8 PRIVATE "/export:DetourFinishHelperProcess,@1,NONAME")
  target_compile_definitions(hoard_alloc8 PRIVATE
    "ALLOC8_XXOWNS_INLINE_HEADER=\"hoardownsinline.h\"")
else()
  target_compile_options(hoard_alloc8 PRIVATE
    -ftls-model=initial-exec
    -ftemplate-depth=1024
  )
  if(ALLOC8_PLATFORM_MACOS)
    # Inline ownership predicate backed by Hoard's ownership map (as in
    # Hoard's own macOS build); elsewhere the runtime xxowns() hook applies.
    target_compile_definitions(hoard_alloc8 PRIVATE
      "ALLOC8_XXOWNS_INLINE_HEADER=\"hoardownsinline.h\"")
  endif()
endif()

if(WIN32)
  target_link_libraries(hoard_alloc8 PRIVATE alloc8::interpose)
else()
  target_link_libraries(hoard_alloc8 PRIVATE alloc8::interpose pthread)
endif()

set_target_properties(hoard_alloc8 PROPERTIES
  OUTPUT_NAME "hoard_alloc8"
  CXX_STANDARD 23     # as Hoard's own build (std::atomic_ref needs >= 20)
  CXX_STANDARD_REQUIRED OFF
)
