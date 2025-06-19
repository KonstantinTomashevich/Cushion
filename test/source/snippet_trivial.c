#define BIND(...) CUSHION_SNIPPET (BINDING_POINT, __VA_ARGS__)

int main (int argc, char **argv)
{
    BIND (argv[0u])
    char *_1 = BINDING_POINT;

    BIND (argv /* test */[1u])
    char *_2 = BINDING_POINT;

    BIND (argv[2u /*test*/])
    char *_3 = BINDING_POINT;

    return 0;
}
