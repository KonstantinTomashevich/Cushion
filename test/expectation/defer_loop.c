#line 1 "source/defer_loop.c"
void test_call_1 () {}
void test_call_2 () {}
void test_call_3 () {}
void test_call_4 () {}

int main (int argc, char **argv)
{
    
    int a = 0;
    
    do
    {
        
        if (a == 3)
        {
            
#line 13 "source/defer_loop.c"
 test_call_2 (); 
#line 16 "source/defer_loop.c"
continue;
        }
        
        for (int i = 0; i < 10; ++i)
        {
            
            for (int j = 0; j < 10; ++j)
            {
                
                if (j == 5)
                {
                    
#line 24 "source/defer_loop.c"
 test_call_4 (); 
#line 27 "source/defer_loop.c"
break;
                }
            
#line 24 "source/defer_loop.c"
 test_call_4 (); 
#line 29 "source/defer_loop.c"
}
            
            if (i == 9)
            {
                
#line 21 "source/defer_loop.c"
 test_call_3 (); 
#line 33 "source/defer_loop.c"
break;
            }
            
            if (i == 10)
            {
                typeof ( 5) cushion_cached_return_value_0 =  5;
#line 21 "source/defer_loop.c"
 test_call_3 (); 
#line 13 "source/defer_loop.c"
 test_call_2 (); 
#line 8 "source/defer_loop.c"
 test_call_1 (); 
#line 38 "source/defer_loop.c"
return cushion_cached_return_value_0;
            }
        
#line 21 "source/defer_loop.c"
 test_call_3 (); 
#line 40 "source/defer_loop.c"
}
        
        ++a;
    
#line 13 "source/defer_loop.c"
 test_call_2 (); 
#line 43 "source/defer_loop.c"
} while (a < 5);
    typeof ( 0) cushion_cached_return_value_1 =  0;
#line 8 "source/defer_loop.c"
 test_call_1 (); 
#line 44 "source/defer_loop.c"
return cushion_cached_return_value_1;
}
