#line 1 "source/include_recursive.c"
#include <stdio.h>

#line 1 "include/include_full/recursive_level_1.h"


#line 1 "include/include_full/recursive_level_2.h"


#line 1 "include/include_full/recursive_level_3.h"


int last_function ();


#line 4 "include/include_full/recursive_level_2.h"

int another_function ();


#line 4 "include/include_full/recursive_level_1.h"

int some_function ();


#line 4 "source/include_recursive.c"

#line 1 "include/include_full/recursive_level_3.h"
#line 6 "source/include_recursive.c"

int main (int argc, char **argv)
{

    printf ("All defines are in here.\n");

    return 0;
}
