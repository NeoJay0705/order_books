find_package(GTest CONFIG REQUIRED)

add_executable(order_books_tests
  toolchain_smoke_test.cpp
)

set_target_properties(order_books_tests PROPERTIES
  CXX_EXTENSIONS OFF
)

target_link_libraries(order_books_tests
  PRIVATE
    order_books::project_options
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(order_books_tests)
