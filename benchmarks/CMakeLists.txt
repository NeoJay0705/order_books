add_executable(order_books_benchmark
  order_book_benchmark.cpp
  pipeline_ceiling_benchmark.cpp
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

if(BUILD_TESTING)
  add_test(
    NAME order_books_benchmark_engine_producer_lanes_zero
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--engine-producer-lanes=0
            -DEXPECTED_ERROR=engine_producer_lanes_must_be_positive
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_engine_producer_lanes_non_numeric
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--engine-producer-lanes=abc
            -DEXPECTED_ERROR=engine_producer_lanes_invalid
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_engine_producer_lanes_over_capacity
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--engine-producer-lanes=65537
            -DEXPECTED_ERROR=engine_producer_lanes_exceed_capacity
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )
endif()
