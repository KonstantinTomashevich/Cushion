#include <stdio.h>

#define SOMETHING

int main (int argc, char **argv)
{
#if defined(NOTHING)
    printf ("should not be here\n");
#endif
    
#ifdef NOTHING
    printf ("should not be here\n");
#endif
    
#if defined(SOMETHING)
    printf ("should be here\n");
#endif
    
#if !defined(NOTHING)
    printf ("should be here\n");
#endif
    
#if !defined(SOMETHING)
    printf ("should not be here\n");
    printf ("should not be here\n");
    printf ("should not be here\n");
#endif
    
#ifndef SOMETHING
    printf ("should not be here\n");
#endif
    
#ifdef SOMETHING
    printf ("should be here\n");
#endif
    
#if defined(NOTHING)
    printf ("should not be here\n");
    printf ("should not be here\n");
    printf ("should not be here\n");
#elifdef SOMETHING
    printf ("should be here\n");
#endif
    
#ifdef NOTHING
    printf ("should not be here\n");
#elifndef NOTHING
    printf ("should be here\n");
#endif
    return 0;
}
