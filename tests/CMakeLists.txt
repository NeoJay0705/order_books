find_package(GTest CONFIG REQUIRED)

add_executable(order_books_tests
  toolchain_smoke_test.cpp
  unit/order_book_test.cpp
  unit/state_machine_test.cpp
  integration/codec_test.cpp
  integration/engine_durable_single_instrument_test.cpp
  integration/engine_test.cpp
  integration/persistence_test.cpp
  integration/wal_write_ceiling_test.cpp
  unit/metrics_registry_test.cpp
)

set_target_properties(order_books_tests PROPERTIES
  CXX_EXTENSIONS OFF
)

target_include_directories(order_books_tests
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)

target_link_libraries(order_books_tests
  PRIVATE
    order_books::project_options
    order_books::runtime
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(order_books_tests)
