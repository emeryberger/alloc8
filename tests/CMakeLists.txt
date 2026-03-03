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
