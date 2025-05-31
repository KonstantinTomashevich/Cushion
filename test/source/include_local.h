#pragma once

int function_1 ();
int function_2 ();

#define SOME_MACRO function_1 () + function_2 ()
