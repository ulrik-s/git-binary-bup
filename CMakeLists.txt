cmake_minimum_required(VERSION 3.13)
project(git_binary_bup C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBGIT2 REQUIRED libgit2)

include_directories(${LIBGIT2_INCLUDE_DIRS} include)
link_directories(${LIBGIT2_LIBRARY_DIRS})

add_library(bup_odb STATIC src/bup_odb.c)
target_link_libraries(bup_odb ${LIBGIT2_LIBRARIES})

add_executable(git2_bin src/git2.c)
set_target_properties(git2_bin PROPERTIES OUTPUT_NAME git2)
target_link_libraries(git2_bin bup_odb ${LIBGIT2_LIBRARIES})

enable_testing()
add_executable(test_backend tests/test_backend.c)
target_link_libraries(test_backend bup_odb ${LIBGIT2_LIBRARIES})
add_test(NAME test_backend COMMAND test_backend)
set_tests_properties(test_backend PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(test_chunk_reuse tests/test_chunk_reuse.c)
target_link_libraries(test_chunk_reuse bup_odb ${LIBGIT2_LIBRARIES})
add_test(NAME test_chunk_reuse COMMAND test_chunk_reuse)
set_tests_properties(test_chunk_reuse PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(test_cli tests/test_cli.c)
target_link_libraries(test_cli bup_odb ${LIBGIT2_LIBRARIES})
add_test(NAME test_cli COMMAND test_cli)
set_tests_properties(test_cli PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(test_many_commits tests/test_many_commits.c)
target_link_libraries(test_many_commits bup_odb ${LIBGIT2_LIBRARIES})
add_test(NAME test_many_commits COMMAND test_many_commits)
set_tests_properties(test_many_commits PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
