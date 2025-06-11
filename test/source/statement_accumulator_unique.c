struct data_t
{
    CUSHION_STATEMENT_ACCUMULATOR (my_data)
};

CUSHION_STATEMENT_ACCUMULATOR_PUSH (my_data, unique) { int x; }
CUSHION_STATEMENT_ACCUMULATOR_PUSH (my_data, unique) { int y; }
CUSHION_STATEMENT_ACCUMULATOR_PUSH (my_data, unique) { int x; }
CUSHION_STATEMENT_ACCUMULATOR_PUSH (my_data, unique) { int y; }
CUSHION_STATEMENT_ACCUMULATOR_PUSH (my_data, unique) { int z; }
CUSHION_STATEMENT_ACCUMULATOR_PUSH (my_data, unique) { int z; }
