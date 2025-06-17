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
        CUSHION_DEFER { test_call_1 (); }
        break;
    }
        
    case 1u:
    {
        CUSHION_DEFER { test_call_2 (); }
        break;
    }
        
    case 2u:
    {
        CUSHION_DEFER { test_call_3 (); }
        switch (*argv[1u])
        {
        case 'a':
        case 'b':
            break;
            
        default:
        {
            CUSHION_DEFER { test_call_4 (); }
            break;
        }
        }
        
        break;
    }
        
    default:
        break;
    }
    
    return 0;
}
