include_guard(GLOBAL)

add_library(order_books_project_options INTERFACE)
add_library(order_books::project_options ALIAS order_books_project_options)

target_compile_features(order_books_project_options INTERFACE cxx_std_20)

target_compile_options(order_books_project_options INTERFACE
  "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>"
)

if(ORDER_BOOKS_WARNINGS_AS_ERRORS)
  target_compile_options(order_books_project_options INTERFACE
    "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>"
  )
endif()

if(ORDER_BOOKS_ENABLE_SANITIZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    message(FATAL_ERROR
      "ORDER_BOOKS_ENABLE_SANITIZERS requires GCC, Clang, or Apple Clang")
  endif()

  target_compile_options(order_books_project_options INTERFACE
    -fno-omit-frame-pointer
    -fsanitize=address,undefined
  )
  target_link_options(order_books_project_options INTERFACE
    -fsanitize=address,undefined
  )
endif()

if(ORDER_BOOKS_ENABLE_CLANG_TIDY)
  find_program(ORDER_BOOKS_CLANG_TIDY_EXECUTABLE
               NAMES clang-tidy
               REQUIRED)
  set(CMAKE_CXX_CLANG_TIDY
      "${ORDER_BOOKS_CLANG_TIDY_EXECUTABLE}"
      CACHE STRING "clang-tidy executable" FORCE)
endif()
