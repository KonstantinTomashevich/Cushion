#define MAKE_PRAGMA(...) _Pragma (#__VA_ARGS__)

#define REFLECTION_ARRAY_TYPE(TYPE) MAKE_PRAGMA (reflection_array_type TYPE)
#define MULTIPLE_PRAGMA(X, Y) _Pragma (#X) _Pragma (#Y)

MULTIPLE_PRAGMA (A, B)
MULTIPLE_PRAGMA (C, D)

REFLECTION_ARRAY_TYPE (interned_string_t)
REFLECTION_ARRAY_TYPE (unsigned int)
REFLECTION_ARRAY_TYPE (struct my_struct_t)
REFLECTION_ARRAY_TYPE (enum my_enum_t)

int main (int argc, char **argv)
{
    return 0;
}
