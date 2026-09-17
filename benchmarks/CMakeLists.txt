add_executable(order_books_benchmark
  order_book_benchmark.cpp
)

set_target_properties(order_books_benchmark PROPERTIES
  CXX_EXTENSIONS OFF
)

target_include_directories(order_books_benchmark
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(order_books_benchmark
  PRIVATE
    order_books::runtime
    order_books::project_options
)
