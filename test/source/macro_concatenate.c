#include <stdio.h>

#define DEFINE_VARIABLE(NAME) int variable_##NAME;

int main (int argc, char **argv)
{
    DEFINE_VARIABLE (1)
    DEFINE_VARIABLE (2)
#define EXISTENT_MACRO should_not_be_unwrapped
    DEFINE_VARIABLE (EXISTENT_MACRO)
#define variable_trick tricky_tricky_replacement_should_work = 42
    DEFINE_VARIABLE (trick)
    return 0;
}
