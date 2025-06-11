CUSHION_STATEMENT_ACCUMULATOR_PUSH (my_data, unordered) { int x; }
CUSHION_STATEMENT_ACCUMULATOR_PUSH (my_data, optional, unordered) { int y; }

struct data_t
{
    CUSHION_STATEMENT_ACCUMULATOR (my_data)
};

CUSHION_STATEMENT_ACCUMULATOR_PUSH (my_data, unordered) { int z; }
CUSHION_STATEMENT_ACCUMULATOR_PUSH (my_data, unordered) { int w; }
