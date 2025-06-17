#line 1 "source/defer_scope.c"
void test_call_1 () {}
void test_call_2 () {}
void test_call_3 () {}
void test_call_4 () {}

void call_function ()
{
    
    
    {
        
        
        {
            
            
#line 14 "source/defer_scope.c"
 test_call_3 (); 
#line 11 "source/defer_scope.c"
 test_call_2 (); 
#line 8 "source/defer_scope.c"
 test_call_1 (); 
#line 15 "source/defer_scope.c"
return;
        }
        
        
    
#line 18 "source/defer_scope.c"
 test_call_4 (); 
#line 11 "source/defer_scope.c"
 test_call_2 (); 
#line 19 "source/defer_scope.c"
}

#line 8 "source/defer_scope.c"
 test_call_1 (); 
#line 20 "source/defer_scope.c"
}

int main (int argc, char **argv)
{
    
    typeof ( 1 + 2 + 3) cushion_cached_return_value_0 =  1 + 2 + 3;
#line 24 "source/defer_scope.c"
 call_function (); 
#line 25 "source/defer_scope.c"
return cushion_cached_return_value_0;
}
