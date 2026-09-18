cmake_minimum_required(VERSION 3.20)
project(oclgdb CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---------------------------------------------------------------------------
# LLVM supplies the DWARF reader. We deliberately do not hand-roll one.
# ---------------------------------------------------------------------------
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
llvm_map_components_to_libnames(OCLGDB_LLVM_LIBS debuginfodwarf object support binaryformat)

add_executable(oclgdb
  src/main.cpp
  src/cli.cpp
  src/session.cpp
  src/inferior.cpp
  src/dwarf_index.cpp
  src/dwarf_expr.cpp
  src/value.cpp
  src/expr.cpp
  src/workitem.cpp
  src/pocl.cpp
  src/runspec.c
  src/util.cpp
)
target_include_directories(oclgdb PRIVATE ${LLVM_INCLUDE_DIRS} src)
target_compile_definitions(oclgdb PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(oclgdb PRIVATE ${OCLGDB_LLVM_LIBS})
target_compile_options(oclgdb PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ---------------------------------------------------------------------------
# The debuggee: a generic OpenCL host harness driven by a .ocl-run spec file.
# ---------------------------------------------------------------------------
find_package(OpenCL REQUIRED)
add_executable(ocl-runner host/ocl_runner.c src/runspec.c)
target_include_directories(ocl-runner PRIVATE src)
target_link_libraries(ocl-runner PRIVATE OpenCL::OpenCL m)
target_compile_options(ocl-runner PRIVATE -g -O0 -Wall)

enable_testing()
add_subdirectory(tests)
