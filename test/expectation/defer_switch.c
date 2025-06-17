#line 1 "source/defer_switch.c"
void test_call_1 () {}
void test_call_2 () {}
void test_call_3 () {}
void test_call_4 () {}

int main (int argc, char **argv)
{
    switch (argc)
    {
    case 0u:
    {
        
        
#line 12 "source/defer_switch.c"
 test_call_1 (); 
#line 13 "source/defer_switch.c"
break;
    }
        
    case 1u:
    {
        
        
#line 18 "source/defer_switch.c"
 test_call_2 (); 
#line 19 "source/defer_switch.c"
break;
    }
        
    case 2u:
    {
        
        switch (*argv[1u])
        {
        case 'a':
        case 'b':
            break;
            
        default:
        {
            
            
#line 33 "source/defer_switch.c"
 test_call_4 (); 
#line 34 "source/defer_switch.c"
break;
        }
        }
        
        
#line 24 "source/defer_switch.c"
 test_call_3 (); 
#line 38 "source/defer_switch.c"
break;
    }
        
    default:
        break;
    }
    
    return 0;
}
