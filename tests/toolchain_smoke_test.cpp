#include <gtest/gtest.h>

#if __cplusplus < 202002L
#error "order_books requires C++20 or newer"
#endif

TEST(ToolchainSmokeTest, UsesCxx20) {
  EXPECT_GE(__cplusplus, 202002L);
}
