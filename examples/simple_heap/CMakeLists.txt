# alloc8/examples/simple_heap/CMakeLists.txt
# Example: Building a simple heap with alloc8

# Build the heap as a shared library for LD_PRELOAD/DYLD_INSERT
add_library(simple_heap SHARED
  simple_heap.cpp
  ${ALLOC8_INTERPOSE_SOURCES}
)

target_link_libraries(simple_heap PRIVATE alloc8::interpose)

# Set output name without 'lib' prefix on some platforms
set_target_properties(simple_heap PROPERTIES
  OUTPUT_NAME "simple_heap"
  PREFIX "lib"
)

# Platform-specific settings
if(APPLE)
  set_target_properties(simple_heap PROPERTIES
    SUFFIX ".dylib"
  )
endif()
