#line 1 "source/pragma_trivial.c"

#line 6 "source/pragma_trivial.c"
#pragma A

#line 6 "source/pragma_trivial.c"
#pragma B
#pragma C

#line 7 "source/pragma_trivial.c"
#pragma D

#pragma reflection_array_type interned_string_t
#pragma reflection_array_type unsigned int
#pragma reflection_array_type struct my_struct_t
#pragma reflection_array_type enum my_enum_t

int main (int argc, char **argv)
{
    return 0;
}
