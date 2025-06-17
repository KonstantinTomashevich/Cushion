#line 1 "source/defer_goto.c"
void test_call_1 () {}
void test_call_2 () {}
void test_call_3 () {}
void test_call_4 () {}
void test_call_5 () {}
void test_call_6 () {}

int main (int argc, char **argv)
{
    
    int a = 0;

condition:
{
    
    if (a == 5)
    {
         
#line 15 "source/defer_goto.c"
 test_call_2 (); 
#line 18 "source/defer_goto.c"
goto finalize;
    }

#line 15 "source/defer_goto.c"
 test_call_2 (); 
#line 20 "source/defer_goto.c"
}

cycle:
{
    
    ++a;
     
#line 24 "source/defer_goto.c"
 test_call_3 (); 
#line 26 "source/defer_goto.c"
goto condition;
}
    
finalize:
{
    
    a = 10;

#line 31 "source/defer_goto.c"
 test_call_4 (); 
#line 33 "source/defer_goto.c"
}
    
    for (int i = 0; i < 10; ++i)
    {
        
        for (int j = 0; j < 10; ++j)
        {
            
            if (i * j > 50)
            {
                 
#line 40 "source/defer_goto.c"
 test_call_6 (); 
#line 37 "source/defer_goto.c"
 test_call_5 (); 
#line 43 "source/defer_goto.c"
goto check_done;
            }
        
#line 40 "source/defer_goto.c"
 test_call_6 (); 
#line 45 "source/defer_goto.c"
}
    
#line 37 "source/defer_goto.c"
 test_call_5 (); 
#line 46 "source/defer_goto.c"
}
    
check_done:
    ;typeof ( a) cushion_cached_return_value_0 =  a;
#line 10 "source/defer_goto.c"
 test_call_1 (); 
#line 49 "source/defer_goto.c"
return cushion_cached_return_value_0;
}
