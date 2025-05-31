#include <stdio.h>

#define ENTRY_FUNCTION __CUSHION_PRESERVE__ int main (int argc, char **argv)
#define EXIT(CODE) __CUSHION_PRESERVE__ \
    return CODE

#define TEST_STRING "Hello, world!\n"
#define TEST_MULTILINE printf ( \
    "%s%s%s",                   \
    TEST_STRING,                \
    TEST_STRING,                \
    TEST_STRING)

#define ADD_NUMBER(X) + X

ENTRY_FUNCTION
{
    TEST_MULTILINE;
    int x = 1 ADD_NUMBER (2) ADD_NUMBER (3) ADD_NUMBER (4);
    EXIT (0);
}
