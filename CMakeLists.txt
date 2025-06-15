cmake_minimum_required(VERSION 3.13)
project(git_binary_bup C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBGIT2 REQUIRED libgit2)

include_directories(${LIBGIT2_INCLUDE_DIRS} include)
link_directories(${LIBGIT2_LIBRARY_DIRS})

add_library(bup_odb STATIC src/bup_odb.c)
target_link_libraries(bup_odb ${LIBGIT2_LIBRARIES})

enable_testing()
add_executable(test_backend tests/test_backend.c)
target_link_libraries(test_backend bup_odb ${LIBGIT2_LIBRARIES})
add_test(NAME test_backend COMMAND test_backend)
