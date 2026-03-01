cmake_minimum_required(VERSION 3.20)

project(jc CXX)

set(CMAKE_CXX_STANDARD 23)

add_executable(jcc
  src/frontend/source.cpp
  src/frontend/diagnostic.cpp
  src/frontend/token.cpp
  src/frontend/lexer.cpp
  src/frontend/parser.cpp
  src/frontend/ast.cpp
  src/frontend/path.cpp

  src/backend/analyzer.cpp
  src/backend/type_registry.cpp
  src/backend/scope.cpp
  src/backend/type.cpp
  src/backend/codegen.cpp
  src/frontend/compiler.cpp)

target_include_directories(jcc PUBLIC include)

# =======
#   LLVM
# =======
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})


# Link against LLVM libraries
target_link_libraries(jcc LLVM)
