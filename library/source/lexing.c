#include "internal.h"

enum cushion_lex_replacement_list_result_t cushion_lex_replacement_list (
    struct cushion_instance_t *instance,
    struct cushion_tokenization_state_t *tokenization_state,
    struct cushion_token_list_item_t **token_list_output)
{
    *token_list_output = NULL;
    struct cushion_token_list_item_t *first = NULL;
    struct cushion_token_list_item_t *last = NULL;
    struct cushion_token_t current_token;
    unsigned int lexing = 1u;

    while (lexing)
    {
        cushion_tokenization_next_token (instance, tokenization_state, &current_token);
        if (cushion_instance_is_error_signaled (instance))
        {
            cushion_instance_signal_error (instance);
            return CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR;
        }

#define APPEND_TOKEN_TO_LIST(ITEM)                                                                                     \
    if (last)                                                                                                          \
    {                                                                                                                  \
        last->next = ITEM;                                                                                             \
    }                                                                                                                  \
    else                                                                                                               \
    {                                                                                                                  \
        first = ITEM;                                                                                                  \
    }                                                                                                                  \
    last = ITEM

#define SAVE_AND_APPEND_TOKEN_TO_LIST                                                                                  \
    {                                                                                                                  \
        struct cushion_token_list_item_t *new_token_item =                                                             \
            cushion_save_token_to_memory (instance, &current_token, CUSHION_ALLOCATION_CLASS_PERSISTENT);              \
        APPEND_TOKEN_TO_LIST (new_token_item);                                                                         \
    }

        switch (current_token.type)
        {
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IFDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IFNDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFNDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELSE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ENDIF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_INCLUDE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_DEFINE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_UNDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_LINE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_PRAGMA:
            cushion_instance_execution_error (
                instance, tokenization_state,
                "Encountered preprocessor directive while lexing replacement list. Shouldn't be "
                "possible at all, can be an internal error.");
            return CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR;

        case CUSHION_TOKEN_TYPE_IDENTIFIER:
            if (current_token.identifier_kind == CUSHION_IDENTIFIER_KIND_CUSHION_PRESERVE)
            {
                if (first)
                {
                    cushion_instance_execution_error (
                        instance, tokenization_state,
                        "Encountered __CUSHION_PRESERVE__ while lexing replacement list and it is not first "
                        "replacement-list-significant token. When using __CUSHION_PRESERVE__ to avoid unwrapping "
                        "macro, it must always be the first thing in the replacement list.");
                    return CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR;
                }

                return CUSHION_LEX_REPLACEMENT_LIST_RESULT_PRESERVED;
            }

            SAVE_AND_APPEND_TOKEN_TO_LIST
            break;

        case CUSHION_TOKEN_TYPE_PUNCTUATOR:
        case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:
        case CUSHION_TOKEN_TYPE_NUMBER_FLOATING:
        case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
        case CUSHION_TOKEN_TYPE_STRING_LITERAL:
        case CUSHION_TOKEN_TYPE_OTHER:
            SAVE_AND_APPEND_TOKEN_TO_LIST
            break;

        case CUSHION_TOKEN_TYPE_NEW_LINE:
        case CUSHION_TOKEN_TYPE_END_OF_FILE:
            lexing = 0u;
            break;

        case CUSHION_TOKEN_TYPE_GLUE:
        case CUSHION_TOKEN_TYPE_COMMENT:
            // Glue and comments are ignored in replacement lists as replacements newer preserve formatting.
            break;
        }

#undef SAVE_AND_APPEND_TOKEN_TO_LIST
#undef APPEND_TOKEN_TO_LIST
    }

    *token_list_output = first;
    return CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR;
}

enum lexer_token_stack_item_flags_t
{
    LEXER_TOKEN_STACK_ITEM_FLAG_NONE = 0u,
    LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT,
};

struct lexer_token_stack_item_t
{
    struct lexer_token_stack_item_t *previous;
    struct cushion_token_list_item_t *tokens_current;
    enum lexer_token_stack_item_flags_t flags;
};

enum lexer_conditional_inclusion_state_t
{
    CONDITIONAL_INCLUSION_STATE_INCLUDED = 0u,
    CONDITIONAL_INCLUSION_STATE_EXCLUDED,
    CONDITIONAL_INCLUSION_STATE_PRESERVED,
};

enum lexer_conditional_inclusion_flags_t
{
    CONDITIONAL_INCLUSION_FLAGS_NONE = 0u,
    CONDITIONAL_INCLUSION_FLAGS_WAS_INCLUDED = 1u << 0u,
    CONDITIONAL_INCLUSION_FLAGS_HAD_PLAIN_ELSE = 1u << 1u,
};

struct lexer_conditional_inclusion_node_t
{
    struct lexer_conditional_inclusion_node_t *previous;
    enum lexer_conditional_inclusion_state_t state;
    enum lexer_conditional_inclusion_flags_t flags;
    unsigned int line;
};

static inline unsigned int lexer_file_state_should_continue (struct cushion_lexer_file_state_t *state)
{
    return state->lexing && !cushion_instance_is_error_signaled (state->instance);
}

static inline void lexer_file_state_push_tokens (struct cushion_lexer_file_state_t *state,
                                                 struct cushion_token_list_item_t *tokens,
                                                 enum lexer_token_stack_item_flags_t flags)
{
    if (!tokens)
    {
        // Can be a macro with empty replacement list, just skip it.
        return;
    }

    struct lexer_token_stack_item_t *item =
        cushion_allocator_allocate (&state->instance->allocator, sizeof (struct lexer_token_stack_item_t),
                                    _Alignof (struct lexer_token_stack_item_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);

    item->previous = state->token_stack_top;
    item->tokens_current = tokens;
    item->flags = flags;
    state->token_stack_top = item;
}

static inline enum lexer_token_stack_item_flags_t lexer_file_state_pop_token (struct cushion_lexer_file_state_t *state,
                                                                              struct cushion_token_t *output)
{
    enum lexer_token_stack_item_flags_t flags = LEXER_TOKEN_STACK_ITEM_FLAG_NONE;
    if (state->token_stack_top)
    {
        flags = state->token_stack_top->flags;
        *output = state->token_stack_top->tokens_current->token;
        state->token_stack_top->tokens_current = state->token_stack_top->tokens_current->next;

        if (!state->token_stack_top->tokens_current)
        {
            // Stack list has ended, that means that macro replacement is no more, so we can pop out that stack item.
            state->token_stack_top = state->token_stack_top->previous;
        }

        goto read_token;
    }

    // No more ready-to-use tokens from replacement lists or other sources, request next token from tokenizer.
    cushion_tokenization_next_token (state->instance, &state->tokenization, output);

read_token:
    if (output->type == CUSHION_TOKEN_TYPE_END_OF_FILE)
    {
        // Process lexing stop on end of file internally for ease of use.
        state->lexing = 0u;
    }

    return flags;
}

static inline void lexer_file_state_path_init (struct cushion_lexer_file_state_t *state, const char *data)
{
    const size_t in_size = strlen (data);
    if (in_size + 1u > CUSHION_PATH_BUFFER_SIZE)
    {
        cushion_instance_execution_error (state->instance, &state->tokenization,
                                          "Failed to initialize path container, given path is too long: %s", data);
        return;
    }

    memcpy (state->path_buffer.data, data, in_size);
    state->path_buffer.data[in_size] = '\0';
    state->path_buffer.size = in_size;
}

static inline void lexer_file_state_path_append_sequence (struct cushion_lexer_file_state_t *state,
                                                          const char *sequence_begin,
                                                          const char *sequence_end)
{
    if (state->path_buffer.size > 0u && state->path_buffer.data[state->path_buffer.size - 1u] != '/' &&
        state->path_buffer.data[state->path_buffer.size - 1u] != '\\')
    {
        // Append directory separator.
        if (state->path_buffer.size >= CUSHION_PATH_BUFFER_SIZE)
        {
            cushion_instance_execution_error (
                state->instance, &state->tokenization,
                "Failed to append directory separator to path container, resulting path is too long. Source path: %s",
                state->path_buffer.data);
        }

        state->path_buffer.data[state->path_buffer.size] = '/';
        ++state->path_buffer.size;
        state->path_buffer.data[state->path_buffer.size] = '\0';
    }

    const size_t in_size = sequence_end - sequence_begin;
    if (state->path_buffer.size + in_size + 1u >= CUSHION_PATH_BUFFER_SIZE)
    {
        cushion_instance_execution_error (
            state->instance, &state->tokenization,
            "Failed to append sequence to path container, resulting path is too long. Source path: %s",
            state->path_buffer.data);
    }

    memcpy (state->path_buffer.data + state->path_buffer.size, sequence_begin, in_size);
    state->path_buffer.size += in_size;
    state->path_buffer.data[state->path_buffer.size] = '\0';
}

struct lex_macro_argument_t
{
    struct lex_macro_argument_t *next;
    struct cushion_token_list_item_t *tokens_first;
};

static unsigned int lex_calculate_stringized_internal_size (struct cushion_token_list_item_t *token)
{
    unsigned int size = 0u;
    while (token)
    {
        switch (token->token.type)
        {
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IFDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IFNDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFNDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELSE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ENDIF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_INCLUDE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_DEFINE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_UNDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_LINE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_PRAGMA:
        case CUSHION_TOKEN_TYPE_NEW_LINE:
        case CUSHION_TOKEN_TYPE_GLUE:
        case CUSHION_TOKEN_TYPE_COMMENT:
        case CUSHION_TOKEN_TYPE_END_OF_FILE:
            // Must never be a part of stringizing sequence.
            assert (0);
            break;

        case CUSHION_TOKEN_TYPE_IDENTIFIER:
        case CUSHION_TOKEN_TYPE_PUNCTUATOR:
        case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:
        case CUSHION_TOKEN_TYPE_NUMBER_FLOATING:
        case CUSHION_TOKEN_TYPE_OTHER:
            size += token->token.end - token->token.begin;
            break;

        case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
        case CUSHION_TOKEN_TYPE_STRING_LITERAL:
        {
            size += token->token.end - token->token.begin;
            if (token->token.type == CUSHION_TOKEN_TYPE_STRING_LITERAL)
            {
                size += 2u; // Two more character for escaping double quotes.
            }

            // By the standard, every "\", even from scape sequence, should be added as "\\".
            const char *cursor = token->token.symbolic_literal.begin;

            while (cursor < token->token.symbolic_literal.end)
            {
                if (*cursor == '\\')
                {
                    size += 2u;
                }

                ++cursor;
            }

            break;
        }
        }

        token = token->next;
        if (token)
        {
            // Symbol for space separator.
            ++size;
        }
    }

    return size;
}

static char *lex_write_stringized_internal_tokens (struct cushion_token_list_item_t *token, char *output)
{
    while (token)
    {
        switch (token->token.type)
        {
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IFDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IFNDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFNDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELSE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ENDIF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_INCLUDE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_DEFINE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_UNDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_LINE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_PRAGMA:
        case CUSHION_TOKEN_TYPE_NEW_LINE:
        case CUSHION_TOKEN_TYPE_GLUE:
        case CUSHION_TOKEN_TYPE_COMMENT:
        case CUSHION_TOKEN_TYPE_END_OF_FILE:
            // Must never be a part of stringizing sequence.
            assert (0);
            break;

        case CUSHION_TOKEN_TYPE_IDENTIFIER:
        case CUSHION_TOKEN_TYPE_PUNCTUATOR:
        case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:
        case CUSHION_TOKEN_TYPE_NUMBER_FLOATING:
        case CUSHION_TOKEN_TYPE_OTHER:
            memcpy (output, token->token.begin, token->token.end - token->token.begin);
            output += token->token.end - token->token.begin;
            break;

        case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
        case CUSHION_TOKEN_TYPE_STRING_LITERAL:
        {
            if (token->token.begin + 1u != token->token.symbolic_literal.begin)
            {
                // Copy encoding prefix.
                memcpy (output, token->token.begin, token->token.symbolic_literal.begin - token->token.begin - 1u);
                output += token->token.symbolic_literal.begin - token->token.begin - 1u;
            }

            if (token->token.type == CUSHION_TOKEN_TYPE_CHARACTER_LITERAL)
            {
                *output = '\'';
                ++output;
            }
            else
            {
                *output = '\\';
                ++output;
                *output = '"';
                ++output;
            }

            // By the standard, every "\", even from scape sequence, should be escaped.
            // So, #X where X is "\"ABC\"" will be "\\\"ABC\\\"".
            const char *cursor = token->token.symbolic_literal.begin;

            while (cursor < token->token.symbolic_literal.end)
            {
                if (*cursor == '\\')
                {
                    *output = '\\';
                    ++output;
                    *output = '\\';
                    ++output;
                }

                *output = *cursor;
                ++output;
                ++cursor;
            }

            if (token->token.type == CUSHION_TOKEN_TYPE_CHARACTER_LITERAL)
            {
                *output = '\'';
                ++output;
            }
            else
            {
                *output = '\\';
                ++output;
                *output = '"';
                ++output;
            }

            break;
        }
        }

        token = token->next;
        if (token)
        {
            *output = ' ';
            ++output;
        }
    }

    return output;
}

static enum cushion_identifier_kind_t lex_relculate_identifier_kind (const char *begin, const char *end)
{
    // Good candidate for future optimization, but should not be that impactful right now.
    const size_t length = end - begin;

#define CHECK_KIND(LITERAL, VALUE)                                                                                     \
    if (length == sizeof (LITERAL) - 1u && strncmp (begin, "identifier", length) == 0)                                 \
    {                                                                                                                  \
        return VALUE;                                                                                                  \
    }

    CHECK_KIND ("__VA_ARGS__", CUSHION_IDENTIFIER_KIND_VA_ARGS)
    CHECK_KIND ("__VA_OPT__", CUSHION_IDENTIFIER_KIND_VA_OPT)

    CHECK_KIND ("__CUSHION_PRESERVE__", CUSHION_IDENTIFIER_KIND_CUSHION_PRESERVE)

    CHECK_KIND ("CUSHION_DEFER", CUSHION_IDENTIFIER_KIND_CUSHION_DEFER)
    CHECK_KIND ("__CUSHION_WRAPPED__", CUSHION_IDENTIFIER_KIND_CUSHION_WRAPPED)
    CHECK_KIND ("CUSHION_STATEMENT_ACCUMULATOR", CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR)
    CHECK_KIND ("CUSHION_STATEMENT_ACCUMULATOR_PUSH", CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_PUSH)
    CHECK_KIND ("CUSHION_STATEMENT_ACCUMULATOR_REFERENCE",
                CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_REFERENCE)

    CHECK_KIND ("__FILE__", CUSHION_IDENTIFIER_KIND_FILE)
    CHECK_KIND ("__LINE__", CUSHION_IDENTIFIER_KIND_LINE)

    CHECK_KIND ("defined", CUSHION_IDENTIFIER_KIND_DEFINED)
    CHECK_KIND ("__has_include", CUSHION_IDENTIFIER_KIND_HAS_INCLUDE)
    CHECK_KIND ("__has_embed", CUSHION_IDENTIFIER_KIND_HAS_EMBED)
    CHECK_KIND ("__has_c_attribute", CUSHION_IDENTIFIER_KIND_HAS_C_ATTRIBUTE)
    CHECK_KIND ("__Pragma", CUSHION_IDENTIFIER_KIND_MACRO_PRAGMA)

    CHECK_KIND ("if", CUSHION_IDENTIFIER_KIND_IF)
    CHECK_KIND ("for", CUSHION_IDENTIFIER_KIND_FOR)
    CHECK_KIND ("while", CUSHION_IDENTIFIER_KIND_WHILE)
    CHECK_KIND ("do", CUSHION_IDENTIFIER_KIND_DO)

    CHECK_KIND ("return", CUSHION_IDENTIFIER_KIND_RETURN)
    CHECK_KIND ("break", CUSHION_IDENTIFIER_KIND_BREAK)
    CHECK_KIND ("continue", CUSHION_IDENTIFIER_KIND_CONTINUE)
    CHECK_KIND ("goto", CUSHION_IDENTIFIER_KIND_GOTO)
#undef CHECK_KIND

    return CUSHION_IDENTIFIER_KIND_REGULAR;
}

struct macro_replacement_token_list_t
{
    struct cushion_token_list_item_t *first;
    struct cushion_token_list_item_t *last;
};

struct macro_replacement_context_t
{
    struct cushion_macro_node_t *macro;
    struct lex_macro_argument_t *arguments;
    struct cushion_token_list_item_t *current_token;
    struct macro_replacement_token_list_t result;
    struct macro_replacement_token_list_t sub_list;
};

static inline void macro_replacement_token_list_append (struct cushion_lexer_file_state_t *state,
                                                        struct macro_replacement_token_list_t *list,
                                                        struct cushion_token_t *token)
{
    struct cushion_token_list_item_t *new_token =
        cushion_allocator_allocate (&state->instance->allocator, sizeof (struct cushion_token_list_item_t),
                                    _Alignof (struct cushion_token_list_item_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);

    // We just copy token value as its string allocations should be already dealt with either through persistent
    // allocation or manual copy.
    new_token->token = *token;
    new_token->next = NULL;

    if (list->last)
    {
        list->last->next = new_token;
    }
    else
    {
        list->first = new_token;
    }

    list->last = new_token;
}

static inline void macro_replacement_context_process_identifier_into_sub_list (
    struct cushion_lexer_file_state_t *state, struct macro_replacement_context_t *context)
{
    context->sub_list.first = NULL;
    context->sub_list.last = NULL;

    if (context->current_token->token.identifier_kind == CUSHION_IDENTIFIER_KIND_VA_ARGS ||
        context->current_token->token.identifier_kind == CUSHION_IDENTIFIER_KIND_VA_OPT)
    {
        if ((context->macro->flags & CUSHION_MACRO_FLAG_VARIADIC_PARAMETERS) == 0u)
        {
            cushion_instance_execution_error (state->instance, &state->tokenization,
                                              "Caught attempt to use __VA_ARGS__/__VA_OPT__ in non-variadic macro.");
            return;
        }

        struct cushion_macro_parameter_node_t *macro_parameter = context->macro->parameters_first;
        struct lex_macro_argument_t *first_variadic_argument = context->arguments;

        while (macro_parameter)
        {
            macro_parameter = macro_parameter->next;
            // There should be no fewer arguments than parameters, otherwise call is malformed.
            assert (first_variadic_argument);
            first_variadic_argument = first_variadic_argument->next;
        }

        if (context->current_token->token.identifier_kind == CUSHION_IDENTIFIER_KIND_VA_ARGS)
        {
            struct lex_macro_argument_t *argument = first_variadic_argument;
            while (argument)
            {
                struct cushion_token_list_item_t *argument_token = argument->tokens_first;
                while (argument_token)
                {
                    macro_replacement_token_list_append (state, &context->sub_list, &argument_token->token);
                    argument_token = argument_token->next;
                }

                argument = argument->next;
                if (argument)
                {
                    static const char *static_token_string_comma = ",";
                    struct cushion_token_t token_comma = {
                        .type = CUSHION_TOKEN_TYPE_PUNCTUATOR,
                        .begin = static_token_string_comma,
                        .end = static_token_string_comma + 1u,
                        .punctuator_kind = CUSHION_PUNCTUATOR_KIND_COMMA,
                    };

                    macro_replacement_token_list_append (state, &context->sub_list, &token_comma);
                    // No need for glue white space, as they are not preserved in replacement lists at all.
                }
            }
        }
        else
        {
            // Scan for the opening parenthesis.
            context->current_token = context->current_token->next;
            // Preprocessor, new line, glue, comment and end of file should never appear here anyway.

            if (!context->current_token || context->current_token->token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
                context->current_token->token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS)
            {
                cushion_instance_execution_error (state->instance, &state->tokenization,
                                                  "Expected \"(\" after __VA_OPT__ in replacement list.");
                return;
            }

            // Scan internals of the __VA_OPT__ macro call.
            unsigned int internal_parenthesis = 0u;
            context->current_token = context->current_token->next;

            while (1u)
            {
                if (!context->current_token)
                {
                    cushion_instance_execution_error (state->instance, &state->tokenization,
                                                      "Got to the end of replacement list while lexing __VA_OPT__.");
                    break;
                }

                // Preprocessor, new line, glue and end of file should never appear here anyway.
                if (context->current_token->token.type == CUSHION_TOKEN_TYPE_PUNCTUATOR)
                {
                    if (context->current_token->token.punctuator_kind == CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS)
                    {
                        ++internal_parenthesis;
                    }
                    else if (context->current_token->token.punctuator_kind == CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS)
                    {
                        if (internal_parenthesis == 0u)
                        {
                            // Whole __VA_OPT__ is lexed.
                            break;
                        }

                        --internal_parenthesis;
                    }
                }

                if (first_variadic_argument)
                {
                    // Add token inside __VA_OPT__ only if there are any variadic arguments.
                    macro_replacement_token_list_append (state, &context->sub_list, &context->current_token->token);
                }

                context->current_token = context->current_token->next;
            }
        }
    }
    else
    {
        struct lex_macro_argument_t *found_argument = context->arguments;
        struct cushion_macro_parameter_node_t *found_parameter = context->macro->parameters_first;

        if (found_parameter)
        {
            const unsigned int hash = cushion_hash_djb2_char_sequence (context->current_token->token.begin,
                                                                       context->current_token->token.end);
            const size_t token_length = context->current_token->token.end - context->current_token->token.begin;

            while (found_parameter)
            {
                if (found_parameter->name_hash == hash && strlen (found_parameter->name) == token_length &&
                    strncmp (found_parameter->name, context->current_token->token.begin, token_length) == 0)
                {
                    break;
                }

                found_parameter = found_parameter->next;
                // There should be no fewer arguments than parameters, otherwise call is malformed.
                assert (found_argument);
                found_argument = found_argument->next;
            }
        }

        if (found_parameter && found_argument)
        {
            struct cushion_token_list_item_t *argument_token = found_argument->tokens_first;
            while (argument_token)
            {
                macro_replacement_token_list_append (state, &context->sub_list, &argument_token->token);
                argument_token = argument_token->next;
            }
        }
        else
        {
            // Just a regular identifier.
            macro_replacement_token_list_append (state, &context->sub_list, &context->current_token->token);
        }
    }
}

static void macro_replacement_context_append_sub_list (struct macro_replacement_context_t *context)
{
    if (context->sub_list.first && context->sub_list.last)
    {
        if (context->result.last)
        {
            context->result.last->next = context->sub_list.first;
        }
        else
        {
            context->result.first = context->sub_list.first;
        }

        context->result.last = context->sub_list.last;
    }
}

static struct cushion_token_list_item_t *lex_do_macro_replacement (struct cushion_lexer_file_state_t *state,
                                                                   struct cushion_macro_node_t *macro,
                                                                   struct lex_macro_argument_t *arguments)
{
    // Technically, full replacement pass is only needed for function-like macros as we need to properly process
    // arguments. However, object-like macros can still use ## operation to merge tokens. It should be processed
    // properly in lex_replacement_list function, but it would complicate that function with knowledge of arguments
    // and would make it logic much more complicated. Therefore, we do full replacement for object-like macros too
    // in this function.

    struct macro_replacement_context_t context = {
        .macro = macro,
        .arguments = arguments,
        .current_token = macro->replacement_list_first,
        .result =
            {
                .first = NULL,
                .last = NULL,
            },
        .sub_list =
            {
                .first = NULL,
                .last = NULL,
            },
    };

    while (context.current_token && !cushion_instance_is_error_signaled (state->instance))
    {
        switch (context.current_token->token.type)
        {
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IFDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IFNDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFNDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELSE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ENDIF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_INCLUDE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_DEFINE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_UNDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_LINE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_PRAGMA:
        case CUSHION_TOKEN_TYPE_NEW_LINE:
        case CUSHION_TOKEN_TYPE_COMMENT:
        case CUSHION_TOKEN_TYPE_GLUE:
        case CUSHION_TOKEN_TYPE_END_OF_FILE:
            // Must never be a part of valid lexed macro.
            assert (0);
            break;

        case CUSHION_TOKEN_TYPE_IDENTIFIER:
        {
            macro_replacement_context_process_identifier_into_sub_list (state, &context);
            if (!cushion_instance_is_error_signaled (state->instance))
            {
                macro_replacement_context_append_sub_list (&context);
            }

            break;
        }

        case CUSHION_TOKEN_TYPE_PUNCTUATOR:
            switch (context.current_token->token.punctuator_kind)
            {
            case CUSHION_PUNCTUATOR_KIND_HASH:
            {
                // Stringify next argument, skipping comments. Error if next token is not an argument.
                context.current_token = context.current_token->next;
                // Preprocessor, new line, glue, comment and end of file should never appear here anyway.

                if (!context.current_token)
                {
                    cushion_instance_execution_error (
                        state->instance, &state->tokenization,
                        "Encountered \"#\" operator as a last token in macro replacement list.");
                    break;
                }

                if (context.current_token->token.type != CUSHION_TOKEN_TYPE_IDENTIFIER)
                {
                    cushion_instance_execution_error (
                        state->instance, &state->tokenization,
                        "Non-comment token following \"#\" operator is not an identifier.");
                    break;
                }

                if (context.current_token->token.identifier_kind == CUSHION_IDENTIFIER_KIND_VA_ARGS)
                {
                    if ((macro->flags & CUSHION_MACRO_FLAG_VARIADIC_PARAMETERS) == 0u)
                    {
                        cushion_instance_execution_error (
                            state->instance, &state->tokenization,
                            "Caught attempt to stringize variadic arguments in non-variadic macro.");
                        break;
                    }

                    struct cushion_macro_parameter_node_t *macro_parameter = macro->parameters_first;
                    struct lex_macro_argument_t *first_variadic_argument = arguments;

                    while (macro_parameter)
                    {
                        macro_parameter = macro_parameter->next;
                        // There should be no fewer arguments than parameters, otherwise call is malformed.
                        assert (first_variadic_argument);
                        first_variadic_argument = first_variadic_argument->next;
                    }

                    unsigned int stringized_size = 2u;
                    struct lex_macro_argument_t *current_argument = first_variadic_argument;

                    while (current_argument)
                    {
                        stringized_size += lex_calculate_stringized_internal_size (current_argument->tokens_first);
                        current_argument = current_argument->next;

                        if (current_argument)
                        {
                            stringized_size += 2u; // As per standard, add ", ";
                        }
                    }

                    struct cushion_token_t stringized_token;
                    stringized_token.type = CUSHION_TOKEN_TYPE_STRING_LITERAL;
                    stringized_token.begin =
                        cushion_allocator_allocate (&state->instance->allocator, stringized_size + 1u, _Alignof (char),
                                                    CUSHION_ALLOCATION_CLASS_TRANSIENT);

                    stringized_token.end = stringized_token.begin + stringized_size;
                    *(char *) stringized_token.end = '\0';

                    stringized_token.symbolic_literal.encoding = CUSHION_TOKEN_SUBSEQUENCE_ENCODING_ORDINARY;
                    stringized_token.symbolic_literal.begin = stringized_token.begin + 1u;
                    stringized_token.symbolic_literal.end = stringized_token.end - 1u;

                    char *output = (char *) stringized_token.symbolic_literal.begin;
                    current_argument = first_variadic_argument;

                    while (current_argument)
                    {
                        output = lex_write_stringized_internal_tokens (current_argument->tokens_first, output);
                        assert (output <= stringized_token.symbolic_literal.end);
                        current_argument = current_argument->next;

                        if (current_argument)
                        {
                            *output = ',';
                            ++output;
                            *output = ' ';
                            ++output;
                        }
                    }

                    *(char *) stringized_token.begin = '"';
                    *(char *) stringized_token.symbolic_literal.end = '"';
                    macro_replacement_token_list_append (state, &context.result, &stringized_token);
                    break;
                }

                const unsigned int hash = cushion_hash_djb2_char_sequence (context.current_token->token.begin,
                                                                           context.current_token->token.end);
                const size_t token_length = context.current_token->token.end - context.current_token->token.begin;
                struct cushion_macro_parameter_node_t *found_parameter = macro->parameters_first;
                struct lex_macro_argument_t *found_argument = arguments;

                while (found_parameter)
                {
                    if (found_parameter->name_hash == hash && strlen (found_parameter->name) == token_length &&
                        strncmp (found_parameter->name, context.current_token->token.begin, token_length) == 0)
                    {
                        break;
                    }

                    found_parameter = found_parameter->next;
                    // There should be no fewer arguments than parameters, otherwise call is malformed.
                    assert (found_argument);
                    found_argument = found_argument->next;
                }

                if (!found_argument)
                {
                    cushion_instance_execution_error (
                        state->instance, &state->tokenization,
                        "Identifier token following \"#\" operator is neither argument name nor __VA_ARGS__.");
                    break;
                }

                const unsigned int stringized_size =
                    2u + lex_calculate_stringized_internal_size (found_argument->tokens_first);

                struct cushion_token_t stringized_token;
                stringized_token.type = CUSHION_TOKEN_TYPE_STRING_LITERAL;
                stringized_token.begin =
                    cushion_allocator_allocate (&state->instance->allocator, stringized_size + 1u, _Alignof (char),
                                                CUSHION_ALLOCATION_CLASS_TRANSIENT);

                stringized_token.end = stringized_token.begin + stringized_size;
                *(char *) stringized_token.end = '\0';

                stringized_token.symbolic_literal.encoding = CUSHION_TOKEN_SUBSEQUENCE_ENCODING_ORDINARY;
                stringized_token.symbolic_literal.begin = stringized_token.begin + 1u;
                stringized_token.symbolic_literal.end = stringized_token.end - 1u;

#if !defined(NDEBUG)
                const char *output_end =
#endif
                    lex_write_stringized_internal_tokens (found_argument->tokens_first,
                                                          (char *) stringized_token.symbolic_literal.begin);

                assert (output_end <= stringized_token.symbolic_literal.end);
                *(char *) stringized_token.begin = '"';
                *(char *) stringized_token.symbolic_literal.end = '"';
                macro_replacement_token_list_append (state, &context.result, &stringized_token);
                break;
            }

            case CUSHION_PUNCTUATOR_KIND_DOUBLE_HASH:
            {
                // Technically, we could optimize token-append operation by firstly calculating whole append sequence
                // and then doing merge, but it makes implementation more complicated and should not affect performance
                // that much, therefore simple merge is used right now.

                if (!context.result.last)
                {
                    cushion_instance_execution_error (
                        state->instance, &state->tokenization,
                        "Encountered \"##\" operator as a first token in macro replacement list.");
                    break;
                }

                if (context.result.last->token.type != CUSHION_TOKEN_TYPE_IDENTIFIER)
                {
                    cushion_instance_execution_error (
                        state->instance, &state->tokenization,
                        "Encountered \"##\" operator after non-identifier token, which is "
                        "currently not supported by Cushion (but possible in standard).");
                    break;
                }

                // Identifiers can actually sometimes be resolved into empty token sequences.
                // Therefore, we need a loop here.

                while (1u)
                {
                    // Find next token for merging.
                    context.current_token = context.current_token->next;
                    // Preprocessor, new line, glue, comment and end of file should never appear here anyway.

                    if (!context.current_token)
                    {
                        cushion_instance_execution_error (
                            state->instance, &state->tokenization,
                            "Encountered \"##\" operator as a last token in macro replacement list.");
                        break;
                    }

#define CHECK_APPENDED_TOKEN_TYPE(TYPE)                                                                                \
    switch (TYPE)                                                                                                      \
    {                                                                                                                  \
    case CUSHION_TOKEN_TYPE_IDENTIFIER:                                                                                \
    case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:                                                                            \
        /* We know how to append that to identifier. */                                                                \
        break;                                                                                                         \
                                                                                                                       \
    default:                                                                                                           \
        cushion_instance_execution_error (                                                                             \
            state->instance, &state->tokenization,                                                                     \
            "Encountered \"##\" operator before token which is not an identifier and not an integer, "                 \
            "which is currently not supported by Cushion (but possible in standard).");                                \
        break;                                                                                                         \
    }                                                                                                                  \
                                                                                                                       \
    if (cushion_instance_is_error_signaled (state->instance))                                                          \
    {                                                                                                                  \
        break;                                                                                                         \
    }

                    CHECK_APPENDED_TOKEN_TYPE (context.current_token->token.type)
                    macro_replacement_context_process_identifier_into_sub_list (state, &context);

                    if (!context.sub_list.first)
                    {
                        // Empty sub list, check next identifier.
                        continue;
                    }

                    CHECK_APPENDED_TOKEN_TYPE (context.sub_list.first->token.type)
#undef CHECK_APPENDED_TOKEN_TYPE

                    unsigned int base_identifier_length =
                        context.result.last->token.end - context.result.last->token.begin;
                    unsigned int append_identifier_length =
                        context.sub_list.first->token.end - context.sub_list.first->token.begin;

                    char *new_token_data = cushion_allocator_allocate (
                        &state->instance->allocator, base_identifier_length + append_identifier_length + 1u,
                        _Alignof (char), CUSHION_ALLOCATION_CLASS_TRANSIENT);
                    char *new_token_data_end = new_token_data + base_identifier_length + append_identifier_length;

                    memcpy (new_token_data, context.result.last->token.begin, base_identifier_length);
                    memcpy (new_token_data + base_identifier_length, context.sub_list.first->token.begin,
                            append_identifier_length);
                    *new_token_data_end = '\0';

                    context.result.last->token.begin = new_token_data;
                    context.result.last->token.end = new_token_data_end;
                    context.result.last->token.identifier_kind = lex_relculate_identifier_kind (
                        context.result.last->token.begin, context.result.last->token.end);

                    context.sub_list.first = context.sub_list.first->next;
                    macro_replacement_context_append_sub_list (&context);
                    break;
                }

                break;
            }

            default:
                macro_replacement_token_list_append (state, &context.result, &context.current_token->token);
                break;
            }

            break;

        case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:
        case CUSHION_TOKEN_TYPE_NUMBER_FLOATING:
        case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
        case CUSHION_TOKEN_TYPE_STRING_LITERAL:
        case CUSHION_TOKEN_TYPE_OTHER:
            macro_replacement_token_list_append (state, &context.result, &context.current_token->token);
            break;
        }

        // Tokens might be consumed internally in switch in rare cases, therefore we need to check for NULL again.
        if (context.current_token)
        {
            context.current_token = context.current_token->next;
        }
    }

    return context.result.first;
}

enum lex_replace_identifier_if_macro_context_t
{
    LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_CODE = 0u,
    LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_EVALUATION,
};

static struct cushion_token_list_item_t *lex_replace_identifier_if_macro (
    struct cushion_lexer_file_state_t *state,
    struct cushion_token_t *identifier_token,
    enum lex_replace_identifier_if_macro_context_t context);

static inline void lex_preprocessor_fixup_line (struct cushion_lexer_file_state_t *state)
{
    if ((state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u &&
        (!state->conditional_inclusion_node ||
         state->conditional_inclusion_node->state == CONDITIONAL_INCLUSION_STATE_INCLUDED))
    {
        // Make sure that next portion of code starts with correct line directive.
        // Useful for conditional inclusion and for file includes.
        cushion_instance_output_line_marker (state->instance, state->tokenization.cursor_line,
                                             state->tokenization.file_name);
    }
}

#define LEX_WHEN_ERROR(...)                                                                                            \
    if (cushion_instance_is_error_signaled (state->instance))                                                          \
    {                                                                                                                  \
        __VA_ARGS__;                                                                                                   \
    }

static inline void lex_preprocessor_preserved_tail (
    struct cushion_lexer_file_state_t *state,
    enum cushion_token_type_t preprocessor_token_type,
    // Macro node is required to properly paste function-line macro arguments if any.
    struct cushion_macro_node_t *macro_node_if_any)
{
    if (state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY)
    {
        // When using scan only, we're not outputting anything, therefore no need for actual pass.
        return;
    }

    switch (preprocessor_token_type)
    {
    case CUSHION_TOKEN_TYPE_PREPROCESSOR_IF:
        cushion_instance_output_null_terminated (state->instance, "#if ");
        break;

    case CUSHION_TOKEN_TYPE_PREPROCESSOR_IFDEF:
    case CUSHION_TOKEN_TYPE_PREPROCESSOR_IFNDEF:
        // This token should not support preserve logic.
        assert (0u);
        break;

    case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIF:
        cushion_instance_output_null_terminated (state->instance, "#elif ");
        break;

    case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFDEF:
        cushion_instance_output_null_terminated (state->instance, "#elifdef ");
        break;

    case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFNDEF:
        cushion_instance_output_null_terminated (state->instance, "#elifndef ");
        break;

    case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELSE:
        cushion_instance_output_null_terminated (state->instance, "#else ");
        break;

    case CUSHION_TOKEN_TYPE_PREPROCESSOR_ENDIF:
        cushion_instance_output_null_terminated (state->instance, "#endif ");
        break;

    case CUSHION_TOKEN_TYPE_PREPROCESSOR_INCLUDE:
    case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
    case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
    case CUSHION_TOKEN_TYPE_PREPROCESSOR_UNDEF:
    case CUSHION_TOKEN_TYPE_PREPROCESSOR_LINE:
        // This token should not support preserve logic.
        assert (0u);
        break;

    case CUSHION_TOKEN_TYPE_PREPROCESSOR_PRAGMA:
        cushion_instance_output_null_terminated (state->instance, "#pragma ");
        break;

    case CUSHION_TOKEN_TYPE_PREPROCESSOR_DEFINE:
    {
        cushion_instance_output_null_terminated (state->instance, "#define ");
        // We need this to properly paste everything back into code.
        assert (macro_node_if_any);
        cushion_instance_output_null_terminated (state->instance, macro_node_if_any->name);

        if (macro_node_if_any->flags & CUSHION_MACRO_FLAG_FUNCTION)
        {
            cushion_instance_output_null_terminated (state->instance, "(");
            struct cushion_macro_parameter_node_t *parameter = macro_node_if_any->parameters_first;

            while (parameter)
            {
                cushion_instance_output_null_terminated (state->instance, parameter->name);
                parameter = parameter->next;

                if (parameter)
                {
                    cushion_instance_output_null_terminated (state->instance, ", ");
                }
            }

            if (macro_node_if_any->flags & CUSHION_MACRO_FLAG_VARIADIC_PARAMETERS)
            {
                if (macro_node_if_any->parameters_first)
                {
                    cushion_instance_output_null_terminated (state->instance, ", ");
                }

                cushion_instance_output_null_terminated (state->instance, "...");
            }

            cushion_instance_output_null_terminated (state->instance, ") ");
        }
        else
        {
            cushion_instance_output_null_terminated (state->instance, " ");
        }

        break;
    }

    case CUSHION_TOKEN_TYPE_IDENTIFIER:
    case CUSHION_TOKEN_TYPE_PUNCTUATOR:
    case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:
    case CUSHION_TOKEN_TYPE_NUMBER_FLOATING:
    case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
    case CUSHION_TOKEN_TYPE_STRING_LITERAL:
    case CUSHION_TOKEN_TYPE_NEW_LINE:
    case CUSHION_TOKEN_TYPE_GLUE:
    case CUSHION_TOKEN_TYPE_COMMENT:
    case CUSHION_TOKEN_TYPE_END_OF_FILE:
    case CUSHION_TOKEN_TYPE_OTHER:
        // We shouldn't pass regular tokens as preprocessor tokens.
        assert (0u);
        break;
    }

    struct cushion_token_t current_token;
    while (lexer_file_state_should_continue (state))
    {
        lexer_file_state_pop_token (state, &current_token);
        LEX_WHEN_ERROR (break)

        switch (current_token.type)
        {
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IFDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IFNDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFNDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELSE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ENDIF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_INCLUDE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_DEFINE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_UNDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_LINE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_PRAGMA:
            cushion_instance_execution_error (state->instance, &state->tokenization,
                                              "Encountered preprocessor directive while lexing preserved preprocessor "
                                              "directive. Shouldn't be possible at all, can be an internal error.");
            return;

        case CUSHION_TOKEN_TYPE_IDENTIFIER:
        {
            switch (current_token.identifier_kind)
            {
            case CUSHION_IDENTIFIER_KIND_CUSHION_PRESERVE:
                cushion_instance_execution_error (state->instance, &state->tokenization,
                                                  "Encountered cushion preserve keyword in unexpected context.");
                return;

            case CUSHION_IDENTIFIER_KIND_CUSHION_WRAPPED:
                cushion_instance_execution_error (
                    state->instance, &state->tokenization,
                    "Encountered cushion wrapped keyword in preserved preprocessor context.");
                return;

            default:
                break;
            }

            struct cushion_token_list_item_t *macro_tokens = lex_replace_identifier_if_macro (
                state, &current_token, LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_EVALUATION);

            if (macro_tokens)
            {
                lexer_file_state_push_tokens (state, macro_tokens, LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT);
            }
            else
            {
                // Not a macro, just regular identifier.
                cushion_instance_output_sequence (state->instance, current_token.begin, current_token.end);
            }

            break;
        }

        case CUSHION_TOKEN_TYPE_PUNCTUATOR:
        case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:
        case CUSHION_TOKEN_TYPE_NUMBER_FLOATING:
        case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
        case CUSHION_TOKEN_TYPE_STRING_LITERAL:
        case CUSHION_TOKEN_TYPE_GLUE:
        case CUSHION_TOKEN_TYPE_OTHER:
            cushion_instance_output_sequence (state->instance, current_token.begin, current_token.end);
            break;

        case CUSHION_TOKEN_TYPE_NEW_LINE:
            // New line, tail has ended, paste the new line and return out of here.
            cushion_instance_output_sequence (state->instance, current_token.begin, current_token.end);
            // Fixup line in case of skipped multi-line comments.
            lex_preprocessor_fixup_line (state);
            return;

        case CUSHION_TOKEN_TYPE_COMMENT:
            // We erase comments.
            break;

        case CUSHION_TOKEN_TYPE_END_OF_FILE:
            // Just return, nothing more to output, nothing to fixup.
            return;
        }
    }
}

static struct cushion_token_list_item_t *lex_replace_identifier_if_macro (
    struct cushion_lexer_file_state_t *state,
    struct cushion_token_t *identifier_token,
    enum lex_replace_identifier_if_macro_context_t context)
{
    struct cushion_macro_node_t *macro =
        cushion_instance_macro_search (state->instance, identifier_token->begin, identifier_token->end);
    if (!macro || (macro->flags & CUSHION_MACRO_FLAG_PRESERVED))
    {
        // No need to unwrap.
        return NULL;
    }

    struct lex_macro_argument_t *arguments_first = NULL;
    struct lex_macro_argument_t *arguments_last = NULL;

    if (macro->flags & CUSHION_MACRO_FLAG_FUNCTION)
    {
        /* Scan for the opening parenthesis. */
        struct cushion_token_t current_token;

        struct cushion_token_list_item_t *argument_tokens_first = NULL;
        struct cushion_token_list_item_t *argument_tokens_last = NULL;
        unsigned int skipping_until_significant = 1u;

        while (skipping_until_significant && !cushion_instance_is_error_signaled (state->instance))
        {
            lexer_file_state_pop_token (state, &current_token);
            LEX_WHEN_ERROR (break)

            switch (current_token.type)
            {
            case CUSHION_TOKEN_TYPE_NEW_LINE:
                switch (context)
                {
                case LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_CODE:
                    // As macro replacement cannot have new lines inside them, then we cannot be in macro replacement
                    // and therefore can freely output new line.
                    cushion_instance_output_sequence (state->instance, current_token.begin, current_token.end);
                    break;

                case LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_EVALUATION:
                    cushion_instance_execution_error (
                        state->instance, &state->tokenization,
                        "Reached new line while expecting \"(\" after function-line macro name "
                        "inside preprocessor directive evaluation.");
                    break;
                }

                break;

            case CUSHION_TOKEN_TYPE_GLUE:
            case CUSHION_TOKEN_TYPE_COMMENT:
                // Glue and comments between macro name and arguments are just skipped.
                break;

            default:
                // Found something significant.
                skipping_until_significant = 0u;
                break;
            }
        }

        LEX_WHEN_ERROR (return NULL)
        if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
            current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS)
        {
            cushion_instance_execution_error (state->instance, &state->tokenization,
                                              "Expected \"(\" after function-line macro name.");
            return NULL;
        }

        unsigned int parenthesis_counter = 1u;
        struct cushion_macro_parameter_node_t *parameter = macro->parameters_first;
        unsigned int parameterless_function_line =
            !parameter && (macro->flags & CUSHION_MACRO_FLAG_VARIADIC_PARAMETERS) == 0u;

        while (parenthesis_counter > 0u && !cushion_instance_is_error_signaled (state->instance))
        {
            lexer_file_state_pop_token (state, &current_token);
            LEX_WHEN_ERROR (break)

            switch (current_token.type)
            {
            case CUSHION_TOKEN_TYPE_PUNCTUATOR:
                switch (current_token.punctuator_kind)
                {
                case CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS:
                    ++parenthesis_counter;
                    goto append_argument_token;
                    break;

                case CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS:
                    --parenthesis_counter;

                    if (parenthesis_counter == 0u && !parameterless_function_line)
                    {
                        goto finalize_argument;
                    }

                    break;

                case CUSHION_PUNCTUATOR_KIND_COMMA:
                finalize_argument:
                {
                    if (parameterless_function_line)
                    {
                        cushion_instance_execution_error (
                            state->instance, &state->tokenization,
                            "Encountered more parameters for function-line macro than expected.");
                        break;
                    }

                    struct lex_macro_argument_t *new_argument = cushion_allocator_allocate (
                        &state->instance->allocator, sizeof (struct lex_macro_argument_t),
                        _Alignof (struct lex_macro_argument_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);

                    new_argument->next = NULL;
                    new_argument->tokens_first = argument_tokens_first;
                    argument_tokens_first = NULL;
                    argument_tokens_last = NULL;

                    if (arguments_last)
                    {
                        arguments_last->next = new_argument;
                    }
                    else
                    {
                        arguments_first = new_argument;
                    }

                    arguments_last = new_argument;
                    assert (parameter || (macro->flags & CUSHION_MACRO_FLAG_VARIADIC_PARAMETERS));

                    if (parameter)
                    {
                        parameter = parameter->next;
                    }

                    break;
                }

                default:
                    goto append_argument_token;
                    break;
                }

                break;

            case CUSHION_TOKEN_TYPE_NEW_LINE:
                switch (context)
                {
                case LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_CODE:
                    // As macro replacement cannot have new lines inside them, then we cannot be in macro replacement
                    // and therefore can freely output new line while parsing arguments.
                    cushion_instance_output_sequence (state->instance, current_token.begin, current_token.end);
                    break;

                case LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_EVALUATION:
                    cushion_instance_execution_error (
                        state->instance, &state->tokenization,
                        "Reached new line while parsing arguments of function-line macro inside "
                        "preprocessor directive evaluation.");
                    break;
                }

                break;

            case CUSHION_TOKEN_TYPE_GLUE:
            case CUSHION_TOKEN_TYPE_COMMENT:
                // Glue and comments are not usually kept in macro arguments.
                break;

            case CUSHION_TOKEN_TYPE_END_OF_FILE:
                cushion_instance_execution_error (
                    state->instance, &state->tokenization,
                    "Got to the end of file while parsing arguments of function-like macro.");
                break;

            default:
            append_argument_token:
            {
                if (!parameter && (macro->flags & CUSHION_MACRO_FLAG_VARIADIC_PARAMETERS) == 0u)
                {
                    cushion_instance_execution_error (
                        state->instance, &state->tokenization,
                        "Encountered more parameters for function-line macro than expected.");
                    break;
                }

                struct cushion_token_list_item_t *argument_token =
                    cushion_save_token_to_memory (state->instance, &current_token, CUSHION_ALLOCATION_CLASS_TRANSIENT);

                if (argument_tokens_last)
                {
                    argument_tokens_last->next = argument_token;
                }
                else
                {
                    argument_tokens_first = argument_token;
                }

                argument_tokens_last = argument_token;
                break;
            }
            }
        }

        LEX_WHEN_ERROR (return NULL)
        if (parameter)
        {
            cushion_instance_execution_error (state->instance, &state->tokenization,
                                              "Encountered less parameters for function-line macro than expected.");
            return NULL;
        }
    }

    return lex_do_macro_replacement (state, macro, arguments_first);
}

enum lex_preprocessor_sub_expression_type_t
{
    LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_ROOT = 0u,
    LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_PARENTHESIS,
    LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_POSITIVE,
    LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_NEGATIVE,
};

static long long lex_preprocessor_evaluate (struct cushion_lexer_file_state_t *state,
                                            enum lex_preprocessor_sub_expression_type_t sub_expression_type);

static inline void lex_skip_glue_and_comments (struct cushion_lexer_file_state_t *state,
                                               struct cushion_token_t *current_token)
{
    while (lexer_file_state_should_continue (state))
    {
        lexer_file_state_pop_token (state, current_token);
        LEX_WHEN_ERROR (return)

        switch (current_token->type)
        {
        case CUSHION_TOKEN_TYPE_GLUE:
        case CUSHION_TOKEN_TYPE_COMMENT:
            break;

        default:
            return;
        }
    }
}

static long long lex_do_defined_check (struct cushion_lexer_file_state_t *state, struct cushion_token_t *current_token)
{
    switch (current_token->type)
    {
    case CUSHION_TOKEN_TYPE_IDENTIFIER:
        switch (current_token->identifier_kind)
        {
        case CUSHION_IDENTIFIER_KIND_VA_ARGS:
        case CUSHION_IDENTIFIER_KIND_VA_OPT:
        case CUSHION_IDENTIFIER_KIND_CUSHION_PRESERVE:
        case CUSHION_IDENTIFIER_KIND_CUSHION_DEFER:
        case CUSHION_IDENTIFIER_KIND_CUSHION_WRAPPED:
        case CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR:
        case CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_PUSH:
        case CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_REFERENCE:
            cushion_instance_execution_error (state->instance, &state->tokenization,
                                              "Encountered unsupported reserved identifier in defined check.");
            return 0u;

        default:
            return cushion_instance_macro_search (state->instance, current_token->begin, current_token->end) ? 1u : 0u;
        }

        break;

    default:
        cushion_instance_execution_error (state->instance, &state->tokenization,
                                          "Expected identifier for defined check.");
        return 0u;
    }

    return 0u;
}

static long long lex_preprocessor_evaluate_defined (struct cushion_lexer_file_state_t *state)
{
    struct cushion_token_t current_token;
    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return 0u)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS)
    {
        cushion_instance_execution_error (state->instance, &state->tokenization,
                                          "Expected \"(\" after \"defined\" in preprocessor expression evaluation.");
        return 0u;
    }

    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return 0u)

    long long result = lex_do_defined_check (state, &current_token);
    LEX_WHEN_ERROR (return 0u)

    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return 0u)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS)
    {
        cushion_instance_execution_error (
            state->instance, &state->tokenization,
            "Expected \")\" after macro name in \"defined\" in preprocessor expression evaluation.");
        return result;
    }

    return result;
}

static long long lex_preprocessor_evaluate_argument (struct cushion_lexer_file_state_t *state,
                                                     enum lex_preprocessor_sub_expression_type_t sub_expression_type)
{
    struct cushion_token_t current_token;
    while (lexer_file_state_should_continue (state))
    {
        lexer_file_state_pop_token (state, &current_token);
        LEX_WHEN_ERROR (break)

        switch (current_token.type)
        {
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IFDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IFNDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFNDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELSE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ENDIF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_INCLUDE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_DEFINE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_UNDEF:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_LINE:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_PRAGMA:
            cushion_instance_execution_error (
                state->instance, &state->tokenization,
                "Encountered preprocessor directive while evaluating preprocessor conditional "
                "expression. Shouldn't be possible at all, can be an internal error.");
            return 0;

        case CUSHION_TOKEN_TYPE_IDENTIFIER:
        {
            switch (current_token.identifier_kind)
            {
            case CUSHION_IDENTIFIER_KIND_LINE:
                return state->tokenization.cursor_line;

            case CUSHION_IDENTIFIER_KIND_DEFINED:
                return lex_preprocessor_evaluate_defined (state);

            case CUSHION_IDENTIFIER_KIND_HAS_INCLUDE:
            case CUSHION_IDENTIFIER_KIND_HAS_EMBED:
            case CUSHION_IDENTIFIER_KIND_HAS_C_ATTRIBUTE:
            {
                cushion_instance_execution_error (
                    state->instance, &state->tokenization,
                    "Encountered has_* check while evaluation preprocessor conditional expression. These checks are "
                    "not supported as Cushion is not guaranteed to have enough info to process them properly.");
                return 0;
                break;
            }

            default:
            {
                struct cushion_token_list_item_t *macro_tokens = lex_replace_identifier_if_macro (
                    state, &current_token, LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_EVALUATION);

                if (!macro_tokens)
                {
                    cushion_instance_execution_error (
                        state->instance, &state->tokenization,
                        "Encountered identifier which can not be unwrapped as macro while evaluation "
                        "preprocessor conditional expression. Identifiers can not be present in these "
                        "expressions as they must be integer constants.");
                    return 0;
                }

                lexer_file_state_push_tokens (state, macro_tokens, LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT);
                break;
            }
            }

            break;
        }

        case CUSHION_TOKEN_TYPE_PUNCTUATOR:
            switch (current_token.punctuator_kind)
            {
            case CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS:
                // Encountered sub expression as argument.
                return lex_preprocessor_evaluate (state, LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_PARENTHESIS);

            case CUSHION_PUNCTUATOR_KIND_BITWISE_INVERSE:
                // Unary "~". Tail-recurse to make code easier.
                return ~lex_preprocessor_evaluate_argument (state, sub_expression_type);

            case CUSHION_PUNCTUATOR_KIND_PLUS:
                // Unary "+". Tail-recurse to make code easier.
                return lex_preprocessor_evaluate_argument (state, sub_expression_type);

            case CUSHION_PUNCTUATOR_KIND_MINUS:
                // Unary "-". Tail-recurse to make code easier.
                return -lex_preprocessor_evaluate_argument (state, sub_expression_type);

            case CUSHION_PUNCTUATOR_KIND_LOGICAL_NOT:
                // Unary "!". Tail-recurse to make code easier.
                return !lex_preprocessor_evaluate_argument (state, sub_expression_type);

            default:
                cushion_instance_execution_error (
                    state->instance, &state->tokenization,
                    "Encountered unexpected punctuator while evaluating preprocessor conditional expression.");
                return 0;
            }

            // Should never happen unless memory is corrupted.
            assert (0);
            return 0;

        case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:
            if (current_token.unsigned_number_value > LLONG_MAX)
            {
                cushion_instance_execution_error (
                    state->instance, &state->tokenization,
                    "Encountered integer number which is higher than %lld (LLONG_MAX) while evaluating preprocessor "
                    "conditional expression, which is not supported by Cushion right now.",
                    (long long) LLONG_MAX);
                return 0;
            }

            return (long long) current_token.unsigned_number_value;

        case CUSHION_TOKEN_TYPE_NUMBER_FLOATING:
            cushion_instance_execution_error (
                state->instance, &state->tokenization,
                "Encountered non-integer number while evaluating preprocessor conditional "
                "expression, which is not supported by specification.");
            return 0;

        case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
            if (current_token.symbolic_literal.encoding != CUSHION_TOKEN_SUBSEQUENCE_ENCODING_ORDINARY)
            {
                cushion_instance_execution_error (
                    state->instance, &state->tokenization,
                    "Encountered non-ordinary character literal while evaluating preprocessor "
                    "conditional expression, which is currently not supported by Cushion.");
                return 0;
            }

            if (current_token.symbolic_literal.end - current_token.symbolic_literal.begin != 1u)
            {
                cushion_instance_execution_error (
                    state->instance, &state->tokenization,
                    "Encountered non-single-character character literal while evaluating preprocessor conditional "
                    "expression, which is currently not supported by Cushion.");
                return 0;
            }

            return (long long) *current_token.symbolic_literal.begin;

        case CUSHION_TOKEN_TYPE_STRING_LITERAL:
            cushion_instance_execution_error (
                state->instance, &state->tokenization,
                "Encountered string literal while evaluating preprocessor conditional expression, "
                "which is not supported by specification.");
            return 0;

        case CUSHION_TOKEN_TYPE_NEW_LINE:
            cushion_instance_execution_error (
                state->instance, &state->tokenization,
                "Encountered end of line while expecting next argument for preprocessor conditional expression.");
            return 0;

        case CUSHION_TOKEN_TYPE_GLUE:
        case CUSHION_TOKEN_TYPE_COMMENT:
            // Never interested in it inside conditional, continue lexing.
            break;

        case CUSHION_TOKEN_TYPE_END_OF_FILE:
            cushion_instance_execution_error (
                state->instance, &state->tokenization,
                "Encountered end of file while expecting next argument for preprocessor conditional expression.");
            return 0;

        case CUSHION_TOKEN_TYPE_OTHER:
            cushion_instance_execution_error (
                state->instance, &state->tokenization,
                "Encountered unknown token expecting next argument for preprocessor conditional expression.");
            return 0;
        }
    }

    // Shouldn't exit that way unless error has occurred.
    assert (cushion_instance_is_error_signaled (state->instance));
    return 0;
}

/// \details Only operators applicable to preprocessor evaluation are listed.
enum lex_evaluate_operator_t
{
    LEX_EVALUATE_OPERATOR_MULTIPLY = 0u,
    LEX_EVALUATE_OPERATOR_DIVIDE,
    LEX_EVALUATE_OPERATOR_MODULO,

    LEX_EVALUATE_OPERATOR_ADD,
    LEX_EVALUATE_OPERATOR_SUBTRACT,

    LEX_EVALUATE_OPERATOR_BITWISE_LEFT_SHIFT,
    LEX_EVALUATE_OPERATOR_BITWISE_RIGHT_SHIFT,

    LEX_EVALUATE_OPERATOR_LESS,
    LEX_EVALUATE_OPERATOR_GREATER,
    LEX_EVALUATE_OPERATOR_LESS_OR_EQUAL,
    LEX_EVALUATE_OPERATOR_GREATER_OR_EQUAL,

    LEX_EVALUATE_OPERATOR_EQUAL,
    LEX_EVALUATE_OPERATOR_NOT_EQUAL,

    LEX_EVALUATE_OPERATOR_BITWISE_AND,
    LEX_EVALUATE_OPERATOR_BITWISE_XOR,
    LEX_EVALUATE_OPERATOR_BITWISE_OR,

    LEX_EVALUATE_OPERATOR_LOGICAL_AND,
    LEX_EVALUATE_OPERATOR_LOGICAL_OR,

    LEX_EVALUATE_OPERATOR_TERNARY,
    LEX_EVALUATE_OPERATOR_COMMA,
};

enum lex_evaluate_operator_associativity_t
{
    LEX_EVALUATE_OPERATOR_ASSOCIATIVITY_LEFT_TO_RIGHT = 0u,
    LEX_EVALUATE_OPERATOR_ASSOCIATIVITY_RIGHT_TO_LEFT,
};

#define LEX_TERNARY_PRECEDENCE 13u                                                  // Used as constant.
#define LEX_TERNARY_ASSOCIATIVITY LEX_EVALUATE_OPERATOR_ASSOCIATIVITY_RIGHT_TO_LEFT // Used as constant.

static inline unsigned int lex_evaluate_operator_precedence (enum lex_evaluate_operator_t operator)
{
    // Values are just copied from https://en.cppreference.com/w/c/language/operator_precedence for the ease of use.
    switch (operator)
    {
    case LEX_EVALUATE_OPERATOR_MULTIPLY:
    case LEX_EVALUATE_OPERATOR_DIVIDE:
    case LEX_EVALUATE_OPERATOR_MODULO:
        return 3u;

    case LEX_EVALUATE_OPERATOR_ADD:
    case LEX_EVALUATE_OPERATOR_SUBTRACT:
        return 4u;

    case LEX_EVALUATE_OPERATOR_BITWISE_LEFT_SHIFT:
    case LEX_EVALUATE_OPERATOR_BITWISE_RIGHT_SHIFT:
        return 5u;

    case LEX_EVALUATE_OPERATOR_LESS:
    case LEX_EVALUATE_OPERATOR_GREATER:
    case LEX_EVALUATE_OPERATOR_LESS_OR_EQUAL:
    case LEX_EVALUATE_OPERATOR_GREATER_OR_EQUAL:
        return 6u;

    case LEX_EVALUATE_OPERATOR_EQUAL:
    case LEX_EVALUATE_OPERATOR_NOT_EQUAL:
        return 7u;

    case LEX_EVALUATE_OPERATOR_BITWISE_AND:
        return 8u;

    case LEX_EVALUATE_OPERATOR_BITWISE_XOR:
        return 9u;

    case LEX_EVALUATE_OPERATOR_BITWISE_OR:
        return 10u;

    case LEX_EVALUATE_OPERATOR_LOGICAL_AND:
        return 11u;

    case LEX_EVALUATE_OPERATOR_LOGICAL_OR:
        return 12u;

    case LEX_EVALUATE_OPERATOR_TERNARY:
        return LEX_TERNARY_PRECEDENCE;

    case LEX_EVALUATE_OPERATOR_COMMA:
        return 14u;
    }

    assert (0u);
    return 100u;
}

static inline enum lex_evaluate_operator_associativity_t lex_evaluate_operator_associativity (
    enum lex_evaluate_operator_t operator)
{
    switch (operator)
    {
    case LEX_EVALUATE_OPERATOR_MULTIPLY:
    case LEX_EVALUATE_OPERATOR_DIVIDE:
    case LEX_EVALUATE_OPERATOR_MODULO:
    case LEX_EVALUATE_OPERATOR_ADD:
    case LEX_EVALUATE_OPERATOR_SUBTRACT:
    case LEX_EVALUATE_OPERATOR_BITWISE_LEFT_SHIFT:
    case LEX_EVALUATE_OPERATOR_BITWISE_RIGHT_SHIFT:
    case LEX_EVALUATE_OPERATOR_LESS:
    case LEX_EVALUATE_OPERATOR_GREATER:
    case LEX_EVALUATE_OPERATOR_LESS_OR_EQUAL:
    case LEX_EVALUATE_OPERATOR_GREATER_OR_EQUAL:
    case LEX_EVALUATE_OPERATOR_EQUAL:
    case LEX_EVALUATE_OPERATOR_NOT_EQUAL:
    case LEX_EVALUATE_OPERATOR_BITWISE_AND:
    case LEX_EVALUATE_OPERATOR_BITWISE_XOR:
    case LEX_EVALUATE_OPERATOR_BITWISE_OR:
    case LEX_EVALUATE_OPERATOR_LOGICAL_AND:
    case LEX_EVALUATE_OPERATOR_LOGICAL_OR:
    case LEX_EVALUATE_OPERATOR_COMMA:
        return LEX_EVALUATE_OPERATOR_ASSOCIATIVITY_LEFT_TO_RIGHT;

    case LEX_EVALUATE_OPERATOR_TERNARY:
        return LEX_TERNARY_ASSOCIATIVITY;
    }

    assert (0u);
    return LEX_EVALUATE_OPERATOR_ASSOCIATIVITY_LEFT_TO_RIGHT;
}

static long long lex_evaluate_operation (long long left, enum lex_evaluate_operator_t operator, long long right)
{
    switch (operator)
    {
    case LEX_EVALUATE_OPERATOR_MULTIPLY:
        return left * right;

    case LEX_EVALUATE_OPERATOR_DIVIDE:
        return left / right;

    case LEX_EVALUATE_OPERATOR_MODULO:
        return left % right;

    case LEX_EVALUATE_OPERATOR_ADD:
        return left + right;

    case LEX_EVALUATE_OPERATOR_SUBTRACT:
        return left - right;

    case LEX_EVALUATE_OPERATOR_BITWISE_LEFT_SHIFT:
        return left << right;

    case LEX_EVALUATE_OPERATOR_BITWISE_RIGHT_SHIFT:
        return left >> right;

    case LEX_EVALUATE_OPERATOR_LESS:
        return left < right;

    case LEX_EVALUATE_OPERATOR_GREATER:
        return left > right;

    case LEX_EVALUATE_OPERATOR_LESS_OR_EQUAL:
        return left <= right;

    case LEX_EVALUATE_OPERATOR_GREATER_OR_EQUAL:
        return left >= right;

    case LEX_EVALUATE_OPERATOR_EQUAL:
        return left == right;

    case LEX_EVALUATE_OPERATOR_NOT_EQUAL:
        return left != right;

    case LEX_EVALUATE_OPERATOR_BITWISE_AND:
        return left & right;

    case LEX_EVALUATE_OPERATOR_BITWISE_XOR:
        return left ^ right;

    case LEX_EVALUATE_OPERATOR_BITWISE_OR:
        return left | right;

    case LEX_EVALUATE_OPERATOR_LOGICAL_AND:
        return left && right;

    case LEX_EVALUATE_OPERATOR_LOGICAL_OR:
        return left || right;

    case LEX_EVALUATE_OPERATOR_TERNARY:
        // Ternary evaluation is a separate unique routine and should not happen here.
        assert (0u);
        return 0u;

    case LEX_EVALUATE_OPERATOR_COMMA:
        return right;
    }

    assert (0u);
    return 0u;
}

static inline unsigned int lex_evaluate_is_operation_precedes (
    unsigned int left_precedence,
    unsigned int right_precedence,
    // Associativity must be the same for operations with the same precedence.
    enum lex_evaluate_operator_associativity_t associativity)
{
    if (left_precedence == right_precedence)
    {
        switch (associativity)
        {
        case LEX_EVALUATE_OPERATOR_ASSOCIATIVITY_LEFT_TO_RIGHT:
            return 1u;

        case LEX_EVALUATE_OPERATOR_ASSOCIATIVITY_RIGHT_TO_LEFT:
            return 0u;
        }
    }

    return left_precedence < right_precedence;
}

struct lex_evaluate_stack_item_t
{
    struct lex_evaluate_stack_item_t *previous;
    long long left_value;
    enum lex_evaluate_operator_t operator;
    unsigned int precedence;
    enum lex_evaluate_operator_associativity_t associativity;
};

static long long lex_preprocessor_evaluate (struct cushion_lexer_file_state_t *state,
                                            enum lex_preprocessor_sub_expression_type_t sub_expression_type)
{
    struct cushion_token_t current_token;
    struct lex_evaluate_stack_item_t *evaluation_stack_top = NULL;
    struct lex_evaluate_stack_item_t *evaluation_stack_reuse = NULL;

    while (lexer_file_state_should_continue (state))
    {
        long long new_argument = lex_preprocessor_evaluate_argument (state, sub_expression_type);

        // We need a label due to custom logic needed to process ternary operator.
        // When ternary is fully processed, it is treated as single argument and operator
        // after it is requested right away.
    lex_next_operator:
        lex_skip_glue_and_comments (state, &current_token);
        LEX_WHEN_ERROR (return 0u)

        switch (current_token.type)
        {
        case CUSHION_TOKEN_TYPE_PUNCTUATOR:
        {
            enum lex_evaluate_operator_t next_operator = LEX_EVALUATE_OPERATOR_MULTIPLY;
            switch (current_token.punctuator_kind)
            {
            case CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS:
                switch (sub_expression_type)
                {
                case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_ROOT:
                case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_POSITIVE:
                    cushion_instance_execution_error (
                        state->instance, &state->tokenization,
                        "Encountered unexpected \")\" in preprocessor expression evaluation.");
                    break;

                case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_PARENTHESIS:
                    // Might happen to ternary that is enclosed in parentheses,
                    // we'll just pass the parenthesis to upper level.
                case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_NEGATIVE:
                    goto finish_evaluation;
                    break;
                }

                break;

            case CUSHION_PUNCTUATOR_KIND_BITWISE_AND:
                next_operator = LEX_EVALUATE_OPERATOR_BITWISE_AND;
                break;

            case CUSHION_PUNCTUATOR_KIND_BITWISE_OR:
                next_operator = LEX_EVALUATE_OPERATOR_BITWISE_OR;
                break;

            case CUSHION_PUNCTUATOR_KIND_BITWISE_XOR:
                next_operator = LEX_EVALUATE_OPERATOR_BITWISE_XOR;
                break;

            case CUSHION_PUNCTUATOR_KIND_PLUS:
                next_operator = LEX_EVALUATE_OPERATOR_ADD;
                break;

            case CUSHION_PUNCTUATOR_KIND_MINUS:
                next_operator = LEX_EVALUATE_OPERATOR_SUBTRACT;
                break;

            case CUSHION_PUNCTUATOR_KIND_MULTIPLY:
                next_operator = LEX_EVALUATE_OPERATOR_MULTIPLY;
                break;

            case CUSHION_PUNCTUATOR_KIND_DIVIDE:
                next_operator = LEX_EVALUATE_OPERATOR_DIVIDE;
                break;

            case CUSHION_PUNCTUATOR_KIND_MODULO:
                next_operator = LEX_EVALUATE_OPERATOR_MODULO;
                break;

            case CUSHION_PUNCTUATOR_KIND_LOGICAL_AND:
                next_operator = LEX_EVALUATE_OPERATOR_LOGICAL_AND;
                break;

            case CUSHION_PUNCTUATOR_KIND_LOGICAL_OR:
                next_operator = LEX_EVALUATE_OPERATOR_LOGICAL_OR;
                break;

            case CUSHION_PUNCTUATOR_KIND_LOGICAL_LESS:
                next_operator = LEX_EVALUATE_OPERATOR_LESS;
                break;

            case CUSHION_PUNCTUATOR_KIND_LOGICAL_GREATER:
                next_operator = LEX_EVALUATE_OPERATOR_GREATER;
                break;

            case CUSHION_PUNCTUATOR_KIND_LOGICAL_LESS_OR_EQUAL:
                next_operator = LEX_EVALUATE_OPERATOR_LESS_OR_EQUAL;
                break;

            case CUSHION_PUNCTUATOR_KIND_LOGICAL_GREATER_OR_EQUAL:
                next_operator = LEX_EVALUATE_OPERATOR_GREATER_OR_EQUAL;
                break;

            case CUSHION_PUNCTUATOR_KIND_LOGICAL_EQUAL:
                next_operator = LEX_EVALUATE_OPERATOR_EQUAL;
                break;

            case CUSHION_PUNCTUATOR_KIND_LOGICAL_NOT_EQUAL:
                next_operator = LEX_EVALUATE_OPERATOR_NOT_EQUAL;
                break;

            case CUSHION_PUNCTUATOR_KIND_LEFT_SHIFT:
                next_operator = LEX_EVALUATE_OPERATOR_BITWISE_LEFT_SHIFT;
                break;

            case CUSHION_PUNCTUATOR_KIND_RIGHT_SHIFT:
                next_operator = LEX_EVALUATE_OPERATOR_BITWISE_RIGHT_SHIFT;
                break;

            case CUSHION_PUNCTUATOR_KIND_QUESTION_MARK:
                next_operator = LEX_EVALUATE_OPERATOR_TERNARY;
                break;

            case CUSHION_PUNCTUATOR_KIND_COMMA:
                next_operator = LEX_EVALUATE_OPERATOR_COMMA;
                break;

            case CUSHION_PUNCTUATOR_KIND_COLON:
                switch (sub_expression_type)
                {
                case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_ROOT:
                case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_PARENTHESIS:
                case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_NEGATIVE:
                    cushion_instance_execution_error (
                        state->instance, &state->tokenization,
                        "Encountered unexpected \":\" in preprocessor expression evaluation.");
                    break;

                case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_POSITIVE:
                    goto finish_evaluation;
                    break;
                }

                break;

            default:
                cushion_instance_execution_error (
                    state->instance, &state->tokenization,
                    "Expected unexpected punctuator while expecting operator supported in preprocessor expression.");
                break;
            }

            LEX_WHEN_ERROR (break)
            unsigned int operator_precedence = lex_evaluate_operator_precedence (next_operator);
            enum lex_evaluate_operator_associativity_t operator_associativity =
                lex_evaluate_operator_associativity (next_operator);

            if (sub_expression_type == LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_NEGATIVE)
            {
                if (lex_evaluate_is_operation_precedes (LEX_TERNARY_PRECEDENCE, operator_precedence,
                                                        LEX_TERNARY_ASSOCIATIVITY))
                {
                    // Negative ternary has ended, get out of here.
                    goto finish_evaluation;
                }
            }

            // Collapse all operations on stack that must precede the new one.
            while (evaluation_stack_top)
            {
                if (lex_evaluate_is_operation_precedes (evaluation_stack_top->precedence, operator_precedence,
                                                        evaluation_stack_top->associativity))
                {
                    // Current operation on stack precedes new operation, calculate it and get rid of it.
                    // It might look incorrect at first glance, but it is actually working as intended because
                    // we do collapse for every new operator, so operator chains with left-to-right associativity
                    // like modulo of division still work as expected due to frequent collapse.
                    new_argument = lex_evaluate_operation (evaluation_stack_top->left_value,
                                                           evaluation_stack_top->operator, new_argument);

                    struct lex_evaluate_stack_item_t *to_reuse = evaluation_stack_top;
                    evaluation_stack_top = evaluation_stack_top->previous;

                    to_reuse->previous = evaluation_stack_reuse;
                    evaluation_stack_reuse = to_reuse;
                    continue;
                }

                break;
            }

            switch (next_operator)
            {
            case LEX_EVALUATE_OPERATOR_TERNARY:
            {
                // Custom logic for ternary operator.
                long long ternary_positive =
                    lex_preprocessor_evaluate (state, LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_POSITIVE);

                long long ternary_negative =
                    lex_preprocessor_evaluate (state, LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_NEGATIVE);

                new_argument = new_argument ? ternary_positive : ternary_negative;

                // We process finished ternary as a full new argument, so we just expect next operator.
                goto lex_next_operator;
                break;
            }

            default:
            {
                struct lex_evaluate_stack_item_t *new_item;
                if (evaluation_stack_reuse)
                {
                    new_item = evaluation_stack_reuse;
                    evaluation_stack_reuse = evaluation_stack_reuse->previous;
                }
                else
                {
                    new_item = cushion_allocator_allocate (
                        &state->instance->allocator, sizeof (struct lex_evaluate_stack_item_t),
                        _Alignof (struct lexer_token_stack_item_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);
                }

                new_item->left_value = new_argument;
                new_item->operator= next_operator;
                new_item->precedence = operator_precedence;
                new_item->associativity = operator_associativity;

                new_item->previous = evaluation_stack_top;
                evaluation_stack_top = new_item;
                break;
            }
            }

            break;
        }

        case CUSHION_TOKEN_TYPE_NEW_LINE:
            switch (sub_expression_type)
            {
            case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_ROOT:
            case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_NEGATIVE:
                goto finish_evaluation;

            case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_PARENTHESIS:
                cushion_instance_execution_error (
                    state->instance, &state->tokenization,
                    "Expected \")\" but got new line in preprocessor expression evaluation.");

            case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_POSITIVE:
                cushion_instance_execution_error (
                    state->instance, &state->tokenization,
                    "Expected \":\" but got new line in preprocessor expression evaluation.");
                break;
            }

            break;

        case CUSHION_TOKEN_TYPE_END_OF_FILE:
            switch (sub_expression_type)
            {
            case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_ROOT:
            case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_NEGATIVE:
                goto finish_evaluation;

            case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_PARENTHESIS:
                cushion_instance_execution_error (
                    state->instance, &state->tokenization,
                    "Expected \")\" but got end of file in preprocessor expression evaluation.");

            case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_POSITIVE:
                cushion_instance_execution_error (
                    state->instance, &state->tokenization,
                    "Expected \":\" but got end of file in preprocessor expression evaluation.");
                break;
            }

            break;

        finish_evaluation:
        {
            switch (sub_expression_type)
            {
            case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_NEGATIVE:
            {
                // Put the current token back into carousel so upper call can process it too.
                // We can just push it without properly saving it as it will be read right away.

                struct cushion_token_list_item_t push_first_token_item = {
                    .next = NULL,
                    .token = current_token,
                };

                lexer_file_state_push_tokens (state, &push_first_token_item, LEXER_TOKEN_STACK_ITEM_FLAG_NONE);
                break;
            }

            default:
                // No additional logic for this type.
                break;
            }

            // Calculate the stack. It must already be precedence-ordered if algorithm is correct.
            while (evaluation_stack_top)
            {
                // Assert that algorithm is correct just in case.
                assert (!evaluation_stack_top->previous ||
                        !lex_evaluate_is_operation_precedes (evaluation_stack_top->previous->precedence,
                                                             evaluation_stack_top->precedence,
                                                             evaluation_stack_top->previous->associativity));

                new_argument = lex_evaluate_operation (evaluation_stack_top->left_value, evaluation_stack_top->operator,
                                                       new_argument);
                evaluation_stack_top = evaluation_stack_top->previous;
            }

            return new_argument;
        }

        default:
            cushion_instance_execution_error (
                state->instance, &state->tokenization,
                "Expected operator token after argument in preprocessor expression evaluation.");
            break;
        }
    }

    cushion_instance_execution_error (state->instance, &state->tokenization,
                                      "Failed to properly evaluate preprocessor constant expression.");
    return 0;
}

static void lex_update_tokenization_flags (struct cushion_lexer_file_state_t *state)
{
    state->tokenization.flags = CUSHION_TOKENIZATION_FLAGS_NONE;
    if ((state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) ||
        (state->conditional_inclusion_node &&
         state->conditional_inclusion_node->state == CONDITIONAL_INCLUSION_STATE_EXCLUDED))
    {
        state->tokenization.flags |= CUSHION_TOKENIZATION_FLAGS_SKIP_REGULAR;
    }
}

static inline void lex_do_not_skip_regular (struct cushion_lexer_file_state_t *state)
{
    state->tokenization.flags &= ~CUSHION_TOKENIZATION_FLAGS_SKIP_REGULAR;
}

static inline void lexer_conditional_inclusion_node_init_state (struct lexer_conditional_inclusion_node_t *node,
                                                                enum lexer_conditional_inclusion_state_t state)
{
    node->state = state;
    node->flags = CONDITIONAL_INCLUSION_FLAGS_NONE;

    if (state == CONDITIONAL_INCLUSION_STATE_INCLUDED)
    {
        node->flags |= CONDITIONAL_INCLUSION_FLAGS_WAS_INCLUDED;
    }
}

static inline void lexer_conditional_inclusion_node_set_state (struct lexer_conditional_inclusion_node_t *node,
                                                               enum lexer_conditional_inclusion_state_t state)
{
    // Assert that we didn't mess up if-else chain.
    assert (state != CONDITIONAL_INCLUSION_STATE_INCLUDED ||
            (node->flags & CONDITIONAL_INCLUSION_FLAGS_WAS_INCLUDED) == 0u);
    node->state = state;

    if (state == CONDITIONAL_INCLUSION_STATE_INCLUDED)
    {
        node->flags |= CONDITIONAL_INCLUSION_FLAGS_WAS_INCLUDED;
    }
}

static unsigned int lex_preprocessor_if_is_transitively_excluded_conditional (struct cushion_lexer_file_state_t *state)
{
    if (state->conditional_inclusion_node &&
        state->conditional_inclusion_node->state == CONDITIONAL_INCLUSION_STATE_EXCLUDED)
    {
        // Everything inside excluded scope is automatically excluded too.
        struct lexer_conditional_inclusion_node_t *node = cushion_allocator_allocate (
            &state->instance->allocator, sizeof (struct lexer_conditional_inclusion_node_t),
            _Alignof (struct lexer_conditional_inclusion_node_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);

        node->previous = state->conditional_inclusion_node;
        lexer_conditional_inclusion_node_init_state (node, CONDITIONAL_INCLUSION_STATE_EXCLUDED);
        node->line = state->tokenization.cursor_line;
        state->conditional_inclusion_node = node;
        return 1u;
    }

    return 0u;
}

static void lex_preprocessor_if (struct cushion_lexer_file_state_t *state,
                                 const struct cushion_token_t *preprocessor_token)
{
    if (lex_preprocessor_if_is_transitively_excluded_conditional (state))
    {
        return;
    }

    const unsigned int start_line = state->tokenization.cursor_line;
    lex_do_not_skip_regular (state);
    struct cushion_token_t current_token;
    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type == CUSHION_TOKEN_TYPE_IDENTIFIER &&
        current_token.identifier_kind == CUSHION_IDENTIFIER_KIND_CUSHION_PRESERVE)
    {
        // Special case: preserved conditional inclusion. Paste it to the output like normal code.
        struct lexer_conditional_inclusion_node_t *node = cushion_allocator_allocate (
            &state->instance->allocator, sizeof (struct lexer_conditional_inclusion_node_t),
            _Alignof (struct lexer_conditional_inclusion_node_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);

        node->previous = state->conditional_inclusion_node;
        lexer_conditional_inclusion_node_init_state (node, CONDITIONAL_INCLUSION_STATE_PRESERVED);
        node->line = state->tokenization.cursor_line;
        state->conditional_inclusion_node = node;

        if ((state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u && start_line != state->tokenization.cursor_line)
        {
            lex_preprocessor_fixup_line (state);
        }

        lex_preprocessor_preserved_tail (state, preprocessor_token->type, NULL);
        lex_update_tokenization_flags (state);
        return;
    }

    // Safe to do this trick, as it is not deallocated until evaluation is shut down.
    // And token is being processed right when evaluation is started, therefore it is also safe to pass it like that.
    struct cushion_token_list_item_t push_first_token_item = {
        .next = NULL,
        .token = current_token,
    };

    lexer_file_state_push_tokens (state, &push_first_token_item, LEXER_TOKEN_STACK_ITEM_FLAG_NONE);
    const long long evaluation_result = lex_preprocessor_evaluate (state, LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_ROOT);
    LEX_WHEN_ERROR (return)

    struct lexer_conditional_inclusion_node_t *node = cushion_allocator_allocate (
        &state->instance->allocator, sizeof (struct lexer_conditional_inclusion_node_t),
        _Alignof (struct lexer_conditional_inclusion_node_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);

    node->previous = state->conditional_inclusion_node;
    lexer_conditional_inclusion_node_init_state (
        node, evaluation_result ? CONDITIONAL_INCLUSION_STATE_INCLUDED : CONDITIONAL_INCLUSION_STATE_EXCLUDED);
    node->line = start_line;
    state->conditional_inclusion_node = node;

    lex_preprocessor_fixup_line (state);
    lex_update_tokenization_flags (state);
}

static void lex_preprocessor_expect_new_line (struct cushion_lexer_file_state_t *state)
{
    struct cushion_token_t current_token;
    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    switch (current_token.type)
    {
    case CUSHION_TOKEN_TYPE_NEW_LINE:
        break;

    default:
        cushion_instance_execution_error (state->instance, &state->tokenization,
                                          "Expected new line after preprocessor expression.");
        break;
    }
}

static void lex_preprocessor_ifdef (struct cushion_lexer_file_state_t *state, unsigned int reverse)
{
    if (lex_preprocessor_if_is_transitively_excluded_conditional (state))
    {
        return;
    }

    lex_do_not_skip_regular (state);
    unsigned int start_line = state->tokenization.cursor_line;
    struct cushion_token_t current_token;

    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    long long check_result = lex_do_defined_check (state, &current_token);
    LEX_WHEN_ERROR (return)

    lex_preprocessor_expect_new_line (state);
    LEX_WHEN_ERROR (return)

    if (reverse)
    {
        check_result = check_result ? 0u : 1u;
    }

    struct lexer_conditional_inclusion_node_t *node = cushion_allocator_allocate (
        &state->instance->allocator, sizeof (struct lexer_conditional_inclusion_node_t),
        _Alignof (struct lexer_conditional_inclusion_node_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);

    node->previous = state->conditional_inclusion_node;
    lexer_conditional_inclusion_node_init_state (
        node, check_result ? CONDITIONAL_INCLUSION_STATE_INCLUDED : CONDITIONAL_INCLUSION_STATE_EXCLUDED);
    node->line = start_line;
    state->conditional_inclusion_node = node;

    lex_preprocessor_fixup_line (state);
    lex_update_tokenization_flags (state);
}

static unsigned int lex_preprocessor_else_is_transitively_excluded_conditional (
    struct cushion_lexer_file_state_t *state)
{
    if (state->conditional_inclusion_node->flags & CONDITIONAL_INCLUSION_FLAGS_WAS_INCLUDED)
    {
        // Was already included, therefore all else expressions are excluded.
        state->conditional_inclusion_node->state = CONDITIONAL_INCLUSION_STATE_EXCLUDED;
        lex_update_tokenization_flags (state);
        return 1u;
    }

    if (state->conditional_inclusion_node->previous &&
        state->conditional_inclusion_node->previous->state == CONDITIONAL_INCLUSION_STATE_EXCLUDED)
    {
        // Excluded on parent level. Nothing to do here.
        return 1u;
    }

    return 0u;
}

static unsigned int lex_preprocessor_else_is_transitively_preserved (struct cushion_lexer_file_state_t *state,
                                                                     const struct cushion_token_t *preprocessor_token)
{
    if (state->conditional_inclusion_node->state == CONDITIONAL_INCLUSION_STATE_PRESERVED)
    {
        lex_preprocessor_preserved_tail (state, preprocessor_token->type, NULL);
        return 1u;
    }

    return 0u;
}

#define LEX_PREPROCESSOR_ELSE_VALIDATE                                                                                 \
    if (!state->conditional_inclusion_node)                                                                            \
    {                                                                                                                  \
        cushion_instance_execution_error (state->instance, &state->tokenization,                                       \
                                          "Found else family preprocessor without if family preprocessor before it."); \
        return;                                                                                                        \
    }                                                                                                                  \
                                                                                                                       \
    if (state->conditional_inclusion_node->flags & CONDITIONAL_INCLUSION_FLAGS_HAD_PLAIN_ELSE)                         \
    {                                                                                                                  \
        cushion_instance_execution_error (state->instance, &state->tokenization,                                       \
                                          "Found else family preprocessor in chain after unconditional #else.");       \
        return;                                                                                                        \
    }

static void lex_preprocessor_elif (struct cushion_lexer_file_state_t *state,
                                   const struct cushion_token_t *preprocessor_token)
{
    LEX_PREPROCESSOR_ELSE_VALIDATE
    if (lex_preprocessor_else_is_transitively_excluded_conditional (state) ||
        lex_preprocessor_else_is_transitively_preserved (state, preprocessor_token))
    {
        return;
    }

    lex_do_not_skip_regular (state);
    unsigned int start_line = state->tokenization.cursor_line;
    const long long evaluation_result = lex_preprocessor_evaluate (state, LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_ROOT);
    LEX_WHEN_ERROR (return)

    lexer_conditional_inclusion_node_set_state (
        state->conditional_inclusion_node,
        evaluation_result ? CONDITIONAL_INCLUSION_STATE_INCLUDED : CONDITIONAL_INCLUSION_STATE_EXCLUDED);
    state->conditional_inclusion_node->line = start_line;

    lex_preprocessor_fixup_line (state);
    lex_update_tokenization_flags (state);
}

static void lex_preprocessor_elifdef (struct cushion_lexer_file_state_t *state,
                                      const struct cushion_token_t *preprocessor_token,
                                      unsigned int reverse)
{
    LEX_PREPROCESSOR_ELSE_VALIDATE
    if (lex_preprocessor_else_is_transitively_excluded_conditional (state) ||
        lex_preprocessor_else_is_transitively_preserved (state, preprocessor_token))
    {
        return;
    }

    lex_do_not_skip_regular (state);
    const unsigned int start_line = state->tokenization.cursor_line;
    struct cushion_token_t current_token;

    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    long long check_result = lex_do_defined_check (state, &current_token);
    LEX_WHEN_ERROR (return)

    lex_preprocessor_expect_new_line (state);
    LEX_WHEN_ERROR (return)

    if (reverse)
    {
        check_result = check_result ? 0u : 1u;
    }

    lexer_conditional_inclusion_node_set_state (
        state->conditional_inclusion_node,
        check_result ? CONDITIONAL_INCLUSION_STATE_INCLUDED : CONDITIONAL_INCLUSION_STATE_EXCLUDED);
    state->conditional_inclusion_node->line = start_line;
    state->conditional_inclusion_node->flags |= CONDITIONAL_INCLUSION_FLAGS_HAD_PLAIN_ELSE;

    lex_preprocessor_fixup_line (state);
    lex_update_tokenization_flags (state);
}

static void lex_preprocessor_else (struct cushion_lexer_file_state_t *state,
                                   const struct cushion_token_t *preprocessor_token)
{
    LEX_PREPROCESSOR_ELSE_VALIDATE
    if (lex_preprocessor_else_is_transitively_excluded_conditional (state) ||
        lex_preprocessor_else_is_transitively_preserved (state, preprocessor_token))
    {
        return;
    }

    lex_do_not_skip_regular (state);
    unsigned int start_line = state->tokenization.cursor_line;

    lex_preprocessor_expect_new_line (state);
    LEX_WHEN_ERROR (return)

    lexer_conditional_inclusion_node_set_state (state->conditional_inclusion_node,
                                                CONDITIONAL_INCLUSION_STATE_INCLUDED);
    state->conditional_inclusion_node->line = start_line;

    lex_preprocessor_fixup_line (state);
    lex_update_tokenization_flags (state);
}

static void lex_preprocessor_endif (struct cushion_lexer_file_state_t *state,
                                    const struct cushion_token_t *preprocessor_token)
{
    if (!state->conditional_inclusion_node)
    {
        cushion_instance_execution_error (state->instance, &state->tokenization,
                                          "Found #endif without if-else family preprocessor before it.");
        return;
    }

    if (state->conditional_inclusion_node->state == CONDITIONAL_INCLUSION_STATE_PRESERVED)
    {
        lex_preprocessor_preserved_tail (state, preprocessor_token->type, NULL);
        return;
    }

    lex_do_not_skip_regular (state);
    lex_preprocessor_expect_new_line (state);
    LEX_WHEN_ERROR (return)

    state->conditional_inclusion_node = state->conditional_inclusion_node->previous;
    lex_preprocessor_fixup_line (state);
    lex_update_tokenization_flags (state);
}

static unsigned int lex_preprocessor_try_include (struct cushion_lexer_file_state_t *state,
                                                  const struct cushion_token_t *header_token,
                                                  struct cushion_include_node_t *include_node)
{
    if (include_node)
    {
        lexer_file_state_path_init (state, include_node->path);
        LEX_WHEN_ERROR (return 1u)
    }

    lexer_file_state_path_append_sequence (state, header_token->header_path.begin, header_token->header_path.end);
    FILE *input_file = fopen (state->path_buffer.data, "r");

    if (!input_file)
    {
        // File at this path does not exist or is not available.
        return 0u;
    }

    enum cushion_lex_file_flags_t flags = CUSHION_LEX_FILE_FLAG_NONE;
    if (include_node)
    {
        switch (include_node->type)
        {
        case INCLUDE_TYPE_FULL:
            if (state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY)
            {
                cushion_instance_execution_error (
                    state->instance, &state->tokenization,
                    "Include \"%s\" points to full include directory, but it is done from file which is under scan "
                    "only directory. It is considered an error as it makes handling includes much more complex. "
                    "Therefore, including files from full path from files from scan only path is forbidden.",
                    state->path_buffer.data);
                return 1u;
            }

            break;

        case INCLUDE_TYPE_SCAN:
            flags |= CUSHION_LEX_FILE_FLAG_SCAN_ONLY;
            break;
        }
    }

    cushion_lex_file_from_handle (state->instance, input_file, state->path_buffer.data, flags);
    fclose (input_file);
    return 1u;
}

static void lex_preprocessor_include (struct cushion_lexer_file_state_t *state)
{
    lex_do_not_skip_regular (state);
    const unsigned int start_line = state->tokenization.cursor_line;
    struct cushion_token_t current_token;

    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    switch (current_token.type)
    {
    case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
    case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
        break;

    default:
        cushion_instance_execution_error (state->instance, &state->tokenization,
                                          "Expected header path after #include.");
        return;
    }

    unsigned int include_happened = 0u;
    if (current_token.type == CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_USER)
    {
        lexer_file_state_path_init (state, state->file_name);
        LEX_WHEN_ERROR (return)

        // Strip data until last path separator to get directory name from file name.
        {
            const char *cursor = state->path_buffer.data + state->path_buffer.size;
            while (cursor >= state->path_buffer.data)
            {
                if (*cursor == '/' || *cursor == '\\')
                {
                    break;
                }

                --cursor;
            }

            if (cursor >= state->path_buffer.data)
            {
                state->path_buffer.size = cursor - state->path_buffer.data;
                state->path_buffer.data[state->path_buffer.size] = '\0';
            }
            else
            {
                state->path_buffer.size = 1u;
                state->path_buffer.data[0u] = '.';
                state->path_buffer.data[1u] = '\0';
            }
        }

        if (lex_preprocessor_try_include (state, &current_token, NULL))
        {
            include_happened = 1u;
        }
    }

    struct cushion_include_node_t *node = state->instance->includes_first;
    while (node && !include_happened)
    {
        if (lex_preprocessor_try_include (state, &current_token, node))
        {
            include_happened = 1u;
            break;
        }

        node = node->next;
    }

    if (!include_happened && (state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u)
    {
        // Include not found. Preserve it in code.
        cushion_instance_output_null_terminated (state->instance, "#include ");
        cushion_instance_output_sequence (state->instance, current_token.begin, current_token.end);
        cushion_instance_output_null_terminated (state->instance, "\n");
    }

    lex_preprocessor_expect_new_line (state);
    LEX_WHEN_ERROR (return)

    if (include_happened ||
        // If there were multiline comments, we need to fixup line.
        // Expected line is always start_line + 1, because we're read new line too.
        start_line + 1u != state->tokenization.cursor_line)
    {
        lex_preprocessor_fixup_line (state);
    }

    lex_update_tokenization_flags (state);
}

static void lex_preprocessor_define (struct cushion_lexer_file_state_t *state)
{
    const unsigned int start_line = state->tokenization.cursor_line;
    lex_do_not_skip_regular (state);
    struct cushion_token_t current_token;
    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_IDENTIFIER)
    {
        cushion_instance_execution_error (state->instance, &state->tokenization, "Expected identifier after #define.");
        return;
    }

    switch (current_token.identifier_kind)
    {
    case CUSHION_IDENTIFIER_KIND_REGULAR:
        break;

    default:
        cushion_instance_execution_error (state->instance, &state->tokenization,
                                          "Reserved word is used as macro name, which is not supported by Cushion.");
        return;
    }

    struct cushion_macro_node_t *node =
        cushion_allocator_allocate (&state->instance->allocator, sizeof (struct cushion_macro_node_t),
                                    _Alignof (struct cushion_macro_node_t), CUSHION_ALLOCATION_CLASS_PERSISTENT);

    node->name = cushion_instance_copy_char_sequence_inside (state->instance, current_token.begin, current_token.end,
                                                             CUSHION_ALLOCATION_CLASS_PERSISTENT);

    node->flags = CUSHION_MACRO_FLAG_NONE;
    node->replacement_list_first = NULL;
    node->parameters_first = NULL;

    lexer_file_state_pop_token (state, &current_token);
    LEX_WHEN_ERROR (return)

    switch (current_token.type)
    {
    case CUSHION_TOKEN_TYPE_PUNCTUATOR:
    {
        node->flags |= CUSHION_MACRO_FLAG_FUNCTION;
        struct cushion_macro_parameter_node_t *parameters_last = NULL;
        unsigned int lexing_arguments = 1u;

        while (lexing_arguments)
        {
            lex_skip_glue_and_comments (state, &current_token);
            LEX_WHEN_ERROR (return)

            switch (current_token.type)
            {
            case CUSHION_TOKEN_TYPE_IDENTIFIER:
            {
                struct cushion_macro_parameter_node_t *parameter = cushion_allocator_allocate (
                    &state->instance->allocator, sizeof (struct cushion_macro_parameter_node_t),
                    _Alignof (struct cushion_macro_parameter_node_t), CUSHION_ALLOCATION_CLASS_PERSISTENT);

                parameter->name = cushion_instance_copy_char_sequence_inside (
                    state->instance, current_token.begin, current_token.end, CUSHION_ALLOCATION_CLASS_PERSISTENT);
                parameter->next = NULL;

                if (parameters_last)
                {
                    parameters_last->next = parameter;
                }
                else
                {
                    node->parameters_first = parameter;
                }

                parameters_last = parameter;

                lex_skip_glue_and_comments (state, &current_token);
                LEX_WHEN_ERROR (return)

                if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
                    current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_COMMA)
                {
                    lexing_arguments = 0u;
                }

                break;
            }

            default:
                lexing_arguments = 0u;
                break;
            }
        }

        if (current_token.type == CUSHION_TOKEN_TYPE_PUNCTUATOR &&
            current_token.punctuator_kind == CUSHION_PUNCTUATOR_KIND_TRIPLE_DOT)
        {
            node->flags |= CUSHION_MACRO_FLAG_VARIADIC_PARAMETERS;
            lex_skip_glue_and_comments (state, &current_token);
            LEX_WHEN_ERROR (return)
        }

        if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR &&
            current_token.punctuator_kind == CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS)
        {
            cushion_instance_execution_error (state->instance, &state->tokenization,
                                              "Expected \")\" or \",\" while reading macro parameter name list.");
            return;
        }

        break;
    }

    case CUSHION_TOKEN_TYPE_NEW_LINE:
        if ((state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u)
        {
            // Push this new line to preserve line numbers.
            cushion_instance_output_null_terminated (state->instance, "\n");
        }

        // No replacement list, go directly to registration.
        goto register_macro;

    case CUSHION_TOKEN_TYPE_END_OF_FILE:
        // No replacement list, go directly to registration.
        goto register_macro;

    case CUSHION_TOKEN_TYPE_GLUE:
    case CUSHION_TOKEN_TYPE_COMMENT:
        break;

    default:
        cushion_instance_execution_error (
            state->instance, &state->tokenization,
            "Expected whitespaces, comments, \"(\", line end or file end after macro name.");
        return;
    }

    // Lex replacement list.
    enum cushion_lex_replacement_list_result_t lex_result =
        cushion_lex_replacement_list (state->instance, &state->tokenization, &node->replacement_list_first);

    switch (lex_result)
    {
    case CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR:
        // Fixup line after reading the macro.
        lex_preprocessor_fixup_line (state);
        break;

    case CUSHION_LEX_REPLACEMENT_LIST_RESULT_PRESERVED:
        if (start_line != state->tokenization.cursor_line)
        {
            // Fixup line if arguments and other things used line continuation.
            lex_preprocessor_fixup_line (state);
        }

        node->flags |= CUSHION_MACRO_FLAG_PRESERVED;
        lex_preprocessor_preserved_tail (state, CUSHION_TOKEN_TYPE_PREPROCESSOR_DEFINE, node);
        break;
    }

register_macro:
    // Register generated macro.
    cushion_instance_macro_add (state->instance, node, &state->tokenization);
    lex_update_tokenization_flags (state);
}

static void lex_preprocessor_undef (struct cushion_lexer_file_state_t *state)
{
    unsigned int start_line = state->tokenization.cursor_line;
    lex_do_not_skip_regular (state);
    struct cushion_token_t current_token;
    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_IDENTIFIER)
    {
        cushion_instance_execution_error (state->instance, &state->tokenization, "Expected identifier after #undef.");
        return;
    }

    struct cushion_macro_node_t *node =
        cushion_instance_macro_search (state->instance, current_token.begin, current_token.end);
    if (!node || (node->flags & CUSHION_MACRO_FLAG_PRESERVED))
    {
        // Preserve #undef as macro is either unknown or explicitly preserved.
        if ((state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u)
        {
            if (start_line != state->tokenization.cursor_line)
            {
                lex_preprocessor_fixup_line (state);
                start_line = state->tokenization.cursor_line;
            }

            cushion_instance_output_null_terminated (state->instance, "#undef ");
            cushion_instance_output_sequence (state->instance, current_token.begin, current_token.end);
            cushion_instance_output_null_terminated (state->instance, "\n");
        }

        lex_preprocessor_expect_new_line (state);
        LEX_WHEN_ERROR (return)

        if (start_line + 1u != state->tokenization.cursor_line)
        {
            lex_preprocessor_fixup_line (state);
        }

        lex_update_tokenization_flags (state);
        return;
    }

    cushion_instance_macro_remove (state->instance, current_token.begin, current_token.end);
    if ((state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u)
    {
        cushion_instance_output_null_terminated (state->instance, "\n");
    }

    lex_preprocessor_expect_new_line (state);
    if (start_line + 1u != state->tokenization.cursor_line)
    {
        lex_preprocessor_fixup_line (state);
    }

    lex_update_tokenization_flags (state);
}

static void lex_preprocessor_line (struct cushion_lexer_file_state_t *state)
{
    if ((state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u)
    {
        cushion_instance_output_null_terminated (state->instance, "#line ");
    }

    lex_do_not_skip_regular (state);
    struct cushion_token_t current_token;
    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_NUMBER_INTEGER)
    {
        cushion_instance_execution_error (
            state->instance, &state->tokenization,
            "Expected integer line number after #line. Standard allows arbitrary preprocessor-evaluable expressions "
            "for line number calculations, but it is not yet supported in Cushion as it is a rare case.");
        return;
    }

    if ((state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u)
    {
        cushion_instance_output_sequence (state->instance, current_token.begin, current_token.end);
        cushion_instance_output_null_terminated (state->instance, " ");
    }

    const unsigned long long line_number = current_token.unsigned_number_value;
    if (line_number > UINT_MAX)
    {
        cushion_instance_execution_error (state->instance, &state->tokenization,
                                          "Line number %llu is too big and is not supported.", line_number);
        return;
    }

    char *new_file_name = NULL;
    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    switch (current_token.type)
    {
    case CUSHION_TOKEN_TYPE_STRING_LITERAL:
    {
        if ((state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u)
        {
            cushion_instance_output_sequence (state->instance, current_token.begin, current_token.end);
        }

        new_file_name = cushion_allocator_allocate (
            &state->instance->allocator, current_token.symbolic_literal.end - current_token.symbolic_literal.begin + 1u,
            _Alignof (char), CUSHION_ALLOCATION_CLASS_TRANSIENT);

        // Need to resolve escapes.
        const char *cursor = current_token.symbolic_literal.begin;
        char *file_name_output = new_file_name;

        while (cursor < current_token.symbolic_literal.end)
        {
            if (*cursor == '\\')
            {
                if (cursor + 1u >= current_token.symbolic_literal.end)
                {
                    cushion_instance_execution_error (
                        state->instance, &state->tokenization,
                        "Encountered \"\\\" as the last symbol of string literal in #line.");
                    return;
                }

                ++cursor;
                switch (*cursor)
                {
                case '"':
                case '\\':
                    // Allowed to be escaped.
                    break;

                default:
                    cushion_instance_execution_error (
                        state->instance, &state->tokenization,
                        "Encountered unsupported escape \"\\%c\" in #line file name, only "
                        "\"\\\\\" and \"\\\"\" are supported in that context.");
                    return;
                }
            }

            *file_name_output = *cursor;
            ++file_name_output;
            ++cursor;
        }

        *file_name_output = '\0';
        lex_skip_glue_and_comments (state, &current_token);
        LEX_WHEN_ERROR (return)

        switch (current_token.type)
        {
        case CUSHION_TOKEN_TYPE_NEW_LINE:
        case CUSHION_TOKEN_TYPE_END_OF_FILE:
            // File not specified, allowed by standard.
            break;

        default:
            cushion_instance_execution_error (state->instance, &state->tokenization,
                                              "Expected new line or file end after file name in #line directive.");
            return;
        }

        break;
    }

    case CUSHION_TOKEN_TYPE_NEW_LINE:
    case CUSHION_TOKEN_TYPE_END_OF_FILE:
        // File not specified, allowed by standard.
        break;

    default:
        cushion_instance_execution_error (
            state->instance, &state->tokenization,
            "Expected file name literal or new line after line number in #line. Standard allows "
            "arbitrary preprocessor-evaluable expressions for file name determination, but it is "
            "not yet supported in Cushion as it is a rare case.");
        return;
    }

    if ((state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u)
    {
        cushion_instance_output_null_terminated (state->instance, "\n");
    }

    if (new_file_name)
    {
        state->tokenization.file_name = new_file_name;
    }

    state->tokenization.cursor_line = (unsigned int) line_number;
    lex_update_tokenization_flags (state);
}

static void lex_preprocessor_pragma (struct cushion_lexer_file_state_t *state)
{
    lex_do_not_skip_regular (state);
    struct cushion_token_t current_token;
    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type == CUSHION_TOKEN_TYPE_IDENTIFIER &&
        current_token.identifier_kind == CUSHION_IDENTIFIER_KIND_REGULAR &&
        current_token.end - current_token.begin == sizeof ("once") - 1u &&
        strncmp (current_token.begin, "once", sizeof ("once") - 1u) == 0)
    {
        if ((state->flags & CUSHION_LEX_FILE_FLAG_PROCESSED_PRAGMA_ONCE) == 0u)
        {
            state->flags |= CUSHION_LEX_FILE_FLAG_PROCESSED_PRAGMA_ONCE;
            const unsigned int path_hash = cushion_hash_djb2_null_terminated (state->file_name);

            struct cushion_pragma_once_file_node_t *search_node =
                state->instance->pragma_once_buckets[path_hash % CUSHION_PRAGMA_ONCE_BUCKETS];

            while (search_node)
            {
                if (search_node->path_hash == path_hash && strcmp (search_node->path, state->file_name) == 0)
                {
                    break;
                }

                search_node = search_node->next;
            }

            if (search_node)
            {
                // End the lexing normally as file was already processed.
                state->lexing = 0u;
                return;
            }

            struct cushion_pragma_once_file_node_t *new_node = cushion_allocator_allocate (
                &state->instance->allocator, sizeof (struct cushion_pragma_once_file_node_t),
                _Alignof (struct cushion_pragma_once_file_node_t), CUSHION_ALLOCATION_CLASS_PERSISTENT);

            new_node->path_hash = path_hash;
            new_node->path = cushion_instance_copy_null_terminated_inside (state->instance, state->file_name,
                                                                           CUSHION_ALLOCATION_CLASS_PERSISTENT);

            new_node->next = state->instance->pragma_once_buckets[path_hash % CUSHION_PRAGMA_ONCE_BUCKETS];
            state->instance->pragma_once_buckets[path_hash % CUSHION_PRAGMA_ONCE_BUCKETS] = new_node;
        }

        lex_preprocessor_expect_new_line (state);
        LEX_WHEN_ERROR (return)

        lex_preprocessor_fixup_line (state);
        lex_update_tokenization_flags (state);
        return;
    }

    struct cushion_token_list_item_t push_first_token_item = {
        .next = NULL,
        .token = current_token,
    };

    lexer_file_state_push_tokens (state, &push_first_token_item, LEXER_TOKEN_STACK_ITEM_FLAG_NONE);
    lex_preprocessor_preserved_tail (state, CUSHION_TOKEN_TYPE_PREPROCESSOR_PRAGMA, NULL);
    lex_update_tokenization_flags (state);
}

static inline void lex_skip_glue_comments_new_line (struct cushion_lexer_file_state_t *state,
                                                    struct cushion_token_t *current_token)
{
    while (lexer_file_state_should_continue (state))
    {
        lexer_file_state_pop_token (state, current_token);
        LEX_WHEN_ERROR (return)

        switch (current_token->type)
        {
        case CUSHION_TOKEN_TYPE_NEW_LINE:
        case CUSHION_TOKEN_TYPE_GLUE:
        case CUSHION_TOKEN_TYPE_COMMENT:
            break;

        default:
            return;
        }
    }
}

static void lex_code_macro_pragma (struct cushion_lexer_file_state_t *state)
{
    const unsigned int start_line = state->tokenization.cursor_line;
    struct cushion_token_t current_token;
    lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR &&
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS)
    {
        cushion_instance_execution_error (state->instance, &state->tokenization, "Expected \"(\" after _Pragma.");
        return;
    }

    lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_STRING_LITERAL)
    {
        cushion_instance_execution_error (state->instance, &state->tokenization,
                                          "Expected string literal as argument of _Pragma.");
        return;
    }

    if (current_token.symbolic_literal.encoding != CUSHION_TOKEN_SUBSEQUENCE_ENCODING_ORDINARY)
    {
        cushion_instance_execution_error (state->instance, &state->tokenization,
                                          "Only ordinary encoding supported for _Pragma argument.");
        return;
    }

    // Currently, we cannot check whether we're already at new line, so we're adding new line just in case.
    // New line addition (even if it was necessary) breaks line numbering for errors,
    // therefore we need to add line directive before it too.

    cushion_instance_output_null_terminated (state->instance, "\n");
    cushion_instance_output_line_marker (state->instance, start_line, state->tokenization.file_name);
    cushion_instance_output_null_terminated (state->instance, "#pragma ");

    const char *output_begin_cursor = current_token.symbolic_literal.begin;
    const char *cursor = current_token.symbolic_literal.begin;

    while (cursor < current_token.symbolic_literal.end)
    {
        if (*cursor == '\\')
        {
            if (cursor + 1u >= current_token.symbolic_literal.end)
            {
                cushion_instance_execution_error (state->instance, &state->tokenization,
                                                  "Encountered \"\\\" as the last symbol of _Pragma argument literal.");
                return;
            }

            if (output_begin_cursor < cursor)
            {
                cushion_instance_output_sequence (state->instance, output_begin_cursor, cursor);
                output_begin_cursor = cursor;
            }

            ++cursor;
            switch (*cursor)
            {
            case '"':
            case '\\':
                // Allowed to be escaped.
                break;

            default:
                cushion_instance_execution_error (
                    state->instance, &state->tokenization,
                    "Encountered unsupported escape \"\\%c\" in _Pragma argument literal, only "
                    "\"\\\\\" and \"\\\"\" are supported in that context.");
                return;
            }
        }

        ++cursor;
    }

    if (output_begin_cursor < cursor)
    {
        cushion_instance_output_sequence (state->instance, output_begin_cursor, cursor);
    }

    cushion_instance_output_null_terminated (state->instance, "\n");
    lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR &&
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS)
    {
        cushion_instance_execution_error (state->instance, &state->tokenization,
                                          "Expected \")\" after _Pragma argument.");
        return;
    }

    // Always fixup line as pragma output results in new line.
    lex_preprocessor_fixup_line (state);
}

static void lex_code_identifier (struct cushion_lexer_file_state_t *state, struct cushion_token_t *current_token)
{
    // When we're doing scan only pass, we should not lex identifiers like this at all.
    assert (!(state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY));

    switch (current_token->identifier_kind)
    {
    case CUSHION_IDENTIFIER_KIND_FILE:
    {
        const size_t file_name_length = strlen (state->tokenization.file_name);
        char *formatted_file_name = cushion_allocator_allocate (&state->instance->allocator, file_name_length + 3u,
                                                                _Alignof (char), CUSHION_ALLOCATION_CLASS_TRANSIENT);

        formatted_file_name[0u] = '"';
        memcpy (formatted_file_name + 1u, state->tokenization.file_name, file_name_length);
        formatted_file_name[file_name_length - 2u] = '"';
        formatted_file_name[file_name_length - 1u] = '\0';

        struct cushion_token_list_item_t push_file_name_item = {
            .next = NULL,
            .token =
                {
                    .type = CUSHION_TOKEN_TYPE_STRING_LITERAL,
                    .begin = formatted_file_name,
                    .end = formatted_file_name + file_name_length + 2u,
                    .symbolic_literal =
                        {
                            .encoding = CUSHION_TOKEN_SUBSEQUENCE_ENCODING_ORDINARY,
                            .begin = formatted_file_name + 1u,
                            .end = formatted_file_name + file_name_length + 1u,
                        },
                },
        };

        lexer_file_state_push_tokens (state, &push_file_name_item, LEXER_TOKEN_STACK_ITEM_FLAG_NONE);
        return;
    }

    case CUSHION_IDENTIFIER_KIND_LINE:
    {
        // Other parts of code might expect stringized value of literal, so we need to create it, unfortunately.
        unsigned int value = state->tokenization.cursor_line;
        unsigned int string_length = 0u;

        if (value > 0u)
        {
            while (value > 0u)
            {
                ++string_length;
                value /= 10u;
            }
        }
        else
        {
            string_length = 1u;
        }

        char *formatted_literal = cushion_allocator_allocate (&state->instance->allocator, string_length + 1u,
                                                              _Alignof (char), CUSHION_ALLOCATION_CLASS_TRANSIENT);
        formatted_literal[string_length] = '\0';

        if (value > 0u)
        {
            char *cursor = formatted_literal + string_length - 1u;
            while (value > 0u)
            {
                *cursor = (char) ('0' + (value % 10u));
                --cursor;
                value /= 10u;
            }
        }
        else
        {
            formatted_literal[0u] = '0';
        }

        struct cushion_token_list_item_t push_file_line_item = {
            .next = NULL,
            .token =
                {
                    .type = CUSHION_TOKEN_TYPE_NUMBER_INTEGER,
                    .begin = formatted_literal,
                    .end = formatted_literal + string_length,
                    .unsigned_number_value = state->tokenization.cursor_line,
                },
        };

        lexer_file_state_push_tokens (state, &push_file_line_item, LEXER_TOKEN_STACK_ITEM_FLAG_NONE);
        return;
    }

    case CUSHION_IDENTIFIER_KIND_CUSHION_PRESERVE:
        cushion_instance_execution_error (state->instance, &state->tokenization,
                                          "Encountered cushion preserve keyword in unexpected context.");
        return;

    case CUSHION_IDENTIFIER_KIND_CUSHION_WRAPPED:
        cushion_instance_execution_error (state->instance, &state->tokenization,
                                          "Encountered cushion wrapped keyword in unexpected context.");
        return;

    case CUSHION_IDENTIFIER_KIND_MACRO_PRAGMA:
        lex_code_macro_pragma (state);
        return;

    default:
        break;
    }

    struct cushion_token_list_item_t *macro_tokens =
        lex_replace_identifier_if_macro (state, current_token, LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_CODE);

    if (macro_tokens)
    {
        lexer_file_state_push_tokens (state, macro_tokens, LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT);
    }
    else
    {
        // Not a macro, just regular identifier.
        cushion_instance_output_sequence (state->instance, current_token->begin, current_token->end);
    }
}

/// \brief Helper function for deciding whether we need to add separator between left token that is created through
///        macro replacement and arbitrary right token (can be from macro replacement too).
static inline unsigned int lex_is_separator_needed_for_token_pair (enum cushion_token_type_t left,
                                                                   enum cushion_token_type_t right)
{
    switch (right)
    {
    case CUSHION_TOKEN_TYPE_NEW_LINE:
    case CUSHION_TOKEN_TYPE_GLUE:
    case CUSHION_TOKEN_TYPE_COMMENT:
    case CUSHION_TOKEN_TYPE_END_OF_FILE:
        // No need for additional space here.
        return 0u;

    default:
        break;
    }

    switch (left)
    {
    case CUSHION_TOKEN_TYPE_COMMENT:
        // No need for additional space here.
        return 0u;

    default:
        break;
    }

    // Currently we put space in any case that we're not fully sure.
    // Better safe than sorry, even at expense of preprocessed replacement formatting.
    return 1u;
}

void cushion_lex_root_file (struct cushion_instance_t *instance, const char *path, enum cushion_lex_file_flags_t flags)
{
    FILE *input_file = fopen (path, "r");
    if (!input_file)
    {
        fprintf (stderr, "Failed to open input file \"%s\".\n", path);
        cushion_instance_signal_error (instance);
        return;
    }

    cushion_lex_file_from_handle (instance, input_file, path, flags);
    fclose (input_file);
}

void cushion_lex_file_from_handle (struct cushion_instance_t *instance,
                                   FILE *input_file,
                                   const char *path,
                                   enum cushion_lex_file_flags_t flags)
{
    struct cushion_allocator_transient_marker_t allocation_marker =
        cushion_allocator_get_transient_marker (&instance->allocator);

    struct cushion_lexer_file_state_t *state =
        cushion_allocator_allocate (&instance->allocator, sizeof (struct cushion_lexer_file_state_t),
                                    _Alignof (struct cushion_lexer_file_state_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);

    state->lexing = 1u;
    state->flags = flags;
    state->instance = instance;
    state->token_stack_top = NULL;
    state->conditional_inclusion_node = NULL;

    // We need to always convert file path to absolute in order to have proper line directives everywhere.
    if (cushion_convert_path_to_absolute (path, state->file_name) != CUSHION_INTERNAL_RESULT_OK)
    {
        cushion_instance_execution_error (state->instance, &state->tokenization,
                                          "Unable to convert path \"%s\" to absolute path.", path);

        cushion_allocator_reset_transient (&instance->allocator, allocation_marker);
        return;
    }

    cushion_instance_output_depfile_entry (state->instance, state->file_name);
    if ((state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u)
    {
        cushion_instance_output_line_marker (instance, 1u, state->file_name);
    }

    cushion_tokenization_state_init_for_file (&state->tokenization, state->file_name, input_file,
                                              &state->instance->allocator, CUSHION_ALLOCATION_CLASS_TRANSIENT);
    lex_update_tokenization_flags (state);

    struct cushion_token_t current_token;
    current_token.type = CUSHION_TOKEN_TYPE_NEW_LINE; // Just stub value.
    unsigned int previous_token_line = state->tokenization.cursor_line;
    enum lexer_token_stack_item_flags_t current_token_flags = LEXER_TOKEN_STACK_ITEM_FLAG_NONE;

    while (lexer_file_state_should_continue (state))
    {
        const unsigned int previous_is_macro_replacement =
            current_token_flags & LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT;
        enum cushion_token_type_t previous_type = current_token.type;

        current_token_flags = lexer_file_state_pop_token (state, &current_token);
        if (cushion_instance_is_error_signaled (instance))
        {
            break;
        }

        if (!(flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) && previous_is_macro_replacement &&
            lex_is_separator_needed_for_token_pair (previous_type, current_token.type))
        {
            cushion_instance_output_null_terminated (instance, " ");
        }

        switch (current_token.type)
        {
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IF:
            lex_preprocessor_if (state, &current_token);
            break;

        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IFDEF:
            lex_preprocessor_ifdef (state, 0u);
            break;

        case CUSHION_TOKEN_TYPE_PREPROCESSOR_IFNDEF:
            lex_preprocessor_ifdef (state, 1u);
            break;

        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIF:
            lex_preprocessor_elif (state, &current_token);
            break;

        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFDEF:
            lex_preprocessor_elifdef (state, &current_token, 0u);
            break;

        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFNDEF:
            lex_preprocessor_elifdef (state, &current_token, 1u);
            break;

        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELSE:
            lex_preprocessor_else (state, &current_token);
            break;

        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ENDIF:
            lex_preprocessor_endif (state, &current_token);
            break;

        case CUSHION_TOKEN_TYPE_PREPROCESSOR_INCLUDE:
            if (!state->conditional_inclusion_node ||
                state->conditional_inclusion_node->state != CONDITIONAL_INCLUSION_STATE_EXCLUDED)
            {
                lex_preprocessor_include (state);
            }

            break;

        case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
        case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
            cushion_instance_execution_error (instance, &state->tokenization,
                                              "Unexpected header path token (no prior #include).");
            break;

        case CUSHION_TOKEN_TYPE_PREPROCESSOR_DEFINE:
            if (!state->conditional_inclusion_node ||
                state->conditional_inclusion_node->state != CONDITIONAL_INCLUSION_STATE_EXCLUDED)
            {
                lex_preprocessor_define (state);
            }

            break;

        case CUSHION_TOKEN_TYPE_PREPROCESSOR_UNDEF:
            if (!state->conditional_inclusion_node ||
                state->conditional_inclusion_node->state != CONDITIONAL_INCLUSION_STATE_EXCLUDED)
            {
                lex_preprocessor_undef (state);
            }

            break;

        case CUSHION_TOKEN_TYPE_PREPROCESSOR_LINE:
            if (!state->conditional_inclusion_node ||
                state->conditional_inclusion_node->state != CONDITIONAL_INCLUSION_STATE_EXCLUDED)
            {
                lex_preprocessor_line (state);
            }

            break;

        case CUSHION_TOKEN_TYPE_PREPROCESSOR_PRAGMA:
            if (!state->conditional_inclusion_node ||
                state->conditional_inclusion_node->state != CONDITIONAL_INCLUSION_STATE_EXCLUDED)
            {
                lex_preprocessor_pragma (state);
            }

            break;

        case CUSHION_TOKEN_TYPE_IDENTIFIER:
            lex_code_identifier (state, &current_token);
            break;

        case CUSHION_TOKEN_TYPE_PUNCTUATOR:
        case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:
        case CUSHION_TOKEN_TYPE_NUMBER_FLOATING:
        case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
        case CUSHION_TOKEN_TYPE_STRING_LITERAL:
        case CUSHION_TOKEN_TYPE_NEW_LINE:
        case CUSHION_TOKEN_TYPE_GLUE:
        case CUSHION_TOKEN_TYPE_OTHER:
            cushion_instance_output_sequence (instance, current_token.begin, current_token.end);
            break;

        case CUSHION_TOKEN_TYPE_COMMENT:
            // If it was a multiline comment, fixup line number.
            if (previous_token_line != state->tokenization.cursor_line)
            {
                cushion_instance_output_null_terminated (state->instance, "\n");
                lex_preprocessor_fixup_line (state);
            }
            else
            {
                // Just a space to make sure that tokens are not merged by mistake.
                cushion_instance_output_null_terminated (state->instance, " ");
            }

            break;

        case CUSHION_TOKEN_TYPE_END_OF_FILE:
            if (state->conditional_inclusion_node)
            {
                cushion_instance_execution_error (
                    instance, &state->tokenization,
                    "Encountered end of file, but conditional inclusion started line %u at is not closed.",
                    state->conditional_inclusion_node->line);
            }

            break;
        }

        previous_token_line = state->tokenization.cursor_line;
    }

    // Currently there is no safe way to reset transient data except for the end of file lexing.
    // For the most cases, we would never use more than 1 or 2 pages for file (with 1mb pages),
    // so there is usually no need for aggressive memory reuse.
    cushion_allocator_reset_transient (&instance->allocator, allocation_marker);
}
