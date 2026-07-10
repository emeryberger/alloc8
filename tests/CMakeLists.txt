# alloc8/tests/CMakeLists.txt

# Test programs that use system malloc (for baseline)
add_executable(test_basic_alloc test_basic_alloc.cpp)
target_compile_features(test_basic_alloc PRIVATE cxx_std_17)

# threadtest - multi-threaded allocator benchmark from Hoard
add_executable(threadtest threadtest.cpp)
target_compile_features(threadtest PRIVATE cxx_std_17)
if(NOT WIN32)
  target_link_libraries(threadtest PRIVATE pthread)
endif()

# Add basic test (without interposition - just tests the test itself)
add_test(NAME test_basic_alloc_native COMMAND test_basic_alloc)

# Caller return-address hint: links new_delete.inc with stub xx* and checks
# that distinct allocation sites observe distinct alloc8_caller_ra values.
# Uses GCC/Clang attributes, so skipped on MSVC.
if(NOT MSVC)
  add_executable(test_caller_ra test_caller_ra.cpp)
  target_compile_features(test_caller_ra PRIVATE cxx_std_20)
  target_link_libraries(test_caller_ra PRIVATE alloc8_headers)
  add_test(NAME test_caller_ra COMMAND test_caller_ra)
endif()

# If examples are built, add tests with interposition
if(TARGET simple_heap)
  if(APPLE)
    set(PRELOAD_ENV "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:simple_heap>")
  elseif(UNIX)
    set(PRELOAD_ENV "LD_PRELOAD=$<TARGET_FILE:simple_heap>")
  endif()

  if(DEFINED PRELOAD_ENV)
    add_test(NAME test_basic_alloc_interposed
             COMMAND ${CMAKE_COMMAND} -E env ${PRELOAD_ENV}
                     $<TARGET_FILE:test_basic_alloc>)
    set_tests_properties(test_basic_alloc_interposed PROPERTIES
      DEPENDS simple_heap
    )
  endif()
endif()
