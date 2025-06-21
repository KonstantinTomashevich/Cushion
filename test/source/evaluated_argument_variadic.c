#define GENERATED_CONTAINER_TYPE_NAME(TYPE) generated_container_##TYPE
#define STRINGIZE_ME(...) #__CUSHION_EVALUATED_ARGUMENT__(__VA_ARGS__)

int main (int argc, char **argv)
{
    const char *type_name_strings = STRINGIZE_ME (
        GENERATED_CONTAINER_TYPE_NAME (a) 
        GENERATED_CONTAINER_TYPE_NAME (b) 
        GENERATED_CONTAINER_TYPE_NAME (c));
    return 0;
}
