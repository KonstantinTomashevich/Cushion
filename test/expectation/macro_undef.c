#line 1 "source/macro_undef.c"
#line 2 "source/macro_undef.c"

#define MACRO_PRESERVED  42
#line 4 "source/macro_undef.c"

#undef MACRO_PRESERVED



#undef NO_SUCH_MACRO_VISIBLE

int main (int argc, char **argv)
{
    return MACRO_EXISTS;
}
