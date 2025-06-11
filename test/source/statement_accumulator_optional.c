CUSHION_STATEMENT_ACCUMULATOR_PUSH (my_data, optional) { int not_here; }
CUSHION_STATEMENT_ACCUMULATOR_PUSH (my_data, optional) { int not_here_too; }

struct data_t
{
    CUSHION_STATEMENT_ACCUMULATOR (my_data)
};

CUSHION_STATEMENT_ACCUMULATOR_PUSH (my_data) { int x; }
CUSHION_STATEMENT_ACCUMULATOR_PUSH (my_data) { int y; }
CUSHION_STATEMENT_ACCUMULATOR_PUSH (my_data) { int z; }
CUSHION_STATEMENT_ACCUMULATOR_PUSH (my_data) { int w; }
