#include <stdio.h>

int main (int argc, char **argv)
{
#if 0
    printf ("A\n");
    printf ("B\n");
#elif 0
    printf ("C\n");
    printf ("D\n");
#elif 1
    printf ("E\n");
    printf ("F\n");
#else
    printf ("G\n");
    printf ("H\n");
#endif
    return 0;
}
