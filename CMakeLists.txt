cmake_minimum_required(VERSION 3.20)

project(px CXX)

set(CMAKE_CXX_STANDARD 23)

add_executable(pxc
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
  src/backend/value.cpp
  src/frontend/compiler.cpp)

target_include_directories(pxc PRIVATE third_party)
target_include_directories(pxc PRIVATE include)

# =======
#   LLVM
# =======
find_package(LLVM REQUIRED CONFIG)

llvm_map_components_to_libnames(llvm_libs core support linker)
target_include_directories(pxc PRIVATE ${LLVM_INCLUDE_DIRS})

#target_link_libraries(pxc ${llvm_libs})
target_link_libraries(pxc LLVM)
