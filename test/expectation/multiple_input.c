#line 1 "source/multiple_input.c"
extern int function_1 ();
extern int function_2 ();
extern int function_3 ();

int main (int argc, char **argv)
{
    return function_1 () + function_2 () + function_3 ();
}
#line 1 "source/multiple_input_append_1.c"
int function_1 ()
{
    return 1;
}
#line 1 "source/multiple_input_append_2.c"
int function_2 ()
{
    return 2;
}
#line 1 "source/multiple_input_append_3.c"
int function_3 ()
{
    return 3;
}
