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

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
