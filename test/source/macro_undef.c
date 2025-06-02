#define MACRO_EXISTS 123

#define MACRO_PRESERVED __CUSHION_PRESERVE__ 42

#undef MACRO_PRESERVED

#undef MACRO_EXISTS

#undef NO_SUCH_MACRO_VISIBLE

int main (int argc, char **argv)
{
    return MACRO_EXISTS;
}
