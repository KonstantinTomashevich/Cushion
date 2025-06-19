#define BIND_DATABASE_CONTEXT(NAME, ...)                                                                               \
    CUSHION_STATEMENT_ACCUMULATOR_REF (database_context_accumulator, NAME)                                             \
    CUSHION_SNIPPET (DATABASE_CONTEXT_PATH, (__VA_ARGS__))

#define DATABASE_QUERY(NAME, VALUE_TYPE, PARAMETER)                                                                    \
    {                                                                                                                  \
        CUSHION_STATEMENT_ACCUMULATOR_PUSH (database_context_accumulator, unique)                                      \
        {                                                                                                              \
            database_query_t query_##VALUE_TYPE;                                                                       \
        }                                                                                                              \
                                                                                                                       \
        database_read_cursor_t cursor_##NAME =                                                                         \
            database_query_execute_read (DATABASE_CONTEXT_PATH->query_##VALUE_TYPE, PARAMETER);                        \
        CUSHION_DEFER { database_read_cursor_close (cursor_##NAME); }                                                  \
                                                                                                                       \
        while (1)                                                                                                      \
        {                                                                                                              \
            database_read_access_t access_##NAME = database_read_cursor_resolve (cursor_##NAME);                       \
            database_read_cursor_advance (cursor_##NAME);                                                              \
            const struct VALUE_TYPE *NAME = database_read_access_resolve (access_##NAME);                              \
                                                                                                                       \
            if (NAME)                                                                                                  \
            {                                                                                                          \
                CUSHION_DEFER { database_read_access_close (access_##NAME); }                                          \
                __CUSHION_WRAPPED__                                                                                    \
            }                                                                                                          \
            else                                                                                                       \
            {                                                                                                          \
                /* No more values in query cursor. */                                                                  \
                break;                                                                                                 \
            }                                                                                                          \
        }                                                                                                              \
    }

struct database_context_1_t
{
    CUSHION_STATEMENT_ACCUMULATOR (database_context_1)
};

struct database_context_2_t
{
    CUSHION_STATEMENT_ACCUMULATOR (database_context_2)
};

BIND_DATABASE_CONTEXT (database_context_1, database_context_ptr)

void database_function_1_a (struct database_context_1_t *database_context_ptr)
{
    DATABASE_QUERY (q_1, value_type_a, 1)
    {
        // Break for test.
        break;
    }

    DATABASE_QUERY (q_2, value_type_b, 2)
    {
        // Continue for test.
        continue;
    }

    DATABASE_QUERY (q_3, value_type_a, 3)
    {
        // Return for test.
        return;
    }

    return;
}

void database_function_1_b (struct database_context_1_t *database_context_ptr)
{
    DATABASE_QUERY (q_1, value_type_b, 1)
    {
        DATABASE_QUERY (q_2, value_type_a, 4)
        {
            // Check break inside query;
            break;
        }

        DATABASE_QUERY (q_3, value_type_a, 4)
        {
            DATABASE_QUERY (q_4, value_type_a, 5)
            {
                // Check return inside queries;
                return 5;
            }
        }
    }
}

BIND_DATABASE_CONTEXT (database_context_2, database_context_ref)

void database_function_2 (struct database_context_2_t *database_context_ref)
{
    DATABASE_QUERY (q_1, value_type_x, 10)
    {
        // Multiline to check wrapped block.
        int a = 19;
        int b = 21;
        int x = a + b;
        int y = x + a + b;
        a = b = x = y;
    }

    DATABASE_QUERY (q_2, value_type_y, 20)
    {
        // Do what you need.
    }
}
