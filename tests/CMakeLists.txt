# alloc8/tests/CMakeLists.txt

# Test programs that use system malloc (for baseline)
add_executable(test_basic_alloc test_basic_alloc.cpp)
target_compile_features(test_basic_alloc PRIVATE cxx_std_17)

# Comprehensive malloc API test (C for maximum portability)
add_executable(test_malloc_api test_malloc_api.c)
if(NOT WIN32)
  target_link_libraries(test_malloc_api PRIVATE pthread)
endif()

# threadtest - multi-threaded allocator benchmark from Hoard
add_executable(threadtest threadtest.cpp)
target_compile_features(threadtest PRIVATE cxx_std_17)
if(NOT WIN32)
  target_link_libraries(threadtest PRIVATE pthread)
endif()

# Add basic tests (without interposition - just tests the tests themselves)
add_test(NAME test_basic_alloc_native COMMAND test_basic_alloc)
add_test(NAME test_malloc_api_native COMMAND test_malloc_api)

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
    set(PRELOAD_ENV "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:simple_heap>;DYLD_FORCE_FLAT_NAMESPACE=1")
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

    add_test(NAME test_malloc_api_interposed
             COMMAND ${CMAKE_COMMAND} -E env ${PRELOAD_ENV}
                     $<TARGET_FILE:test_malloc_api>)
    set_tests_properties(test_malloc_api_interposed PROPERTIES
      DEPENDS simple_heap
    )
  endif()
endif()
