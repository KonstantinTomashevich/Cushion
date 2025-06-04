#line 1 "source/macro_preserve.c"
#include <stdio.h>

#define ENTRY_FUNCTION  int main (int argc, char **argv)
#define EXIT(CODE)  \
    return CODE

#line 16 "source/macro_preserve.c"
ENTRY_FUNCTION
{
    printf ( "%s%s%s" ,  "Hello, world!\n" ,  "Hello, world!\n" ,  "Hello, world!\n" ) ;
    int x = 1 + 2 + 3 + 4 ;
    EXIT (0);
}
