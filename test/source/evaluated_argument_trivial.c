#define PREFIX(ARG) prefix_##__CUSHION_EVALUATED_ARGUMENT__(ARG)
#define SUFFIX(ARG) __CUSHION_EVALUATED_ARGUMENT__(ARG)##_suffix
#define WRAPPED(ARG) PREFIX (SUFFIX (PREFIX (SUFFIX (ARG))))

#define DOUBLE_TROUBLE(ARG) ARG = ARG

int main (int argc, char **argv)
{
    int WRAPPED (my_variable) = 0;
    int WRAPPED (DOUBLE_TROUBLE (my_variable));
    return 0;
}
