#include <unity.h>
#include "core/Median.h"

using quantix::core::medianFromArray;

void setUp() {}
void tearDown() {}

void test_empty_returns_zero()
{
    TEST_ASSERT_EQUAL_UINT32(0u, medianFromArray(nullptr, 0));
}

void test_single_element()
{
    uint32_t a[] = {42};
    TEST_ASSERT_EQUAL_UINT32(42u, medianFromArray(a, 1));
}

void test_all_equal()
{
    uint32_t a[] = {7, 7, 7, 7, 7};
    TEST_ASSERT_EQUAL_UINT32(7u, medianFromArray(a, 5));
}

void test_odd_count_unsorted()
{
    uint32_t a[] = {5, 1, 3, 9, 2};
    TEST_ASSERT_EQUAL_UINT32(3u, medianFromArray(a, 5));
}

void test_even_count_uses_average()
{
    uint32_t a[] = {1, 2, 3, 4};
    TEST_ASSERT_EQUAL_UINT32(2u, medianFromArray(a, 4));  // (2+3)/2 == 2 en entero
}

// Un outlier grande no debe contaminar la mediana (diferencia vs. media).
void test_outlier_does_not_dominate()
{
    uint32_t a[] = {100, 101, 102, 103, 1000000};
    TEST_ASSERT_EQUAL_UINT32(102u, medianFromArray(a, 5));
}

// Entrada monotónica: la mediana es el elemento del medio.
void test_monotonic_input()
{
    uint32_t a[] = {10, 20, 30, 40, 50, 60, 70};
    TEST_ASSERT_EQUAL_UINT32(40u, medianFromArray(a, 7));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty_returns_zero);
    RUN_TEST(test_single_element);
    RUN_TEST(test_all_equal);
    RUN_TEST(test_odd_count_unsorted);
    RUN_TEST(test_even_count_uses_average);
    RUN_TEST(test_outlier_does_not_dominate);
    RUN_TEST(test_monotonic_input);
    return UNITY_END();
}
