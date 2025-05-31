#include <stdio.h>

#define STRINGIZE_ME(ARGUMENT) #ARGUMENT
#define STRINGIZE_VARIADIC(...) #__VA_ARGS__

int main (int argc, char **argv)
{
    const char *_1 = STRINGIZE_ME (1);
    const char *_2 = STRINGIZE_ME (abc);
    const char *_3 = STRINGIZE_ME ("abc");
    const char *_4 = STRINGIZE_ME ("\"abc\"");
    const char *_5 = STRINGIZE_VARIADIC (1,  2, 3,  4, "abc", "\"abc\"");
    return 0;
}
