#line 1 "source/macro_wrapper.c"
#include <stdio.h>
#line 24 "source/macro_wrapper.c"
int main (int argc, char **argv)
{
     { struct tree_traverse_context context = tree_traverse_context_init ( ) ; while ( tree_traverse_context_step ( & context ) ) { struct tree_node_t * node = context . node ; { if ( node -> value > 10 ) {
{
        printf ("Node value: %d.\n", (int) node->value);
        printf ("Node pointer: %p.\n", (void *) node);
    }
#line 26 "source/macro_wrapper.c"
} } } tree_traverse_context_shutdown ( & context ) ; }
#line 32 "source/macro_wrapper.c"
    return 0;
}
