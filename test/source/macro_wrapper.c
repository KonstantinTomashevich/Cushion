#include <stdio.h>

#define TEST_WRAPPER_NO_ARGS                                                                                           \
    {                                                                                                                  \
        struct tree_traverse_context context = tree_traverse_context_init ();                                          \
        while (tree_traverse_context_step (&context))                                                                  \
        {                                                                                                              \
            struct tree_node_t *node = context.node;                                                                   \
            __CUSHION_WRAPPED__                                                                                        \
        }                                                                                                              \
                                                                                                                       \
        tree_traverse_context_shutdown (&context);                                                                     \
    }

#define TEST_WRAPPER_WITH_ARGS(FILTER)                                                                                 \
    TEST_WRAPPER_NO_ARGS                                                                                               \
    {                                                                                                                  \
        if (FILTER)                                                                                                    \
        {                                                                                                              \
            __CUSHION_WRAPPED__                                                                                        \
        }                                                                                                              \
    }

int main (int argc, char **argv)
{
    TEST_WRAPPER_WITH_ARGS (node->value > 10)
    {
#define GET_VALUE(NODE) ((NODE)->value)
        printf ("Node value: %d.\n", (int) GET_VALUE (node));
        printf ("Node pointer: %p.\n", (void *) node);
#undef GET_VALUE
    }

    return 0;
}
