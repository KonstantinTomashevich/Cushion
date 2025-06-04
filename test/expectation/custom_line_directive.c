#line 1 "source/custom_line_directive.c"

#line 42 "our own rules here"
#line 1 "include/include_full/trivial.h"


int function_1 ();
int function_2 ();


#line 43 "our own rules here"

#pragma something different

int main (int argc, char **argv)
{
    return function_1 ( ) + function_2 ( ) ;
}
