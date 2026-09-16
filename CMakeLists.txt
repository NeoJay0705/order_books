cmake_minimum_required(VERSION 3.25)

project(order_books LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

option(ORDER_BOOKS_ENABLE_SANITIZERS
       "Enable AddressSanitizer and UndefinedBehaviorSanitizer"
       OFF)
option(ORDER_BOOKS_WARNINGS_AS_ERRORS
       "Treat compiler warnings as errors"
       OFF)
option(ORDER_BOOKS_ENABLE_CLANG_TIDY
       "Run clang-tidy during compilation"
       OFF)

include(cmake/ProjectOptions.cmake)

find_package(Threads REQUIRED)

add_library(order_books_core STATIC
  src/domain/invariant_checker.cpp
  src/domain/order_book.cpp
  src/domain/state_machine.cpp
)
add_library(order_books::core ALIAS order_books_core)
target_include_directories(order_books_core
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(order_books_core PUBLIC order_books::project_options)

add_library(order_books_storage STATIC
  src/persistence/binary_codec.cpp
  src/persistence/config_store.cpp
  src/persistence/crc32c.cpp
  src/persistence/file_ops.cpp
  src/persistence/snapshot_store.cpp
  src/persistence/wal.cpp
)
add_library(order_books::storage ALIAS order_books_storage)
target_include_directories(order_books_storage
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(order_books_storage
  PUBLIC
    order_books::core
)

add_library(order_books_runtime STATIC
  src/runtime/engine.cpp
  src/runtime/event_publisher.cpp
  src/runtime/metrics_registry.cpp
  src/runtime/shard_runtime.cpp
)
add_library(order_books::runtime ALIAS order_books_runtime)
target_include_directories(order_books_runtime
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(order_books_runtime
  PUBLIC
    order_books::storage
    order_books::core
    Threads::Threads
)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

option(ORDER_BOOKS_BUILD_BENCHMARKS "Build the opt-in benchmark executable" OFF)
if(ORDER_BOOKS_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
