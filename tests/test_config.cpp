#include <gtest/gtest.h>

#include "common/config.h"

using namespace arbordb;

TEST(Config, PageSizeIs4K) { EXPECT_EQ(kPageSize, 4096u); }

TEST(Config, RidEquality) {
    RID a{1, 2}, b{1, 2}, c{1, 3};
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c);
}
