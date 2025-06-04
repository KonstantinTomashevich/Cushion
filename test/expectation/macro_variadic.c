#line 1 "source/macro_variadic.c"
#include <stdio.h>



int main (int argc, char **argv)
{
    func_1 ( "%s%s%s" , "a" , "b" , "c" ) ;
    func_2 ( "123" ) ;
    func_3 ( "123" , ) ;
    return 0;
}
