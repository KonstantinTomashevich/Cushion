#include <stdio.h>

#define SOME_EXPRESSION (3 * 4 + 7)
#define PARAMETRIZED(X, Y, Z) (X + Y * Y / Z)

int main (int argc, char **argv)
{
#if 1 + SOME_EXPRESSION == 20
    printf ("A\n");
    printf ("B\n");
#endif
    
#if 1 + SOME_EXPRESSION == 23
    printf ("C\n");
    printf ("D\n");
#endif

#if PARAMETRIZED(1, 12, 4) == 37
    printf ("E\n");
    printf ("F\n");
#endif
    
#if PARAMETRIZED(1, 12, 5) == 37
    printf ("G\n");
    printf ("H\n");
#endif
    return 0;
}
