void test_call_1 () {}
void test_call_2 () {}
void test_call_3 () {}
void test_call_4 () {}

int main (int argc, char **argv)
{
    CUSHION_DEFER { test_call_1 (); }
    int a = 0;
    
    do
    {
        CUSHION_DEFER { test_call_2 (); }
        if (a == 3)
        {
            continue;
        }
        
        for (int i = 0; i < 10; ++i)
        {
            CUSHION_DEFER { test_call_3 (); }
            for (int j = 0; j < 10; ++j)
            {
                CUSHION_DEFER { test_call_4 (); }
                if (j == 5)
                {
                    break;
                }
            }
            
            if (i == 9)
            {
                break;
            }
            
            if (i == 10)
            {
                return 5;
            }
        }
        
        ++a;
    } while (a < 5);
    return 0;
}
