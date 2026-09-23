/*
 * test_fpu.c - Standalone test harness for SoftFloat and 68881/68882/68040 FPU
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>

#include "m68kops.h"
#include "m68kcpu.h"

int musashi_illg_callback(int opcode) { (void)opcode; return 0; }

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, msg) do { \
    if (expr) { \
        g_pass++; \
    } else { \
        g_fail++; \
        printf("FAIL: %s (line %d)\n", msg, __LINE__); \
    } \
} while (0)

static double fx80_to_dbl(floatx80 fx)
{
    uint64_t d = floatx80_to_float64(fx);
    double val;
    memcpy(&val, &d, sizeof(val));
    return val;
}

static floatx80 dbl_to_fx80(double d)
{
    uint64_t raw;
    memcpy(&raw, &d, sizeof(raw));
    return float64_to_floatx80(raw);
}

void test_softfloat_basic_arithmetic(void)
{
    printf("Running SoftFloat basic arithmetic tests...\n");

    // Addition
    floatx80 a = dbl_to_fx80(12.5);
    floatx80 b = dbl_to_fx80(3.25);
    floatx80 res = floatx80_add(a, b);
    double d_res = fx80_to_dbl(res);
    CHECK(fabs(d_res - 15.75) < 1e-12, "floatx80 12.5 + 3.25 == 15.75");

    // Subtraction
    res = floatx80_sub(a, b);
    d_res = fx80_to_dbl(res);
    CHECK(fabs(d_res - 9.25) < 1e-12, "floatx80 12.5 - 3.25 == 9.25");

    // Multiplication
    res = floatx80_mul(a, b);
    d_res = fx80_to_dbl(res);
    CHECK(fabs(d_res - 40.625) < 1e-12, "floatx80 12.5 * 3.25 == 40.625");

    // Division
    res = floatx80_div(dbl_to_fx80(100.0), dbl_to_fx80(4.0));
    d_res = fx80_to_dbl(res);
    CHECK(fabs(d_res - 25.0) < 1e-12, "floatx80 100.0 / 4.0 == 25.0");

    // Square Root
    res = floatx80_sqrt(dbl_to_fx80(144.0));
    d_res = fx80_to_dbl(res);
    CHECK(fabs(d_res - 12.0) < 1e-12, "floatx80 sqrt(144.0) == 12.0");
}

void test_softfloat_comparisons_and_flags(void)
{
    printf("Running SoftFloat comparison and flag tests...\n");

    floatx80 a = dbl_to_fx80(10.0);
    floatx80 b = dbl_to_fx80(20.0);
    floatx80 c = dbl_to_fx80(10.0);

    CHECK(floatx80_lt(a, b), "10.0 < 20.0");
    CHECK(!floatx80_lt(b, a), "!(20.0 < 10.0)");
    CHECK(floatx80_eq(a, c), "10.0 == 10.0");
    CHECK(floatx80_le(a, c), "10.0 <= 10.0");

    // Int32 to floatx80 conversion
    floatx80 i_val = int32_to_floatx80(-12345);
    double d_val = fx80_to_dbl(i_val);
    CHECK(d_val == -12345.0, "int32_to_floatx80(-12345) == -12345.0");

    // floatx80 to int32 conversion
    int32_t round_val = floatx80_to_int32(dbl_to_fx80(456.78));
    CHECK(round_val == 457 || round_val == 456, "floatx80_to_int32(456.78) rounds accurately");
}

int main(void)
{
    printf("=== CockatriceIII / Musashi FPU & SoftFloat Test Suite ===\n");
    test_softfloat_basic_arithmetic();
    test_softfloat_comparisons_and_flags();

    printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
