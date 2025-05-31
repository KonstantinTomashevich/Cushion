#include <stdio.h>

#include <include_scan_only/recursive_level_1.h>
// To also check pragma once correctness.
#include <include_scan_only/recursive_level_3.h>

int main (int argc, char **argv)
{
#if defined(MACRO_LEVEL_1) && defined(MACRO_LEVEL_2) && defined(MACRO_LEVEL_3)
    printf ("All defines are in here.\n");
#endif
    return 0;
}
