# alloc8/examples/CMakeLists.txt

add_subdirectory(simple_heap)

# Optional: Build Hoard/DieHard examples (requires fetching external repos)
option(ALLOC8_BUILD_HOARD_EXAMPLE "Build Hoard allocator example" OFF)
option(ALLOC8_BUILD_DIEHARD_EXAMPLE "Build DieHard allocator example" OFF)

if(ALLOC8_BUILD_HOARD_EXAMPLE)
  add_subdirectory(hoard)
endif()

if(ALLOC8_BUILD_DIEHARD_EXAMPLE)
  add_subdirectory(diehard)
endif()
