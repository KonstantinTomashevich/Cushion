#include <stdio.h>

#define TEST_VARIADIC(FUNCTION_NAME, FORMAT, ...) FUNCTION_NAME (FORMAT __VA_OPT__ (,) __VA_ARGS__)

int main (int argc, char **argv)
{
    TEST_VARIADIC (func_1, "%s%s%s", "a", "b", "c");
    TEST_VARIADIC (func_2, "123");
    TEST_VARIADIC (func_3, "123",);
    return 0;
}
