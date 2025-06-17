void test_call_1 () {}
void test_call_2 () {}
void test_call_3 () {}
void test_call_4 () {}

void call_function ()
{
    CUSHION_DEFER { test_call_1 (); }
    
    {
        CUSHION_DEFER { test_call_2 (); }
        
        {
            CUSHION_DEFER { test_call_3 (); }
            return;
        }
        
        CUSHION_DEFER { test_call_4 (); }
    }
}

int main (int argc, char **argv)
{
    CUSHION_DEFER { call_function (); }
    return 1 + 2 + 3;
}
