void test_call_1 () {}
void test_call_2 () {}
void test_call_3 () {}
void test_call_4 () {}
void test_call_5 () {}
void test_call_6 () {}

int main (int argc, char **argv)
{
    CUSHION_DEFER { test_call_1 (); }
    int a = 0;

condition:
{
    CUSHION_DEFER { test_call_2 (); }
    if (a == 5)
    {
        goto finalize;
    }
}

cycle:
{
    CUSHION_DEFER { test_call_3 (); }
    ++a;
    goto condition;
}
    
finalize:
{
    CUSHION_DEFER { test_call_4 (); }
    a = 10;
}
    
    for (int i = 0; i < 10; ++i)
    {
        CUSHION_DEFER { test_call_5 (); }
        for (int j = 0; j < 10; ++j)
        {
            CUSHION_DEFER { test_call_6 (); }
            if (i * j > 50)
            {
                goto check_done;
            }
        }
    }
    
check_done:
    return a;
}
