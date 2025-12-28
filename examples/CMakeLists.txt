# alloc8/examples/CMakeLists.txt

add_subdirectory(simple_heap)

# Optional: Build Hoard/DieHard examples (requires fetching external repos)
option(ALLOC8_BUILD_HOARD_EXAMPLE "Build Hoard allocator example" OFF)
option(ALLOC8_BUILD_DIEHARD_EXAMPLE "Build DieHard allocator example" OFF)

# Note: DieHard and Hoard depend on Heap-Layers which requires fixes for Windows.
# The fixes have been applied to the local Heap-Layers and DieHard repos.

if(ALLOC8_BUILD_HOARD_EXAMPLE)
  add_subdirectory(hoard)
endif()

if(ALLOC8_BUILD_DIEHARD_EXAMPLE)
  add_subdirectory(diehard)
endif()
