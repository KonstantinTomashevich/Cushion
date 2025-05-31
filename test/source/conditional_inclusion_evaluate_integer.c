#include <stdio.h>

int main (int argc, char **argv)
{
#if 1 + 3 * 4 + 7 == 20
    printf ("A\n");
    printf ("B\n");
#endif
    
#if 1 + 3 * 4 + 7 == 23
    printf ("C\n");
    printf ("D\n");
#endif
    
#if (1 + 3) * 4 + 7 == 23
    printf ("E\n");
    printf ("F\n");
#endif
    
#if (1 == 1) + (2 == 2) + (3 == 4) == 2
    printf ("G\n");
    printf ("H\n");
#endif
    return 0;
}
