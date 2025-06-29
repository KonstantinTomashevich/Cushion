#define _CRT_SECURE_NO_WARNINGS

#include "internal.h"

enum lexer_token_stack_item_flags_t
{
    LEXER_TOKEN_STACK_ITEM_FLAG_NONE = 0u,
    LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT = 1u << 0u,
};

struct lexer_token_stack_item_t
{
    struct lexer_token_stack_item_t *previous;
    struct cushion_token_list_item_t *tokens_current;
    enum lexer_token_stack_item_flags_t flags;

#if defined(CUSHION_EXTENSIONS)
    enum cushion_token_list_item_flags_t last_popped_flags;
#endif
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

    // Add stack exit file name and line.
    if (!state->token_stack_top)
    {
        state->stack_exit_file = state->tokenization.file_name;
        state->stack_exit_line = state->tokenization.cursor_line;
    }

    struct lexer_token_stack_item_t *item =
        cushion_allocator_allocate (&state->instance->allocator, sizeof (struct lexer_token_stack_item_t),
                                    _Alignof (struct lexer_token_stack_item_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);

    item->previous = state->token_stack_top;
    item->tokens_current = tokens;
    item->flags = flags;
#if defined(CUSHION_EXTENSIONS)
    item->last_popped_flags = CUSHION_TOKEN_LIST_ITEM_FLAG_NONE;
#endif
    state->token_stack_top = item;
}

static inline void lexer_file_state_reinsert_token (struct cushion_lexer_file_state_t *state,
                                                    const struct cushion_token_t *token)
{
    struct cushion_token_list_item_t *new_token =
        cushion_allocator_allocate (&state->instance->allocator, sizeof (struct cushion_token_list_item_t),
                                    _Alignof (struct cushion_token_list_item_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);

    new_token->next = NULL;
    new_token->token = *token;

    new_token->file = state->tokenization.file_name;
    new_token->line = state->last_token_line;

#if defined(CUSHION_EXTENSIONS)
    new_token->flags =
        state->token_stack_top ? state->token_stack_top->last_popped_flags : CUSHION_TOKEN_LIST_ITEM_FLAG_NONE;
#endif

    lexer_file_state_push_tokens (
        state, new_token, state->token_stack_top ? state->token_stack_top->flags : LEXER_TOKEN_STACK_ITEM_FLAG_NONE);
}

struct lexer_pop_token_meta_t
{
    enum lexer_token_stack_item_flags_t flags;
    const char *file;
    unsigned int line;
};

static inline struct cushion_error_context_t lex_error_context (struct cushion_lexer_file_state_t *state,
                                                                const struct lexer_pop_token_meta_t *meta)
{
    struct cushion_error_context_t context = {
        .file = meta->file,
        .line = meta->line,
        .column = UINT_MAX,
    };

    if (state->last_token_line == meta->line &&
        (state->last_marked_file == meta->file || strcmp (state->file_name, meta->file) == 0))
    {
        context.column = state->tokenization.cursor_column;
    }

    return context;
}

static inline struct cushion_error_context_t tokenization_error_context (
    struct cushion_tokenization_state_t *tokenization_state)
{
    return (struct cushion_error_context_t) {
        .file = tokenization_state->file_name,
        .line = tokenization_state->cursor_line,
        .column = tokenization_state->cursor_column,
    };
}

static inline void cushion_instance_lexer_error (struct cushion_lexer_file_state_t *state,
                                                 const struct lexer_pop_token_meta_t *meta,
                                                 const char *format,
                                                 ...)
{
    va_list variadic_arguments;
    va_start (variadic_arguments, format);
    cushion_instance_execution_error_internal (state->instance, lex_error_context (state, meta), format,
                                               variadic_arguments);
    va_end (variadic_arguments);
}

static struct lexer_pop_token_meta_t lexer_file_state_pop_token (struct cushion_lexer_file_state_t *state,
                                                                 struct cushion_token_t *output)
{
    struct lexer_pop_token_meta_t meta = {
        .flags = LEXER_TOKEN_STACK_ITEM_FLAG_NONE,
        .file = state->tokenization.file_name,
        .line = state->tokenization.cursor_line,
    };

    while (state->token_stack_top)
    {
        if (state->token_stack_top->tokens_current)
        {
            *output = state->token_stack_top->tokens_current->token;
            meta.flags = state->token_stack_top->flags;
            meta.file = state->token_stack_top->tokens_current->file;
            meta.line = state->token_stack_top->tokens_current->line;

#if defined(CUSHION_EXTENSIONS)
            if (state->token_stack_top->tokens_current->flags & CUSHION_TOKEN_LIST_ITEM_FLAG_WRAPPED_BLOCK)
            {
                // Manually disable macro replacement logic for wrapped blocks.
                meta.flags &= ~LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT;
            }

            state->token_stack_top->last_popped_flags = state->token_stack_top->tokens_current->flags;
#endif

            state->token_stack_top->tokens_current = state->token_stack_top->tokens_current->next;
            goto read_token;
        }
        else
        {
            state->token_stack_top = state->token_stack_top->previous;
            if (!state->token_stack_top)
            {
                meta.file = state->stack_exit_file;
                meta.line = state->stack_exit_line;
            }
        }
    }

    // No more ready-to-use tokens from replacement lists or other sources, request next token from tokenizer.
    cushion_tokenization_next_token (state->instance, &state->tokenization, output);

read_token:
    if (output->type == CUSHION_TOKEN_TYPE_END_OF_FILE)
    {
        // Process lexing stop on end of file internally for ease of use.
        state->lexing = 0u;
    }

    state->last_token_line = meta.line;
    return meta;
}

static inline void lexer_file_state_path_init (struct cushion_lexer_file_state_t *state, const char *data)
{
    if (!data)
    {
        state->path_buffer.data[0u] = '\0';
        state->path_buffer.size = 0u;
        return;
    }

    const size_t in_size = strlen (data);
    if (in_size + 1u > CUSHION_PATH_BUFFER_SIZE)
    {
        cushion_instance_execution_error (state->instance, tokenization_error_context (&state->tokenization),
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
                state->instance, tokenization_error_context (&state->tokenization),
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
            state->instance, tokenization_error_context (&state->tokenization),
            "Failed to append sequence to path container, resulting path is too long. Source path: %s",
            state->path_buffer.data);
    }

    memcpy (state->path_buffer.data + state->path_buffer.size, sequence_begin, in_size);
    state->path_buffer.size += in_size;
    state->path_buffer.data[state->path_buffer.size] = '\0';
}

/// \details We need to be able to properly parse replacement lists from both command line arguments that has only
///          tokenization state and from code (has lexer state and it is possible to use directives in
///          __CUSHION_WRAPPED__ code). Therefore, we need context and two functions.
struct cushion_lex_replacement_list_context_t
{
    struct cushion_instance_t *instance;
    struct cushion_error_context_t error_context;
    enum cushion_macro_flags_t *flags_output;

    struct cushion_token_list_item_t *first;
    struct cushion_token_list_item_t *last;

    struct cushion_token_t current_token;
    unsigned int lexing;
};

static enum cushion_lex_replacement_list_result_t cushion_lex_replacement_list_step (
    struct cushion_lex_replacement_list_context_t *context)
{
#define SAVE_AND_APPEND_TOKEN_TO_LIST                                                                                  \
    {                                                                                                                  \
        struct cushion_token_list_item_t *new_token_item = cushion_save_token_to_memory (                              \
            context->instance, &context->current_token, CUSHION_ALLOCATION_CLASS_PERSISTENT);                          \
                                                                                                                       \
        /* We do not save file and line info in replacement lists as this is not how macros usually work. */           \
        if (context->last)                                                                                             \
        {                                                                                                              \
            context->last->next = new_token_item;                                                                      \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            context->first = new_token_item;                                                                           \
        }                                                                                                              \
                                                                                                                       \
        context->last = new_token_item;                                                                                \
    }

    switch (context->current_token.type)
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
            context->instance, context->error_context,
            "Encountered preprocessor directive while lexing replacement list. Shouldn't be "
            "possible at all, can be an internal error.");
        return CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR;

    case CUSHION_TOKEN_TYPE_IDENTIFIER:
        switch (context->current_token.identifier_kind)
        {
        case CUSHION_IDENTIFIER_KIND_CUSHION_PRESERVE:
            if (context->first)
            {
                cushion_instance_execution_error (
                    context->instance, context->error_context,
                    "Encountered __CUSHION_PRESERVE__ while lexing replacement list and it is not first "
                    "replacement-list-significant token. When using __CUSHION_PRESERVE__ to avoid unwrapping "
                    "macro, it must always be the first thing in the replacement list.");
                return CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR;
            }

            return CUSHION_LEX_REPLACEMENT_LIST_RESULT_PRESERVED;

#if defined(CUSHION_EXTENSIONS)
        case CUSHION_IDENTIFIER_KIND_CUSHION_WRAPPED:
            if (cushion_instance_has_feature (context->instance, CUSHION_FEATURE_WRAPPER_MACRO))
            {
                *context->flags_output |= CUSHION_MACRO_FLAG_WRAPPED;
            }
            else
            {
                cushion_instance_execution_error (context->instance, context->error_context,
                                                  "Encountered __CUSHION_WRAPPED__, but this feature is not "
                                                  "enabled in current execution context.");
                return CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR;
            }

            break;

        case CUSHION_IDENTIFIER_KIND_CUSHION_EVALUATED_ARGUMENT:
            if (!cushion_instance_has_feature (context->instance, CUSHION_FEATURE_EVALUATED_ARGUMENT))
            {
                cushion_instance_execution_error (context->instance, context->error_context,
                                                  "Encountered __CUSHION_EVALUATED_ARGUMENT__, but this feature is not "
                                                  "enabled in current execution context.");
                return CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR;
            }

            break;

        case CUSHION_IDENTIFIER_KIND_CUSHION_REPLACEMENT_INDEX:
            if (!cushion_instance_has_feature (context->instance, CUSHION_FEATURE_REPLACEMENT_INDEX))
            {
                cushion_instance_execution_error (context->instance, context->error_context,
                                                  "Encountered __CUSHION_REPLACEMENT_INDEX__, but this feature is not "
                                                  "enabled in current execution context.");
                return CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR;
            }

            break;
#endif

        default:
            break;
        }

        SAVE_AND_APPEND_TOKEN_TO_LIST
        break;

    case CUSHION_TOKEN_TYPE_PUNCTUATOR:
    case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:
    case CUSHION_TOKEN_TYPE_NUMBER_FLOATING:
    case CUSHION_TOKEN_TYPE_DIGIT_IDENTIFIER_SEQUENCE:
    case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
    case CUSHION_TOKEN_TYPE_STRING_LITERAL:
    case CUSHION_TOKEN_TYPE_OTHER:
        SAVE_AND_APPEND_TOKEN_TO_LIST
        break;

    case CUSHION_TOKEN_TYPE_NEW_LINE:
    case CUSHION_TOKEN_TYPE_END_OF_FILE:
        context->lexing = 0u;
        break;

    case CUSHION_TOKEN_TYPE_GLUE:
    case CUSHION_TOKEN_TYPE_COMMENT:
        // Glue and comments are ignored in replacement lists as replacements newer preserve formatting.
        break;
    }

#undef SAVE_AND_APPEND_TOKEN_TO_LIST
    return CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR;
}

enum cushion_lex_replacement_list_result_t cushion_lex_replacement_list_from_tokenization (
    struct cushion_instance_t *instance,
    struct cushion_tokenization_state_t *tokenization_state,
    struct cushion_token_list_item_t **token_list_output,
    enum cushion_macro_flags_t *flags_output)
{
    struct cushion_lex_replacement_list_context_t context = {
        .instance = instance,
        .flags_output = flags_output,
        .first = NULL,
        .last = NULL,
        .lexing = 1u,
    };

    *token_list_output = NULL;
    while (context.lexing)
    {
        cushion_tokenization_next_token (instance, tokenization_state, &context.current_token);
        context.error_context = tokenization_error_context (tokenization_state);

        if (cushion_instance_is_error_signaled (instance))
        {
            return CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR;
        }

        switch (cushion_lex_replacement_list_step (&context))
        {
        case CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR:
            if (cushion_instance_is_error_signaled (instance))
            {
                return CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR;
            }

            break;

        case CUSHION_LEX_REPLACEMENT_LIST_RESULT_PRESERVED:
            return CUSHION_LEX_REPLACEMENT_LIST_RESULT_PRESERVED;
        }
    }

    *token_list_output = context.first;
    return CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR;
}

enum cushion_lex_replacement_list_result_t cushion_lex_replacement_list_from_lexer (
    struct cushion_instance_t *instance,
    struct cushion_lexer_file_state_t *lexer_state,
    struct cushion_token_list_item_t **token_list_output,
    enum cushion_macro_flags_t *flags_output)
{
    struct cushion_lex_replacement_list_context_t context = {
        .instance = instance,
        .flags_output = flags_output,
        .first = NULL,
        .last = NULL,
        .lexing = 1u,
    };

    *token_list_output = NULL;
    while (context.lexing)
    {
        struct lexer_pop_token_meta_t meta = lexer_file_state_pop_token (lexer_state, &context.current_token);
        context.error_context = lex_error_context (lexer_state, &meta);

        if (cushion_instance_is_error_signaled (instance))
        {
            return CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR;
        }

        switch (cushion_lex_replacement_list_step (&context))
        {
        case CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR:
            if (cushion_instance_is_error_signaled (instance))
            {
                return CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR;
            }

            break;

        case CUSHION_LEX_REPLACEMENT_LIST_RESULT_PRESERVED:
            return CUSHION_LEX_REPLACEMENT_LIST_RESULT_PRESERVED;
        }
    }

    *token_list_output = context.first;
    return CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR;
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
        case CUSHION_TOKEN_TYPE_DIGIT_IDENTIFIER_SEQUENCE:
        case CUSHION_TOKEN_TYPE_OTHER:
            size += (unsigned int) (token->token.end - token->token.begin);
            break;

        case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
        case CUSHION_TOKEN_TYPE_STRING_LITERAL:
        {
            size += (unsigned int) (token->token.end - token->token.begin);
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
        case CUSHION_TOKEN_TYPE_DIGIT_IDENTIFIER_SEQUENCE:
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
    CHECK_KIND ("CUSHION_STATEMENT_ACCUMULATOR_REF", CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_REF)
    CHECK_KIND ("CUSHION_STATEMENT_ACCUMULATOR_UNREF", CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_UNREF)
    CHECK_KIND ("CUSHION_SNIPPET", CUSHION_IDENTIFIER_KIND_CUSHION_SNIPPET)
    CHECK_KIND ("__CUSHION_EVALUATED_ARGUMENT__", CUSHION_IDENTIFIER_KIND_CUSHION_EVALUATED_ARGUMENT)
    CHECK_KIND ("__CUSHION_REPLACEMENT_INDEX__", CUSHION_IDENTIFIER_KIND_CUSHION_REPLACEMENT_INDEX)

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
    CHECK_KIND ("switch", CUSHION_IDENTIFIER_KIND_SWITCH)

    CHECK_KIND ("return", CUSHION_IDENTIFIER_KIND_RETURN)
    CHECK_KIND ("break", CUSHION_IDENTIFIER_KIND_BREAK)
    CHECK_KIND ("continue", CUSHION_IDENTIFIER_KIND_CONTINUE)
    CHECK_KIND ("goto", CUSHION_IDENTIFIER_KIND_GOTO)

    CHECK_KIND ("default", CUSHION_IDENTIFIER_KIND_DEFAULT)
#undef CHECK_KIND

    return CUSHION_IDENTIFIER_KIND_REGULAR;
}

#define LEX_WHEN_ERROR(...)                                                                                            \
    if (cushion_instance_is_error_signaled (state->instance))                                                          \
    {                                                                                                                  \
        __VA_ARGS__;                                                                                                   \
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
    struct cushion_token_list_item_t *wrapped_tokens;
    struct cushion_token_list_item_t *current_token;
    unsigned int replacement_line;
    struct macro_replacement_token_list_t result;
    struct macro_replacement_token_list_t sub_list;
#if defined(CUSHION_EXTENSIONS)
    unsigned int replacement_index;
#endif
};

static inline struct cushion_error_context_t macro_replacement_error_context (
    struct cushion_lexer_file_state_t *state, struct macro_replacement_context_t *context)
{
    return (struct cushion_error_context_t) {
        .file = state->tokenization.file_name,
        .line = context->replacement_line,
        .column = UINT_MAX,
    };
}

static inline struct cushion_token_list_item_t *macro_replacement_token_list_append (
    struct cushion_lexer_file_state_t *state,
    struct macro_replacement_token_list_t *list,
    struct cushion_token_t *token,
    const char *file,
    unsigned int line)
{
    struct cushion_token_list_item_t *new_token =
        cushion_allocator_allocate (&state->instance->allocator, sizeof (struct cushion_token_list_item_t),
                                    _Alignof (struct cushion_token_list_item_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);

    // We just copy token value as its string allocations should be already dealt with either through persistent
    // allocation or manual copy.
    new_token->token = *token;
    new_token->next = NULL;

    new_token->file = file;
    new_token->line = line;

#if defined(CUSHION_EXTENSIONS)
    new_token->flags = CUSHION_TOKEN_LIST_ITEM_FLAG_NONE;
#endif

    if (list->last)
    {
        list->last->next = new_token;
    }
    else
    {
        list->first = new_token;
    }

    list->last = new_token;
    return new_token;
}

struct lex_macro_argument_read_context_t
{
    struct cushion_macro_node_t *macro;
    struct lex_macro_argument_t *arguments_first;
    struct lex_macro_argument_t *arguments_last;

    struct cushion_token_t current_token;
    struct lexer_pop_token_meta_t current_token_meta;

    struct cushion_token_list_item_t *argument_tokens_first;
    struct cushion_token_list_item_t *argument_tokens_last;

    unsigned int argument_token_line;
    unsigned int parenthesis_counter;
    struct cushion_macro_parameter_node_t *parameter;
};

static inline struct lex_macro_argument_read_context_t lex_macro_argument_read_context_init (
    struct cushion_macro_node_t *macro, unsigned int argument_token_line)
{
    struct lex_macro_argument_read_context_t context;
    context.macro = macro;
    context.arguments_first = NULL;
    context.arguments_last = NULL;

    // Initialize current token with default values so warnings checks do not trigger on it.
    context.current_token.type = CUSHION_TOKEN_TYPE_NEW_LINE;
    context.current_token.begin = "\n";
    context.current_token.end = context.current_token.begin + 1u;

    context.argument_tokens_first = NULL;
    context.argument_tokens_last = NULL;

    context.argument_token_line = argument_token_line;
    context.parenthesis_counter = 1u;
    context.parameter = macro->parameters_first;
    return context;
}

static void lex_macro_argument_read_step (struct cushion_lexer_file_state_t *state,
                                          struct lex_macro_argument_read_context_t *context)
{
    const unsigned int parameterless_function_line =
        !context->parameter && (context->macro->flags & CUSHION_MACRO_FLAG_VARIADIC_PARAMETERS) == 0u;

    switch (context->current_token.type)
    {
    case CUSHION_TOKEN_TYPE_PUNCTUATOR:
        switch (context->current_token.punctuator_kind)
        {
        case CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS:
            ++context->parenthesis_counter;
            goto append_argument_token;
            break;

        case CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS:
            --context->parenthesis_counter;

            if (context->parenthesis_counter == 0u && !parameterless_function_line)
            {
                goto finalize_argument;
            }

            goto append_argument_token;
            break;

        case CUSHION_PUNCTUATOR_KIND_COMMA:
            if (context->parenthesis_counter > 1u)
            {
                goto append_argument_token;
            }
            else
            {
                goto finalize_argument;
            }

            break;

        finalize_argument:
        {
            if (parameterless_function_line)
            {
                cushion_instance_lexer_error (state, &context->current_token_meta,
                                              "Encountered more parameters for function-line macro than expected.");
                break;
            }

            struct lex_macro_argument_t *new_argument =
                cushion_allocator_allocate (&state->instance->allocator, sizeof (struct lex_macro_argument_t),
                                            _Alignof (struct lex_macro_argument_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);

            new_argument->next = NULL;
            new_argument->tokens_first = context->argument_tokens_first;
            context->argument_tokens_first = NULL;
            context->argument_tokens_last = NULL;

            if (context->arguments_last)
            {
                context->arguments_last->next = new_argument;
            }
            else
            {
                context->arguments_first = new_argument;
            }

            context->arguments_last = new_argument;
            assert (context->parameter || (context->macro->flags & CUSHION_MACRO_FLAG_VARIADIC_PARAMETERS));

            if (context->parameter)
            {
                context->parameter = context->parameter->next;
            }

            break;
        }

        default:
            goto append_argument_token;
            break;
        }

        break;

    case CUSHION_TOKEN_TYPE_NEW_LINE:
    case CUSHION_TOKEN_TYPE_GLUE:
    case CUSHION_TOKEN_TYPE_COMMENT:
        // Glue and comments are not usually kept in macro arguments.
        // New line should either be skipped or be checked by function user and cause appropriate error
        // depending on the context.
        break;

    case CUSHION_TOKEN_TYPE_END_OF_FILE:
        cushion_instance_lexer_error (state, &context->current_token_meta,
                                      "Got to the end of file while parsing arguments of function-like macro.");
        break;

    default:
    append_argument_token:
    {
        if (!context->parameter && (context->macro->flags & CUSHION_MACRO_FLAG_VARIADIC_PARAMETERS) == 0u)
        {
            cushion_instance_lexer_error (state, &context->current_token_meta,
                                          "Encountered more parameters for function-line macro than expected.");
            break;
        }

        struct cushion_token_list_item_t *argument_tokens_new_token =
            cushion_save_token_to_memory (state->instance, &context->current_token, CUSHION_ALLOCATION_CLASS_TRANSIENT);

        argument_tokens_new_token->file = state->tokenization.file_name;
        argument_tokens_new_token->line = context->argument_token_line;

        if (context->argument_tokens_last)
        {
            context->argument_tokens_last->next = argument_tokens_new_token;
        }
        else
        {
            context->argument_tokens_first = argument_tokens_new_token;
        }

        context->argument_tokens_last = argument_tokens_new_token;
        break;
    }
    }
}

static struct cushion_token_list_item_t *lex_do_macro_replacement (
    struct cushion_lexer_file_state_t *state,
    struct cushion_macro_node_t *macro,
    struct lex_macro_argument_t *arguments,
    struct cushion_token_list_item_t *wrapped_tokens,
    // Replacement line is used to properly set file name and line marker information for replacement tokens.
    unsigned int replacement_line);

#if defined(CUSHION_EXTENSIONS)
static inline void macro_replacement_context_evaluate_sub_list (struct cushion_lexer_file_state_t *state,
                                                                struct macro_replacement_context_t *context)
{
    struct cushion_token_list_item_t *cursor = context->sub_list.first;
    struct cushion_token_list_item_t *previous = NULL;

    while (cursor)
    {
        if (cursor->token.type == CUSHION_TOKEN_TYPE_IDENTIFIER)
        {
            struct cushion_macro_node_t *macro =
                cushion_instance_macro_search (state->instance, cursor->token.begin, cursor->token.end);

            if (!macro || (macro->flags & CUSHION_MACRO_FLAG_PRESERVED))
            {
                goto not_evaluated;
            }

            struct lex_macro_argument_t *arguments = NULL;
            const unsigned int replacement_line = cursor->line;

            if (macro->flags & CUSHION_MACRO_FLAG_FUNCTION)
            {
                struct lex_macro_argument_read_context_t argument_context =
                    lex_macro_argument_read_context_init (macro, cursor->line);

                // Scan for opening parenthesis.
                cursor = cursor->next;

                if (!cursor || cursor->token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
                    cursor->token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS)
                {
                    cushion_instance_execution_error (state->instance, macro_replacement_error_context (state, context),
                                                      "Expected \"(\" after function-like macro name while doing "
                                                      "argument evaluation inside macro replacement.");
                    return;
                }

                while (argument_context.parenthesis_counter > 0u &&
                       !cushion_instance_is_error_signaled (state->instance))
                {
                    cursor = cursor->next;
                    if (!cursor)
                    {
                        cushion_instance_execution_error (state->instance,
                                                          macro_replacement_error_context (state, context),
                                                          "Encountered end of argument evaluation sub list while "
                                                          "expecting \")\" inside function-like macro call.");
                        return;
                    }

                    argument_context.current_token = cursor->token;
                    argument_context.current_token_meta.file = cursor->file;
                    argument_context.current_token_meta.line = cursor->line;
                    argument_context.current_token_meta.flags = LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT;

                    lex_macro_argument_read_step (state, &argument_context);
                    LEX_WHEN_ERROR (return)
                }

                if (argument_context.parameter)
                {
                    cushion_instance_lexer_error (state, &argument_context.current_token_meta,
                                                  "Encountered less parameters for function-line macro %s than "
                                                  "expected while argument evaluation.",
                                                  macro->name);
                    return;
                }

                arguments = argument_context.arguments_first;
            }

            if (macro->flags & CUSHION_MACRO_FLAG_WRAPPED)
            {
                cushion_instance_execution_error (
                    state->instance, macro_replacement_error_context (state, context),
                    "Encountered wrapped macro %s during argument evaluation. Wrapper macros "
                    "are not supported in this context and are not really a good fit here.",
                    macro->name);
                return;
            }

            struct cushion_token_list_item_t *replacement_tokens =
                lex_do_macro_replacement (state, macro, arguments, NULL, replacement_line);

            if (replacement_tokens)
            {
                if (previous)
                {
                    previous->next = replacement_tokens;
                }
                else
                {
                    context->sub_list.first = replacement_tokens;
                }

                struct cushion_token_list_item_t *last = replacement_tokens;
                while (last->next)
                {
                    last = last->next;
                }

                last->next = cursor->next;
                cursor = replacement_tokens;
            }
            else
            {
                cursor = cursor->next;
                if (previous)
                {
                    previous->next = cursor;
                }
                else
                {
                    context->sub_list.first = cursor;
                }
            }

            // Evaluation of this identifier is done.
            continue;
        }

    not_evaluated:
        previous = cursor;
        cursor = cursor->next;
    }

    context->sub_list.last = previous;
}
#endif

/// \return 0 if regular identifier that was not replaced and just injected into sublist, 1 if replacement happened.
static inline unsigned int macro_replacement_context_process_identifier_into_sub_list (
    struct cushion_lexer_file_state_t *state, struct macro_replacement_context_t *context)
{
    context->sub_list.first = NULL;
    context->sub_list.last = NULL;

    switch (context->current_token->token.identifier_kind)
    {
    case CUSHION_IDENTIFIER_KIND_VA_ARGS:
    case CUSHION_IDENTIFIER_KIND_VA_OPT:
    {
        if ((context->macro->flags & CUSHION_MACRO_FLAG_VARIADIC_PARAMETERS) == 0u)
        {
            cushion_instance_execution_error (state->instance, macro_replacement_error_context (state, context),
                                              "Caught attempt to use __VA_ARGS__/__VA_OPT__ in non-variadic macro.");
            return 1u;
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
                    macro_replacement_token_list_append (state, &context->sub_list, &argument_token->token,
                                                         state->tokenization.file_name, context->replacement_line);
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

                    macro_replacement_token_list_append (state, &context->sub_list, &token_comma,
                                                         state->tokenization.file_name, context->replacement_line);
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
                cushion_instance_execution_error (state->instance, macro_replacement_error_context (state, context),
                                                  "Expected \"(\" after __VA_OPT__ in replacement list.");
                return 1u;
            }

            // Scan internals of the __VA_OPT__ macro call.
            unsigned int internal_parenthesis = 0u;
            context->current_token = context->current_token->next;

            while (1u)
            {
                if (!context->current_token)
                {
                    cushion_instance_execution_error (state->instance, macro_replacement_error_context (state, context),
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
                    macro_replacement_token_list_append (state, &context->sub_list, &context->current_token->token,
                                                         state->tokenization.file_name, context->replacement_line);
                }

                context->current_token = context->current_token->next;
            }
        }

        return 1u;
    }

#if defined(CUSHION_EXTENSIONS)
    case CUSHION_IDENTIFIER_KIND_CUSHION_WRAPPED:
    {
        struct cushion_token_list_item_t *wrapped_token = context->wrapped_tokens;
        while (wrapped_token)
        {
            struct cushion_token_list_item_t *added = macro_replacement_token_list_append (
                state, &context->sub_list, &wrapped_token->token, wrapped_token->file, wrapped_token->line);
            added->flags = wrapped_token->flags;
            wrapped_token = wrapped_token->next;
        }

        return 1u;
    }

    case CUSHION_IDENTIFIER_KIND_CUSHION_EVALUATED_ARGUMENT:
    {
        // Scan for the opening parenthesis.
        context->current_token = context->current_token->next;
        // Preprocessor, new line, glue, comment and end of file should never appear here anyway.

        if (!context->current_token || context->current_token->token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
            context->current_token->token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS)
        {
            cushion_instance_execution_error (
                state->instance, macro_replacement_error_context (state, context),
                "Expected \"(\" after __CUSHION_EVALUATED_ARGUMENT__ in replacement list.");
            return 1u;
        }

        context->current_token = context->current_token->next;
        // Preprocessor, new line, glue, comment and end of file should never appear here anyway.

        if (!context->current_token || context->current_token->token.type != CUSHION_TOKEN_TYPE_IDENTIFIER)
        {
            cushion_instance_execution_error (
                state->instance, macro_replacement_error_context (state, context),
                "Expected identifier as __CUSHION_EVALUATED_ARGUMENT__ argument in replacement list.");
            return 1u;
        }

        // Gather input argument/__VA_ARGS__/__VA_OPT__ tokens into sublist.
        if (!macro_replacement_context_process_identifier_into_sub_list (state, context))
        {
            cushion_instance_execution_error (
                state->instance, macro_replacement_error_context (state, context),
                "Expected argument name, __VA_ARGS__ or __VA_OPT__ as __CUSHION_EVALUATED_ARGUMENT__ argument in "
                "replacement list, but got another identifier.");
            return 1u;
        }

        if (!context->sub_list.first)
        {
            // Nothing to evaluate.
            return 1u;
        }

        macro_replacement_context_evaluate_sub_list (state, context);
        context->current_token = context->current_token->next;
        // Preprocessor, new line, glue, comment and end of file should never appear here anyway.

        if (!context->current_token || context->current_token->token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
            context->current_token->token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS)
        {
            cushion_instance_execution_error (
                state->instance, macro_replacement_error_context (state, context),
                "Expected \")\" after __CUSHION_EVALUATED_ARGUMENT__ argument in replacement list.");
            return 1u;
        }

        return 1u;
    }

    case CUSHION_IDENTIFIER_KIND_CUSHION_REPLACEMENT_INDEX:
    {
        // Other parts of code might expect stringized value of literal, so we need to create it, unfortunately.
        unsigned int value = context->replacement_index;
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
        value = context->replacement_index;

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

        struct cushion_token_t token = {
            .type = CUSHION_TOKEN_TYPE_NUMBER_INTEGER,
            .begin = formatted_literal,
            .end = formatted_literal + string_length,
            .unsigned_number_value = state->last_token_line,
        };

        macro_replacement_token_list_append (state, &context->sub_list, &token, state->tokenization.file_name,
                                             context->replacement_line);
        return 1u;
    }
#endif

    default:
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
                macro_replacement_token_list_append (state, &context->sub_list, &argument_token->token,
                                                     state->tokenization.file_name, context->replacement_line);
                argument_token = argument_token->next;
            }

            return 1u;
        }

        // Just a regular identifier.
        macro_replacement_token_list_append (state, &context->sub_list, &context->current_token->token,
                                             state->tokenization.file_name, context->replacement_line);
        return 0u;
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

static struct cushion_token_list_item_t *lex_do_macro_replacement (
    struct cushion_lexer_file_state_t *state,
    struct cushion_macro_node_t *macro,
    struct lex_macro_argument_t *arguments,
    struct cushion_token_list_item_t *wrapped_tokens,
    // Replacement line is used to properly set file name and line marker information for replacement tokens.
    unsigned int replacement_line)
{
    // Technically, full replacement pass is only needed for function-like macros as we need to properly process
    // arguments. However, object-like macros can still use ## operation to merge tokens. It should be processed
    // properly in lex_replacement_list function, but it would complicate that function with knowledge of arguments
    // and would make it logic much more complicated. Therefore, we do full replacement for object-like macros too
    // in this function.

    struct macro_replacement_context_t context = {
        .macro = macro,
        .arguments = arguments,
        .wrapped_tokens = wrapped_tokens,
        .current_token = macro->replacement_list_first,
        .replacement_line = replacement_line,
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
#if defined(CUSHION_EXTENSIONS)
        .replacement_index = state->instance->macro_replacement_index++,
#endif
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
                        state->instance, macro_replacement_error_context (state, &context),
                        "Encountered \"#\" operator as a last token in macro replacement list.");
                    break;
                }

                if (context.current_token->token.type != CUSHION_TOKEN_TYPE_IDENTIFIER)
                {
                    cushion_instance_execution_error (
                        state->instance, macro_replacement_error_context (state, &context),
                        "Non-comment token following \"#\" operator is not an identifier.");
                    break;
                }

                if (!macro_replacement_context_process_identifier_into_sub_list (state, &context))
                {
                    cushion_instance_execution_error (
                        state->instance, macro_replacement_error_context (state, &context),
                        "Expected argument name, __VA_ARGS__ or __VA_OPT__ as \"#\" operand in "
                        "replacement list, but got another identifier.");
                    break;
                }

                const unsigned int stringized_size =
                    2u + lex_calculate_stringized_internal_size (context.sub_list.first);

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
                    lex_write_stringized_internal_tokens (context.sub_list.first,
                                                          (char *) stringized_token.symbolic_literal.begin);

                assert (output_end <= stringized_token.symbolic_literal.end);
                *(char *) stringized_token.begin = '"';
                *(char *) stringized_token.symbolic_literal.end = '"';
                macro_replacement_token_list_append (state, &context.result, &stringized_token,
                                                     state->tokenization.file_name, context.replacement_line);
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
                        state->instance, macro_replacement_error_context (state, &context),
                        "Encountered \"##\" operator as a first token in macro replacement list.");
                    break;
                }

                if (context.result.last->token.type != CUSHION_TOKEN_TYPE_IDENTIFIER)
                {
                    cushion_instance_execution_error (
                        state->instance, macro_replacement_error_context (state, &context),
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
                            state->instance, macro_replacement_error_context (state, &context),
                            "Encountered \"##\" operator as a last token in macro replacement list.");
                        break;
                    }

#define CHECK_APPENDED_TOKEN_TYPE(TYPE)                                                                                \
    switch (TYPE)                                                                                                      \
    {                                                                                                                  \
    case CUSHION_TOKEN_TYPE_IDENTIFIER:                                                                                \
    case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:                                                                            \
    case CUSHION_TOKEN_TYPE_DIGIT_IDENTIFIER_SEQUENCE:                                                                 \
        /* We know how to append that to identifier. */                                                                \
        break;                                                                                                         \
                                                                                                                       \
    default:                                                                                                           \
        cushion_instance_execution_error (                                                                             \
            state->instance, macro_replacement_error_context (state, &context),                                        \
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
                        (unsigned int) (context.result.last->token.end - context.result.last->token.begin);

                    unsigned int append_identifier_length =
                        (unsigned int) (context.sub_list.first->token.end - context.sub_list.first->token.begin);

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
                macro_replacement_token_list_append (state, &context.result, &context.current_token->token,
                                                     state->tokenization.file_name, context.replacement_line);
                break;
            }

            break;

        case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:
        case CUSHION_TOKEN_TYPE_NUMBER_FLOATING:
        case CUSHION_TOKEN_TYPE_DIGIT_IDENTIFIER_SEQUENCE:
        case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
        case CUSHION_TOKEN_TYPE_STRING_LITERAL:
        case CUSHION_TOKEN_TYPE_OTHER:
            macro_replacement_token_list_append (state, &context.result, &context.current_token->token,
                                                 state->tokenization.file_name, context.replacement_line);
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

static inline void lex_on_line_mark_manually_updated (struct cushion_lexer_file_state_t *state,
                                                      const char *file,
                                                      unsigned int line)
{
    state->last_marked_file = file;
    state->last_marked_line = line;
}

static unsigned int lex_update_line_mark (struct cushion_lexer_file_state_t *state,
                                          const char *required_file,
                                          unsigned int required_line)
{
    const unsigned int same_file =
        required_file == state->last_marked_file || strcmp (required_file, state->last_marked_file) == 0;

    // Check line number and add marker if needed.
    if (state->last_marked_line != required_line || !same_file)
    {
        const int max_lines_to_cover_with_new_line = 5u;
        if (same_file && state->last_marked_line < required_line &&
            (int) required_line - (int) state->last_marked_line < max_lines_to_cover_with_new_line)
        {
            int difference = (int) required_line - (int) state->last_marked_line;
            while (difference)
            {
                cushion_instance_output_null_terminated (state->instance, "\n");
                --difference;
            }
        }
        else
        {
            cushion_instance_output_null_terminated (state->instance, "\n");
            cushion_instance_output_line_marker (state->instance, required_file, required_line);
        }

        lex_on_line_mark_manually_updated (state, required_file, required_line);
        return 1u;
    }

    return 0u;
}

enum lex_replace_identifier_if_macro_context_t
{
    LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_CODE = 0u,
    LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_EVALUATION,
};

/// \details We cannot just return pointer to token list as token-less macros exist.
struct lex_replace_macro_result_t
{
    unsigned int replaced;
    struct cushion_token_list_item_t *tokens;
};

static struct lex_replace_macro_result_t lex_replace_identifier_if_macro (
    struct cushion_lexer_file_state_t *state,
    struct cushion_token_t *identifier_token,
    const struct lexer_pop_token_meta_t *identifier_token_meta,
    enum lex_replace_identifier_if_macro_context_t context);

static inline void lex_output_preserved_tail_beginning (
    struct cushion_lexer_file_state_t *state,
    enum cushion_token_type_t preprocessor_token_type,
    // Macro node is required to properly paste function-line macro arguments if any.
    struct cushion_macro_node_t *macro_node_if_any)
{
    lex_update_line_mark (state, state->tokenization.file_name, state->tokenization.cursor_line);
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
    case CUSHION_TOKEN_TYPE_DIGIT_IDENTIFIER_SEQUENCE:
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

    lex_output_preserved_tail_beginning (state, preprocessor_token_type, macro_node_if_any);
    struct cushion_token_t current_token;
    struct lexer_pop_token_meta_t current_token_meta = {
        .flags = LEXER_TOKEN_STACK_ITEM_FLAG_NONE,
        .file = state->tokenization.file_name,
        .line = state->tokenization.cursor_line,
    };

    while (lexer_file_state_should_continue (state))
    {
        const unsigned int previous_is_macro_replacement =
            current_token_meta.flags & LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT;

        current_token_meta = lexer_file_state_pop_token (state, &current_token);
        LEX_WHEN_ERROR (break)

        // Add spaces to avoid merging tokens by mistake.
        if (previous_is_macro_replacement)
        {
            cushion_instance_output_null_terminated (state->instance, " ");
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
            cushion_instance_lexer_error (state, &current_token_meta,
                                          "Encountered preprocessor directive while lexing preserved preprocessor "
                                          "directive. Shouldn't be possible at all, can be an internal error.");
            return;

        case CUSHION_TOKEN_TYPE_IDENTIFIER:
        {
            switch (current_token.identifier_kind)
            {
            case CUSHION_IDENTIFIER_KIND_CUSHION_PRESERVE:
                cushion_instance_lexer_error (state, &current_token_meta,
                                              "Encountered cushion preserve keyword in unexpected context.");
                return;

            case CUSHION_IDENTIFIER_KIND_CUSHION_WRAPPED:
                cushion_instance_lexer_error (state, &current_token_meta,
                                              "Encountered cushion wrapped keyword in preserved preprocessor context.");
                return;

            case CUSHION_IDENTIFIER_KIND_CUSHION_EVALUATED_ARGUMENT:
                cushion_instance_lexer_error (
                    state, &current_token_meta,
                    "Encountered cushion evaluate argument keyword in preserved preprocessor context.");
                return;

            case CUSHION_IDENTIFIER_KIND_CUSHION_REPLACEMENT_INDEX:
                cushion_instance_lexer_error (
                    state, &current_token_meta,
                    "Encountered cushion replacement index keyword in preserved preprocessor context.");
                return;

            default:
                break;
            }

            struct lex_replace_macro_result_t replace_result = lex_replace_identifier_if_macro (
                state, &current_token, &current_token_meta, LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_EVALUATION);

            if (replace_result.replaced)
            {
                lexer_file_state_push_tokens (state, replace_result.tokens,
                                              LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT);
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
        case CUSHION_TOKEN_TYPE_DIGIT_IDENTIFIER_SEQUENCE:
        case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
        case CUSHION_TOKEN_TYPE_STRING_LITERAL:
        case CUSHION_TOKEN_TYPE_GLUE:
        case CUSHION_TOKEN_TYPE_OTHER:
            cushion_instance_output_sequence (state->instance, current_token.begin, current_token.end);
            break;

        case CUSHION_TOKEN_TYPE_NEW_LINE:
            // New line, tail has ended, paste the new line and return out of here.
            cushion_instance_output_sequence (state->instance, current_token.begin, current_token.end);
            lex_on_line_mark_manually_updated (state, state->tokenization.file_name, state->tokenization.cursor_line);
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

static struct lex_replace_macro_result_t lex_replace_identifier_if_macro (
    struct cushion_lexer_file_state_t *state,
    struct cushion_token_t *identifier_token,
    const struct lexer_pop_token_meta_t *identifier_token_meta,
    enum lex_replace_identifier_if_macro_context_t context)
{
    struct cushion_macro_node_t *macro =
        cushion_instance_macro_search (state->instance, identifier_token->begin, identifier_token->end);

#define RETURN_NOT_REPLACED                                                                                            \
    return (struct lex_replace_macro_result_t) { .replaced = 0u, .tokens = NULL }

    if (!macro || (macro->flags & CUSHION_MACRO_FLAG_PRESERVED))
    {
        // No need to unwrap.
        RETURN_NOT_REPLACED;
    }

    const unsigned int start_line = identifier_token_meta->line;
    switch (context)
    {
    case LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_CODE:
        break;

    case LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_EVALUATION:
#if defined(CUSHION_EXTENSIONS)
        if (macro->flags & CUSHION_MACRO_FLAG_WRAPPED)
        {
            cushion_instance_lexer_error (
                state, identifier_token_meta,
                "Encountered macro that uses __CUSHION_WRAPPED__ inside evaluation context, which is not supported as "
                "this kind of macros can never be unwrapped into valid integer constant expression.");
            RETURN_NOT_REPLACED;
        }
#endif
        break;
    }

    struct lex_macro_argument_t *arguments = NULL;
    if (macro->flags & CUSHION_MACRO_FLAG_FUNCTION)
    {
        struct lex_macro_argument_read_context_t argument_context =
            lex_macro_argument_read_context_init (macro, start_line);

        // Scan for the opening parenthesis.
        unsigned int skipping_until_significant = 1u;

        while (skipping_until_significant && !cushion_instance_is_error_signaled (state->instance))
        {
            argument_context.current_token_meta = lexer_file_state_pop_token (state, &argument_context.current_token);
            LEX_WHEN_ERROR (break)

            switch (argument_context.current_token.type)
            {
            case CUSHION_TOKEN_TYPE_NEW_LINE:
                switch (context)
                {
                case LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_CODE:
                    // As macro replacement cannot have new lines inside them, then we cannot be in macro replacement
                    // and therefore can freely output new line.
                    cushion_instance_output_sequence (state->instance, argument_context.current_token.begin,
                                                      argument_context.current_token.end);
                    break;

                case LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_EVALUATION:
                    cushion_instance_lexer_error (
                        state, &argument_context.current_token_meta,
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

        LEX_WHEN_ERROR (RETURN_NOT_REPLACED)
        if (argument_context.current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
            argument_context.current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS)
        {
            cushion_instance_lexer_error (state, &argument_context.current_token_meta,
                                          "Expected \"(\" after function-line macro name.");
            RETURN_NOT_REPLACED;
        }

        while (argument_context.parenthesis_counter > 0u && !cushion_instance_is_error_signaled (state->instance))
        {
            argument_context.current_token_meta = lexer_file_state_pop_token (state, &argument_context.current_token);
            LEX_WHEN_ERROR (break)

            if (argument_context.current_token.type == CUSHION_TOKEN_TYPE_NEW_LINE)
            {
                switch (context)
                {
                case LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_CODE:
                    // Just skip new line, line number after token replacement would be managed through directives.
                    break;

                case LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_EVALUATION:
                    cushion_instance_lexer_error (
                        state, &argument_context.current_token_meta,
                        "Reached new line while parsing arguments of function-line macro inside "
                        "preprocessor directive evaluation.");
                    break;
                }
            }

            lex_macro_argument_read_step (state, &argument_context);
        }

        LEX_WHEN_ERROR (RETURN_NOT_REPLACED)
        if (argument_context.parameter)
        {
            cushion_instance_lexer_error (state, &argument_context.current_token_meta,
                                          "Encountered less parameters for function-line macro than expected.");
            RETURN_NOT_REPLACED;
        }

        arguments = argument_context.arguments_first;
    }

    struct cushion_token_list_item_t *wrapped_tokens_first = NULL;
#if defined(CUSHION_EXTENSIONS)
    struct cushion_token_list_item_t *wrapped_tokens_last = NULL;

    if (macro->flags & CUSHION_MACRO_FLAG_WRAPPED)
    {
        // Should not have flag if feature is not enabled.
        assert (cushion_instance_has_feature (state->instance, CUSHION_FEATURE_WRAPPER_MACRO));

        // Not supported inside evaluation anyway.
        assert (context != LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_EVALUATION);

        // Scan for opening brace.
        // Initialize current token with default values so warnings checks do not trigger on it.
        struct cushion_token_t current_token = {
            .type = CUSHION_TOKEN_TYPE_NEW_LINE,
            .begin = "\n",
        };

        current_token.end = current_token.begin + 1u;
        struct lexer_pop_token_meta_t current_token_meta;
        unsigned int skipping_until_significant = 1u;

        while (skipping_until_significant && !cushion_instance_is_error_signaled (state->instance))
        {
            current_token_meta = lexer_file_state_pop_token (state, &current_token);
            LEX_WHEN_ERROR (break)

            switch (current_token.type)
            {
            case CUSHION_TOKEN_TYPE_NEW_LINE:
            case CUSHION_TOKEN_TYPE_GLUE:
            case CUSHION_TOKEN_TYPE_COMMENT:
                // New line, glue and comments between macro scope and wrapped block are just skipped.
                break;

            default:
                // Found something significant.
                skipping_until_significant = 0u;
                break;
            }
        }

        LEX_WHEN_ERROR (RETURN_NOT_REPLACED)
        if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
            current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_LEFT_CURLY_BRACE)
        {
            cushion_instance_lexer_error (state, &current_token_meta,
                                          "Expected \"{\" after function-line macro with wrapped block arguments.");
            RETURN_NOT_REPLACED;
        }

#    define APPEND_WRAPPED_TOKEN(LIST_NAME, TOKEN_TO_SAVE)                                                             \
        {                                                                                                              \
            struct cushion_token_list_item_t *LIST_NAME##_new_token =                                                  \
                cushion_save_token_to_memory (state->instance, TOKEN_TO_SAVE, CUSHION_ALLOCATION_CLASS_TRANSIENT);     \
                                                                                                                       \
            LIST_NAME##_new_token->file = current_token_meta.file;                                                     \
            LIST_NAME##_new_token->line = current_token_meta.line;                                                     \
                                                                                                                       \
            if ((current_token_meta.flags & LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT) == 0u)                      \
            {                                                                                                          \
                LIST_NAME##_new_token->flags |= CUSHION_TOKEN_LIST_ITEM_FLAG_WRAPPED_BLOCK;                            \
            }                                                                                                          \
                                                                                                                       \
            if (LIST_NAME##_last)                                                                                      \
            {                                                                                                          \
                LIST_NAME##_last->next = LIST_NAME##_new_token;                                                        \
            }                                                                                                          \
            else                                                                                                       \
            {                                                                                                          \
                LIST_NAME##_first = LIST_NAME##_new_token;                                                             \
            }                                                                                                          \
                                                                                                                       \
            LIST_NAME##_last = LIST_NAME##_new_token;                                                                  \
        }

        // Append initial brace.
        unsigned int brace_counter = 1u;

        while (brace_counter > 0u && !cushion_instance_is_error_signaled (state->instance))
        {
            current_token_meta = lexer_file_state_pop_token (state, &current_token);
            LEX_WHEN_ERROR (break)

            switch (current_token.type)
            {
            case CUSHION_TOKEN_TYPE_PUNCTUATOR:
                switch (current_token.punctuator_kind)
                {
                case CUSHION_PUNCTUATOR_KIND_LEFT_CURLY_BRACE:
                    ++brace_counter;
                    break;

                case CUSHION_PUNCTUATOR_KIND_RIGHT_CURLY_BRACE:
                    --brace_counter;
                    break;

                default:
                    break;
                }

                if (brace_counter > 0u)
                {
                    APPEND_WRAPPED_TOKEN (wrapped_tokens, &current_token)
                }

                break;

            case CUSHION_TOKEN_TYPE_END_OF_FILE:
                cushion_instance_lexer_error (state, &current_token_meta,
                                              "Got to the end of file while parsing wrapped block for "
                                              "function-like macro with wrapped block feature enabled.");
                break;

            default:
                APPEND_WRAPPED_TOKEN (wrapped_tokens, &current_token)
                break;
            }
        }

        LEX_WHEN_ERROR (RETURN_NOT_REPLACED)
#    undef APPEND_WRAPPED_TOKEN
    }
#endif

    return (struct lex_replace_macro_result_t) {
        .replaced = 1u, .tokens = lex_do_macro_replacement (state, macro, arguments, wrapped_tokens_first, start_line)};
#undef RETURN_NOT_REPLACED
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

static inline struct lexer_pop_token_meta_t lex_skip_glue_and_comments (struct cushion_lexer_file_state_t *state,
                                                                        struct cushion_token_t *current_token)
{
    struct lexer_pop_token_meta_t meta = {
        .file = state->last_marked_file,
        .line = state->last_token_line,
        .flags = LEXER_TOKEN_STACK_ITEM_FLAG_NONE,
    };

    while (lexer_file_state_should_continue (state))
    {
        meta = lexer_file_state_pop_token (state, current_token);
        LEX_WHEN_ERROR (return meta)

        switch (current_token->type)
        {
        case CUSHION_TOKEN_TYPE_GLUE:
        case CUSHION_TOKEN_TYPE_COMMENT:
            break;

        default:
            return meta;
        }
    }

    return meta;
}

static long long lex_do_defined_check (struct cushion_lexer_file_state_t *state,
                                       struct cushion_token_t *current_token,
                                       const struct lexer_pop_token_meta_t *meta)
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
        case CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_REF:
        case CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_UNREF:
        case CUSHION_IDENTIFIER_KIND_CUSHION_SNIPPET:
        case CUSHION_IDENTIFIER_KIND_CUSHION_EVALUATED_ARGUMENT:
        case CUSHION_IDENTIFIER_KIND_CUSHION_REPLACEMENT_INDEX:
            cushion_instance_lexer_error (state, meta, "Encountered unsupported reserved identifier in defined check.");
            return 0u;

        default:
            return cushion_instance_macro_search (state->instance, current_token->begin, current_token->end) ? 1u : 0u;
        }

        break;

    default:
        cushion_instance_lexer_error (state, meta, "Expected identifier for defined check.");
        return 0u;
    }
}

static long long lex_preprocessor_evaluate_defined (struct cushion_lexer_file_state_t *state)
{
    struct cushion_token_t current_token;
    struct lexer_pop_token_meta_t current_token_meta = lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return 0u)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS)
    {
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Expected \"(\" after \"defined\" in preprocessor expression evaluation.");
        return 0u;
    }

    current_token_meta = lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return 0u)

    long long result = lex_do_defined_check (state, &current_token, &current_token_meta);
    LEX_WHEN_ERROR (return 0u)

    current_token_meta = lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return 0u)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS)
    {
        cushion_instance_lexer_error (
            state, &current_token_meta,
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
        struct lexer_pop_token_meta_t meta = lexer_file_state_pop_token (state, &current_token);
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
            cushion_instance_lexer_error (
                state, &meta,
                "Encountered preprocessor directive while evaluating preprocessor conditional "
                "expression. Shouldn't be possible at all, can be an internal error.");
            return 0;

        case CUSHION_TOKEN_TYPE_IDENTIFIER:
        {
            switch (current_token.identifier_kind)
            {
            case CUSHION_IDENTIFIER_KIND_LINE:
                return meta.line;

            case CUSHION_IDENTIFIER_KIND_DEFINED:
                return lex_preprocessor_evaluate_defined (state);

            case CUSHION_IDENTIFIER_KIND_HAS_INCLUDE:
            case CUSHION_IDENTIFIER_KIND_HAS_EMBED:
            case CUSHION_IDENTIFIER_KIND_HAS_C_ATTRIBUTE:
            {
                cushion_instance_lexer_error (
                    state, &meta,
                    "Encountered has_* check while evaluation preprocessor conditional expression. These checks are "
                    "not supported as Cushion is not guaranteed to have enough info to process them properly.");
                return 0;
                break;
            }

            default:
            {
                struct lex_replace_macro_result_t replace_result = lex_replace_identifier_if_macro (
                    state, &current_token, &meta, LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_EVALUATION);

                if (!replace_result.replaced)
                {
                    cushion_instance_lexer_error (
                        state, &meta,
                        "Encountered identifier which can not be unwrapped as macro while evaluation "
                        "preprocessor conditional expression. Identifiers can not be present in these "
                        "expressions as they must be integer constants.");
                    return 0;
                }

                lexer_file_state_push_tokens (state, replace_result.tokens,
                                              LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT);
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
                cushion_instance_lexer_error (
                    state, &meta,
                    "Encountered unexpected punctuator while evaluating preprocessor conditional expression.");
                return 0;
            }

        case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:
            if (current_token.unsigned_number_value > LLONG_MAX)
            {
                cushion_instance_lexer_error (
                    state, &meta,
                    "Encountered integer number which is higher than %lld (LLONG_MAX) while evaluating preprocessor "
                    "conditional expression, which is not supported by Cushion right now.",
                    (long long) LLONG_MAX);
                return 0;
            }

            return (long long) current_token.unsigned_number_value;

        case CUSHION_TOKEN_TYPE_NUMBER_FLOATING:
            cushion_instance_lexer_error (state, &meta,
                                          "Encountered non-integer number while evaluating preprocessor conditional "
                                          "expression, which is not supported by specification.");
            return 0;

        case CUSHION_TOKEN_TYPE_DIGIT_IDENTIFIER_SEQUENCE:
            cushion_instance_lexer_error (state, &meta,
                                          "Encountered digit-identifier sequence token expecting next argument for "
                                          "preprocessor conditional expression.");
            return 0;

        case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
            if (current_token.symbolic_literal.encoding != CUSHION_TOKEN_SUBSEQUENCE_ENCODING_ORDINARY)
            {
                cushion_instance_lexer_error (
                    state, &meta,
                    "Encountered non-ordinary character literal while evaluating preprocessor "
                    "conditional expression, which is currently not supported by Cushion.");
                return 0;
            }

            if (current_token.symbolic_literal.end - current_token.symbolic_literal.begin != 1u)
            {
                cushion_instance_lexer_error (
                    state, &meta,
                    "Encountered non-single-character character literal while evaluating preprocessor conditional "
                    "expression, which is currently not supported by Cushion.");
                return 0;
            }

            return (long long) *current_token.symbolic_literal.begin;

        case CUSHION_TOKEN_TYPE_STRING_LITERAL:
            cushion_instance_lexer_error (
                state, &meta,
                "Encountered string literal while evaluating preprocessor conditional expression, "
                "which is not supported by specification.");
            return 0;

        case CUSHION_TOKEN_TYPE_NEW_LINE:
            cushion_instance_lexer_error (
                state, &meta,
                "Encountered end of line while expecting next argument for preprocessor conditional expression.");
            return 0;

        case CUSHION_TOKEN_TYPE_GLUE:
        case CUSHION_TOKEN_TYPE_COMMENT:
            // Never interested in it inside conditional, continue lexing.
            break;

        case CUSHION_TOKEN_TYPE_END_OF_FILE:
            cushion_instance_lexer_error (
                state, &meta,
                "Encountered end of file while expecting next argument for preprocessor conditional expression.");
            return 0;

        case CUSHION_TOKEN_TYPE_OTHER:
            cushion_instance_lexer_error (
                state, &meta,
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
    struct lexer_pop_token_meta_t current_token_meta;
    struct lex_evaluate_stack_item_t *evaluation_stack_top = NULL;
    struct lex_evaluate_stack_item_t *evaluation_stack_reuse = NULL;

    while (lexer_file_state_should_continue (state))
    {
        long long new_argument = lex_preprocessor_evaluate_argument (state, sub_expression_type);

        // We need a label due to custom logic needed to process ternary operator.
        // When ternary is fully processed, it is treated as single argument and operator
        // after it is requested right away.
    lex_next_operator:
        current_token_meta = lex_skip_glue_and_comments (state, &current_token);
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
                    cushion_instance_lexer_error (
                        state, &current_token_meta,
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
                    cushion_instance_lexer_error (
                        state, &current_token_meta,
                        "Encountered unexpected \":\" in preprocessor expression evaluation.");
                    break;

                case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_POSITIVE:
                    goto finish_evaluation;
                    break;
                }

                break;

            default:
                cushion_instance_lexer_error (
                    state, &current_token_meta,
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
                        _Alignof (struct lex_evaluate_stack_item_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);
                }

                new_item->left_value = new_argument;
                new_item->operator = next_operator;
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
                cushion_instance_lexer_error (state, &current_token_meta,
                                              "Expected \")\" but got new line in preprocessor expression evaluation.");

            case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_POSITIVE:
                cushion_instance_lexer_error (state, &current_token_meta,
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
                cushion_instance_lexer_error (
                    state, &current_token_meta,
                    "Expected \")\" but got end of file in preprocessor expression evaluation.");

            case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_POSITIVE:
                cushion_instance_lexer_error (
                    state, &current_token_meta,
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
                lexer_file_state_reinsert_token (state, &current_token);
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
            cushion_instance_lexer_error (
                state, &current_token_meta,
                "Expected operator token after argument in preprocessor expression evaluation.");
            break;
        }
    }

    cushion_instance_lexer_error (state, &current_token_meta,
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
        node->line = state->last_token_line;
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

    const unsigned int start_line = state->last_token_line;
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
        node->line = start_line;
        state->conditional_inclusion_node = node;

        lex_preprocessor_preserved_tail (state, preprocessor_token->type, NULL);
        lex_update_tokenization_flags (state);
        return;
    }

    lexer_file_state_reinsert_token (state, &current_token);
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
    lex_update_tokenization_flags (state);
}

static void lex_preprocessor_expect_new_line (struct cushion_lexer_file_state_t *state)
{
    struct cushion_token_t current_token;
    struct lexer_pop_token_meta_t current_token_meta = lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    switch (current_token.type)
    {
    case CUSHION_TOKEN_TYPE_NEW_LINE:
        break;

    default:
        cushion_instance_lexer_error (state, &current_token_meta, "Expected new line after preprocessor expression.");
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
    unsigned int start_line = state->last_token_line;
    struct cushion_token_t current_token;

    struct lexer_pop_token_meta_t current_token_meta = lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    long long check_result = lex_do_defined_check (state, &current_token, &current_token_meta);
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
        cushion_instance_lexer_error (state, preprocessor_token_meta,                                                  \
                                      "Found else family preprocessor without if family preprocessor before it.");     \
        return;                                                                                                        \
    }                                                                                                                  \
                                                                                                                       \
    if (state->conditional_inclusion_node->flags & CONDITIONAL_INCLUSION_FLAGS_HAD_PLAIN_ELSE)                         \
    {                                                                                                                  \
        cushion_instance_lexer_error (state, preprocessor_token_meta,                                                  \
                                      "Found else family preprocessor in chain after unconditional #else.");           \
        return;                                                                                                        \
    }

static void lex_preprocessor_elif (struct cushion_lexer_file_state_t *state,
                                   const struct cushion_token_t *preprocessor_token,
                                   const struct lexer_pop_token_meta_t *preprocessor_token_meta)
{
    LEX_PREPROCESSOR_ELSE_VALIDATE
    if (lex_preprocessor_else_is_transitively_excluded_conditional (state) ||
        lex_preprocessor_else_is_transitively_preserved (state, preprocessor_token))
    {
        return;
    }

    lex_do_not_skip_regular (state);
    unsigned int start_line = state->last_token_line;
    const long long evaluation_result = lex_preprocessor_evaluate (state, LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_ROOT);
    LEX_WHEN_ERROR (return)

    lexer_conditional_inclusion_node_set_state (
        state->conditional_inclusion_node,
        evaluation_result ? CONDITIONAL_INCLUSION_STATE_INCLUDED : CONDITIONAL_INCLUSION_STATE_EXCLUDED);

    state->conditional_inclusion_node->line = start_line;
    lex_update_tokenization_flags (state);
}

static void lex_preprocessor_elifdef (struct cushion_lexer_file_state_t *state,
                                      const struct cushion_token_t *preprocessor_token,
                                      const struct lexer_pop_token_meta_t *preprocessor_token_meta,
                                      unsigned int reverse)
{
    LEX_PREPROCESSOR_ELSE_VALIDATE
    if (lex_preprocessor_else_is_transitively_excluded_conditional (state) ||
        lex_preprocessor_else_is_transitively_preserved (state, preprocessor_token))
    {
        return;
    }

    lex_do_not_skip_regular (state);
    const unsigned int start_line = state->last_token_line;
    struct cushion_token_t current_token;

    struct lexer_pop_token_meta_t current_token_meta = lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    long long check_result = lex_do_defined_check (state, &current_token, &current_token_meta);
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
    lex_update_tokenization_flags (state);
}

static void lex_preprocessor_else (struct cushion_lexer_file_state_t *state,
                                   const struct cushion_token_t *preprocessor_token,
                                   const struct lexer_pop_token_meta_t *preprocessor_token_meta)
{
    LEX_PREPROCESSOR_ELSE_VALIDATE
    if (lex_preprocessor_else_is_transitively_excluded_conditional (state) ||
        lex_preprocessor_else_is_transitively_preserved (state, preprocessor_token))
    {
        return;
    }

    lex_do_not_skip_regular (state);
    unsigned int start_line = state->last_token_line;

    lex_preprocessor_expect_new_line (state);
    LEX_WHEN_ERROR (return)

    lexer_conditional_inclusion_node_set_state (state->conditional_inclusion_node,
                                                CONDITIONAL_INCLUSION_STATE_INCLUDED);

    state->conditional_inclusion_node->line = start_line;
    lex_update_tokenization_flags (state);
}

static void lex_preprocessor_endif (struct cushion_lexer_file_state_t *state,
                                    const struct cushion_token_t *preprocessor_token,
                                    const struct lexer_pop_token_meta_t *preprocessor_token_meta)
{
    if (!state->conditional_inclusion_node)
    {
        cushion_instance_lexer_error (state, preprocessor_token_meta,
                                      "Found #endif without if-else family preprocessor before it.");
        return;
    }

    if (state->conditional_inclusion_node->state == CONDITIONAL_INCLUSION_STATE_PRESERVED)
    {
        lex_preprocessor_preserved_tail (state, preprocessor_token->type, NULL);
        state->conditional_inclusion_node = state->conditional_inclusion_node->previous;
        return;
    }

    lex_do_not_skip_regular (state);
    lex_preprocessor_expect_new_line (state);
    LEX_WHEN_ERROR (return)

    state->conditional_inclusion_node = state->conditional_inclusion_node->previous;
    lex_update_tokenization_flags (state);
}

static unsigned int lex_preprocessor_try_include (struct cushion_lexer_file_state_t *state,
                                                  const struct cushion_token_t *header_token,
                                                  const struct lexer_pop_token_meta_t *header_token_meta,
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
                cushion_instance_lexer_error (
                    state, header_token_meta,
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

    // Update line mark in order to avoid situations where #line directive is not starting on new line.
    // Also, it makes it easier to debug what was included from where.
    if ((state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u)
    {
        lex_update_line_mark (state, state->tokenization.file_name, state->tokenization.cursor_line);
    }

    cushion_lex_file_from_handle (state->instance, input_file, state->path_buffer.data, flags);
    fclose (input_file);
    return 1u;
}

enum lex_include_result_t
{
    LEX_INCLUDE_RESULT_NOT_FOUND = 0u,
    LEX_INCLUDE_RESULT_SCAN,
    LEX_INCLUDE_RESULT_FULL,
};

static void lex_preprocessor_include (struct cushion_lexer_file_state_t *state)
{
    lex_do_not_skip_regular (state);
    const unsigned int start_line = state->last_token_line;
    struct cushion_token_t current_token;

    struct lexer_pop_token_meta_t current_token_meta = lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    switch (current_token.type)
    {
    case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
    case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
        break;

    default:
        cushion_instance_lexer_error (state, &current_token_meta, "Expected header path after #include.");
        return;
    }

    enum lex_include_result_t include_result = LEX_INCLUDE_RESULT_NOT_FOUND;
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

        if (lex_preprocessor_try_include (state, &current_token, &current_token_meta, NULL))
        {
            include_result = LEX_INCLUDE_RESULT_FULL;
        }
    }

    // Try absolute include. It is a rare case, but may happen.
    if (include_result == LEX_INCLUDE_RESULT_NOT_FOUND)
    {
        lexer_file_state_path_init (state, NULL);
        LEX_WHEN_ERROR (return)

        if (lex_preprocessor_try_include (state, &current_token, &current_token_meta, NULL))
        {
            include_result = LEX_INCLUDE_RESULT_FULL;
        }
    }

    struct cushion_include_node_t *node = state->instance->includes_first;
    while (node && include_result == LEX_INCLUDE_RESULT_NOT_FOUND)
    {
        if (lex_preprocessor_try_include (state, &current_token, &current_token_meta, node))
        {
            include_result = node->type == INCLUDE_TYPE_FULL ? LEX_INCLUDE_RESULT_FULL : LEX_INCLUDE_RESULT_SCAN;
            break;
        }

        node = node->next;
    }

    if (include_result != LEX_INCLUDE_RESULT_FULL && (state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u)
    {
        // Include not found. Preserve it in code.
        lex_update_line_mark (state, state->tokenization.file_name, start_line);
        cushion_instance_output_null_terminated (state->instance, "#include ");
        cushion_instance_output_sequence (state->instance, current_token.begin, current_token.end);
    }

    lex_preprocessor_expect_new_line (state);
    LEX_WHEN_ERROR (return)

    if (include_result == LEX_INCLUDE_RESULT_FULL && (state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u)
    {
        cushion_instance_output_line_marker (state->instance, state->tokenization.file_name,
                                             state->tokenization.cursor_line);
        lex_on_line_mark_manually_updated (state, state->tokenization.file_name, state->tokenization.cursor_line);
    }

    lex_update_tokenization_flags (state);
}

static void lex_preprocessor_define (struct cushion_lexer_file_state_t *state)
{
    lex_do_not_skip_regular (state);
    struct cushion_token_t current_token;
    struct lexer_pop_token_meta_t current_token_meta = lex_skip_glue_and_comments (state, &current_token);
    struct lexer_pop_token_meta_t first_token_meta = current_token_meta;
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_IDENTIFIER)
    {
        cushion_instance_lexer_error (state, &current_token_meta, "Expected identifier after #define.");
        return;
    }

    switch (current_token.identifier_kind)
    {
    case CUSHION_IDENTIFIER_KIND_REGULAR:
        break;

    default:
        cushion_instance_lexer_error (state, &current_token_meta,
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

    current_token_meta = lexer_file_state_pop_token (state, &current_token);
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
            current_token_meta = lex_skip_glue_and_comments (state, &current_token);
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
            current_token_meta = lex_skip_glue_and_comments (state, &current_token);
            LEX_WHEN_ERROR (return)
        }

        if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
            current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS)
        {
            cushion_instance_lexer_error (state, &current_token_meta,
                                          "Expected \")\" or \",\" while reading macro parameter name list.");
            return;
        }

        break;
    }

    case CUSHION_TOKEN_TYPE_NEW_LINE:
    case CUSHION_TOKEN_TYPE_END_OF_FILE:
        // No replacement list, go directly to registration.

        if (state->conditional_inclusion_node &&
            state->conditional_inclusion_node->state == CONDITIONAL_INCLUSION_STATE_PRESERVED)
        {
            // Inside preserved scope, therefore is always preserved.
            node->flags |= CUSHION_MACRO_FLAG_PRESERVED | CUSHION_MACRO_FLAG_FROM_PRESERVED_SCOPE;
            if ((state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u)
            {
                lex_output_preserved_tail_beginning (state, CUSHION_TOKEN_TYPE_PREPROCESSOR_DEFINE, node);
                cushion_instance_output_null_terminated (state->instance, "\n");
                lex_on_line_mark_manually_updated (state, state->tokenization.file_name,
                                                   state->tokenization.cursor_line);
                return;
            }
        }

        goto register_macro;

    case CUSHION_TOKEN_TYPE_GLUE:
    case CUSHION_TOKEN_TYPE_COMMENT:
        break;

    default:
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Expected whitespaces, comments, \"(\", line end or file end after macro name.");
        return;
    }

    if (state->conditional_inclusion_node &&
        state->conditional_inclusion_node->state == CONDITIONAL_INCLUSION_STATE_PRESERVED)
    {
        // Inside preserved scope, therefore is always preserved.
        node->flags |= CUSHION_MACRO_FLAG_PRESERVED | CUSHION_MACRO_FLAG_FROM_PRESERVED_SCOPE;
        lex_preprocessor_preserved_tail (state, CUSHION_TOKEN_TYPE_PREPROCESSOR_DEFINE, node);
    }
    else
    {
        // Lex replacement list.
        enum cushion_lex_replacement_list_result_t lex_result = cushion_lex_replacement_list_from_lexer (
            state->instance, state, &node->replacement_list_first, &node->flags);
        LEX_WHEN_ERROR (return)

        switch (lex_result)
        {
        case CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR:
            break;

        case CUSHION_LEX_REPLACEMENT_LIST_RESULT_PRESERVED:
            node->flags |= CUSHION_MACRO_FLAG_PRESERVED;
            lex_preprocessor_preserved_tail (state, CUSHION_TOKEN_TYPE_PREPROCESSOR_DEFINE, node);
            break;
        }
    }

register_macro:
    // Register generated macro.
    cushion_instance_macro_add (state->instance, node, lex_error_context (state, &first_token_meta));
    lex_update_tokenization_flags (state);
}

static void lex_preprocessor_undef (struct cushion_lexer_file_state_t *state)
{
    const unsigned int start_line = state->last_token_line;
    lex_do_not_skip_regular (state);
    struct cushion_token_t current_token;
    struct lexer_pop_token_meta_t current_token_meta = lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_IDENTIFIER)
    {
        cushion_instance_lexer_error (state, &current_token_meta, "Expected identifier after #undef.");
        return;
    }

    struct cushion_macro_node_t *node =
        cushion_instance_macro_search (state->instance, current_token.begin, current_token.end);

    if (!node || (node->flags & CUSHION_MACRO_FLAG_PRESERVED))
    {
        // Preserve #undef as macro is either unknown or explicitly preserved.
        if ((state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u)
        {
            lex_update_line_mark (state, state->tokenization.file_name, start_line);
            cushion_instance_output_null_terminated (state->instance, "#undef ");
            cushion_instance_output_sequence (state->instance, current_token.begin, current_token.end);
        }
    }

    cushion_instance_macro_remove (state->instance, current_token.begin, current_token.end);
    lex_preprocessor_expect_new_line (state);
    lex_update_tokenization_flags (state);
}

static void lex_preprocessor_line (struct cushion_lexer_file_state_t *state)
{
    lex_do_not_skip_regular (state);
    struct cushion_token_t current_token;
    struct lexer_pop_token_meta_t current_token_meta = lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_NUMBER_INTEGER)
    {
        cushion_instance_lexer_error (
            state, &current_token_meta,
            "Expected integer line number after #line. Standard allows arbitrary preprocessor-evaluable expressions "
            "for line number calculations, but it is not yet supported in Cushion as it is a rare case.");
        return;
    }

    const unsigned long long line_number = current_token.unsigned_number_value;
    if (line_number > UINT_MAX)
    {
        cushion_instance_lexer_error (state, &current_token_meta, "Line number %llu is too big and is not supported.",
                                      line_number);
        return;
    }

    char *new_file_name = NULL;
    current_token_meta = lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    switch (current_token.type)
    {
    case CUSHION_TOKEN_TYPE_STRING_LITERAL:
    {
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
                    cushion_instance_lexer_error (state, &current_token_meta,
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
                    cushion_instance_lexer_error (state, &current_token_meta,
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
        current_token_meta = lex_skip_glue_and_comments (state, &current_token);
        LEX_WHEN_ERROR (return)

        switch (current_token.type)
        {
        case CUSHION_TOKEN_TYPE_NEW_LINE:
        case CUSHION_TOKEN_TYPE_END_OF_FILE:
            // File not specified, allowed by standard.
            break;

        default:
            cushion_instance_lexer_error (state, &current_token_meta,
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
        cushion_instance_lexer_error (
            state, &current_token_meta,
            "Expected file name literal or new line after line number in #line. Standard allows "
            "arbitrary preprocessor-evaluable expressions for file name determination, but it is "
            "not yet supported in Cushion as it is a rare case.");
        return;
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

        lex_update_tokenization_flags (state);
        return;
    }

    lexer_file_state_reinsert_token (state, &current_token);
    lex_preprocessor_preserved_tail (state, CUSHION_TOKEN_TYPE_PREPROCESSOR_PRAGMA, NULL);
    lex_update_tokenization_flags (state);
}

static inline struct lexer_pop_token_meta_t lex_skip_glue_comments_new_line (struct cushion_lexer_file_state_t *state,
                                                                             struct cushion_token_t *current_token)
{
    struct lexer_pop_token_meta_t meta = {
        .file = state->last_marked_file,
        .line = state->last_token_line,
        .flags = LEXER_TOKEN_STACK_ITEM_FLAG_NONE,
    };

    while (lexer_file_state_should_continue (state))
    {
        meta = lexer_file_state_pop_token (state, current_token);
        LEX_WHEN_ERROR (return meta)

        switch (current_token->type)
        {
        case CUSHION_TOKEN_TYPE_NEW_LINE:
        case CUSHION_TOKEN_TYPE_GLUE:
        case CUSHION_TOKEN_TYPE_COMMENT:
            break;

        default:
            return meta;
        }
    }

    return meta;
}

#if defined(CUSHION_EXTENSIONS)

/// \details Defers and statement accumulator pushes should have the same rules for their content,
///          therefore they're called extensions injectors and this logic is for them.
static struct cushion_token_list_item_t *lex_extension_injector_content (struct cushion_lexer_file_state_t *state,
                                                                         const char *safe_to_use_file_name)
{
    struct cushion_token_t current_token;
    struct lexer_pop_token_meta_t current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return NULL)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_LEFT_CURLY_BRACE)
    {
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Expected \"{\" after CUSHION_STATEMENT_ACCUMULATOR_PUSH / CUSHION_DEFER.");
        return NULL;
    }

    unsigned int brace_count = 1u;
    struct cushion_token_list_item_t *token_first = NULL;
    struct cushion_token_list_item_t *token_last = NULL;

    while (brace_count > 0u)
    {
        current_token_meta = lexer_file_state_pop_token (state, &current_token);
        if (cushion_instance_is_error_signaled (state->instance))
        {
            return NULL;
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
            // It is technically possible and even technically usable to put #line or #pragma inside push/defer, but it
            // makes lexing implementation more difficult and it seems not worth it to support them inside.
            cushion_instance_lexer_error (
                state, &current_token_meta,
                "Encountered preprocessor directive while reading CUSHION_STATEMENT_ACCUMULATOR_PUSH / CUSHION_DEFER "
                "code block. Preprocessor directives are not supported there as their support would make it possible "
                "to introduce circular dependency in code generation.");
            return NULL;

        case CUSHION_TOKEN_TYPE_PUNCTUATOR:
            switch (current_token.punctuator_kind)
            {
            case CUSHION_PUNCTUATOR_KIND_LEFT_CURLY_BRACE:
                ++brace_count;
                goto append_token;

            case CUSHION_PUNCTUATOR_KIND_RIGHT_CURLY_BRACE:
                --brace_count;

                if (brace_count == 0u)
                {
                    break;
                }

                goto append_token;

            default:
                goto append_token;
            }

            break;

        case CUSHION_TOKEN_TYPE_IDENTIFIER:
            switch (current_token.identifier_kind)
            {
            case CUSHION_IDENTIFIER_KIND_CUSHION_PRESERVE:
            case CUSHION_IDENTIFIER_KIND_CUSHION_DEFER:
            case CUSHION_IDENTIFIER_KIND_CUSHION_WRAPPED:
            case CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR:
            case CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_PUSH:
            case CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_REF:
            case CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_UNREF:
            case CUSHION_IDENTIFIER_KIND_CUSHION_SNIPPET:
            case CUSHION_IDENTIFIER_KIND_CUSHION_EVALUATED_ARGUMENT:
            case CUSHION_IDENTIFIER_KIND_CUSHION_REPLACEMENT_INDEX:
                cushion_instance_lexer_error (
                    state, &current_token_meta,
                    "Encountered cushion keywords in CUSHION_STATEMENT_ACCUMULATOR_PUSH / CUSHION_DEFER code block, "
                    "which is not supported as it might result in circular dependency in code generation.");
                return NULL;

            case CUSHION_IDENTIFIER_KIND_MACRO_PRAGMA:
                // It is possible to support _Pragma here, but it is just a little bit of extra work that is not really
                // important and useful in practice, therefore we forbid it for now.
                cushion_instance_lexer_error (state, &current_token_meta,
                                              "Encountered _Pragma in CUSHION_STATEMENT_ACCUMULATOR_PUSH / "
                                              "CUSHION_DEFER code block, which is not supported right now.");
                return NULL;

            default:
            {
                struct lex_replace_macro_result_t replace_result = lex_replace_identifier_if_macro (
                    state, &current_token, &current_token_meta, LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_CODE);

                if (replace_result.replaced)
                {
                    lexer_file_state_push_tokens (state, replace_result.tokens,
                                                  LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT);
                }
                else
                {
                    goto append_token;
                }

                break;
            }
            }

            break;

        case CUSHION_TOKEN_TYPE_NEW_LINE:
        case CUSHION_TOKEN_TYPE_COMMENT:
            // Skipped as for macro replacement.
            break;

        case CUSHION_TOKEN_TYPE_END_OF_FILE:
            cushion_instance_lexer_error (
                state, &current_token_meta,
                "Got to the end of file while parsing CUSHION_STATEMENT_ACCUMULATOR_PUSH / CUSHION_DEFER code block.");
            return NULL;

        default:
        append_token:
        {
            struct cushion_token_list_item_t *new_token =
                cushion_save_token_to_memory (state->instance, &current_token, CUSHION_ALLOCATION_CLASS_PERSISTENT);

            new_token->file = safe_to_use_file_name;
            new_token->line = current_token_meta.line;

            if (current_token_meta.flags & LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT)
            {
                new_token->flags |= CUSHION_TOKEN_LIST_ITEM_FLAG_INJECTED_MACRO_REPLACEMENT;
            }

            if (token_last)
            {
                token_last->next = new_token;
            }
            else
            {
                token_first = new_token;
            }

            token_last = new_token;
        }
        }
    }

    return token_first;
}

enum output_extension_injector_content_flags_t
{
    OUTPUT_EXTENSION_INJECTOR_FLAGS_NONE = 0u,
    OUTPUT_EXTENSION_INJECTOR_FLAGS_JUMP_FORBIDDEN = 1u << 0u,
};

static void output_extension_injector_content (struct cushion_instance_t *instance,
                                               struct cushion_token_list_item_t *content,
                                               enum output_extension_injector_content_flags_t flags)
{
    if (!content || cushion_instance_is_error_signaled (instance))
    {
        return;
    }

    cushion_instance_output_null_terminated (instance, "\n");
    cushion_instance_output_line_marker (instance, content->file, content->line);

    const char *last_output_file = content->file;
    unsigned int last_output_line = content->line;
    unsigned int previous_is_macro = 0u;

    while (content && !cushion_instance_is_error_signaled (instance))
    {
        if (last_output_line != content->line ||
            (last_output_file != content->file && strcmp (last_output_file, content->file) != 0))
        {
            const int max_lines_to_cover_with_new_line = 5u;
            if (last_output_line < content->line &&
                (int) content->line - (int) last_output_line < max_lines_to_cover_with_new_line)
            {
                int difference = (int) content->line - (int) last_output_line;
                while (difference)
                {
                    cushion_instance_output_null_terminated (instance, "\n");
                    --difference;
                }
            }
            else
            {
                cushion_instance_output_null_terminated (instance, "\n");
                cushion_instance_output_line_marker (instance, content->file, content->line);
            }

            last_output_file = content->file;
            last_output_line = content->line;
        }
        else if (previous_is_macro)
        {
            // Output guarding space to avoid merging macro-related tokens by mistake.
            cushion_instance_output_null_terminated (instance, " ");
        }

        switch (content->token.type)
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
        case CUSHION_TOKEN_TYPE_END_OF_FILE:
            // Not supported and error should've been printed during block lexing.
            assert (0u);
            break;

        case CUSHION_TOKEN_TYPE_IDENTIFIER:
            switch (content->token.identifier_kind)
            {
            case CUSHION_IDENTIFIER_KIND_FILE:
                cushion_instance_output_null_terminated (instance, "\"");
                cushion_instance_output_null_terminated (instance, content->file);
                cushion_instance_output_null_terminated (instance, "\"");
                break;

            case CUSHION_IDENTIFIER_KIND_LINE:
                cushion_instance_output_formatted (instance, "%u", (unsigned int) content->line);
                break;

            case CUSHION_IDENTIFIER_KIND_MACRO_PRAGMA:
                // Not supported and error should've been printed during block lexing.
                assert (0u);
                break;

            case CUSHION_IDENTIFIER_KIND_RETURN:
            case CUSHION_IDENTIFIER_KIND_BREAK:
            case CUSHION_IDENTIFIER_KIND_CONTINUE:
            case CUSHION_IDENTIFIER_KIND_GOTO:
                if (flags & OUTPUT_EXTENSION_INJECTOR_FLAGS_JUMP_FORBIDDEN)
                {
                    struct cushion_error_context_t error_context = {
                        .file = content->file,
                        .line = content->line,
                        .column = UINT_MAX,
                    };

                    cushion_instance_execution_error (
                        instance, error_context,
                        "Encountered return/break/continue/goto in context that forbids it due to possibility of "
                        "circular dependency in code generation. These keywords are always forbidden in CUSHION_DEFER "
                        "and also forbidden in CUSHION_STATEMENT_ACCUMULATOR_PUSH for accumulators that coexist with "
                        "defers.");
                }

                goto output_token;
                break;

            default:
                goto output_token;
                break;
            }

            break;

        case CUSHION_TOKEN_TYPE_PUNCTUATOR:
        case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:
        case CUSHION_TOKEN_TYPE_NUMBER_FLOATING:
        case CUSHION_TOKEN_TYPE_DIGIT_IDENTIFIER_SEQUENCE:
        case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
        case CUSHION_TOKEN_TYPE_STRING_LITERAL:
        case CUSHION_TOKEN_TYPE_GLUE:
        case CUSHION_TOKEN_TYPE_OTHER:
        output_token:
            cushion_instance_output_sequence (instance, content->token.begin, content->token.end);
            break;
        }

        previous_is_macro = content->flags & CUSHION_TOKEN_LIST_ITEM_FLAG_INJECTED_MACRO_REPLACEMENT;
        content = content->next;
    }
}

#    define GUARDRAIL_ACQUIRE(FEATURE, VALUE)                                                                          \
        assert (!state->tokenization.guardrail_##FEATURE);                                                             \
        state->tokenization.guardrail_##FEATURE = VALUE;                                                               \
        state->tokenization.guardrail_##FEATURE##_base = VALUE

#    define GUARDRAIL_EXTRACT(FEATURE, VALUE)                                                                          \
        ((VALUE) - (state->tokenization.guardrail_##FEATURE##_base - state->tokenization.guardrail_##FEATURE))

#    define GUARDRAIL_RELEASE(FEATURE)                                                                                 \
        assert (state->tokenization.guardrail_##FEATURE);                                                              \
        state->tokenization.guardrail_##FEATURE = NULL;                                                                \
        state->tokenization.guardrail_##FEATURE##_base = NULL

enum lex_defer_scope_t
{
    LEX_DEFER_SCOPE_GLOBAL = 0u,

    /// \details Understanding whether it is a function or some other block (initializer, struct, enum, union, etc)
    ///          requires kind-of-advanced understanding of language syntax and we'd like to avoid that to make this
    ///          feature kind-of-simple, Therefore, we treat anything non-global as a function right now: anyway
    ///          putting function-like code with return-break-continue-goto inside any other context than function
    ///          is a compiler error and we won't make it that much worse if we unwrap erroneous defers there.
    LEX_DEFER_SCOPE_MAYBE_FUNCTION,
};

enum lex_defer_statement_flags_t
{
    LEX_DEFER_STATEMENT_FLAG_NONE = 0u,
    LEX_DEFER_STATEMENT_FLAG_FIRST_TOKEN = 1u << 0u,

    LEX_DEFER_STATEMENT_FLAG_IF = 1u << 1u,
    LEX_DEFER_STATEMENT_FLAG_FOR = 1u << 2u,
    LEX_DEFER_STATEMENT_FLAG_WHILE = 1u << 3u,
    LEX_DEFER_STATEMENT_FLAG_DO = 1u << 4u,
    LEX_DEFER_STATEMENT_FLAG_SWITCH = 1u << 5u,

    LEX_DEFER_STATEMENT_FLAG_EXPECTING_LABEL = 1u << 6u,
    LEX_DEFER_STATEMENT_FLAG_LABELED = 1u << 7u,

    LEX_DEFER_STATEMENT_FLAG_HAS_JUMP = 1u << 8u,
    LEX_DEFER_STATEMENT_FLAG_PREVIOUS_IS_JUMP = 1u << 9u,

    LEX_DEFER_STATEMENT_FLAG_RECORDING_RETURN = 1u << 10u,
    LEX_DEFER_STATEMENT_FLAG_EXPECTING_GOTO_LABEL = 1u << 11u,
};

enum lex_defer_block_flags_t
{
    LEX_DEFER_BLOCK_FLAG_NONE = 0u,
    LEX_DEFER_BLOCK_FLAG_BREAK_CONTINUE_RECEIVER = 1u << 0u,
    LEX_DEFER_BLOCK_FLAG_SWITCH = 1u << 1u,
    LEX_DEFER_BLOCK_FLAG_HAS_LABELS = 1u << 2u,
};

enum lex_defer_event_type_t
{
    LEX_DEFER_EVENT_TYPE_BLOCK = 0u,
    LEX_DEFER_EVENT_TYPE_DEFER,
};

struct lex_defer_expression_t
{
    const char *source_file;
    unsigned int source_line;
    struct cushion_token_list_item_t *content_first;
};

struct lex_defer_event_t
{
    struct lex_defer_event_t *next;
    enum lex_defer_event_type_t type;
    union
    {
        struct lex_defer_block_state_t *child_block;
        struct lex_defer_expression_t defer;
    };
};

struct lex_defer_block_state_t
{
    struct lex_defer_block_state_t *owner;
    enum lex_defer_block_flags_t flags;
    unsigned int depth;

    /// \details Defers are intentionally ordered as LIFO, because this is how RAII should work.
    ///          Therefore, all events are also ordered as LIFO.
    struct lex_defer_event_t *events_first;
};

struct lex_defer_label_t
{
    struct lex_defer_label_t *next;
    struct lex_defer_block_state_t *owner;

    const char *name;
    unsigned int name_length;

    // We do not need any more information about the label for the defers:
    // only its block to properly process defers when using goto.
};

struct lex_defer_unresolved_goto_t
{
    struct lex_defer_unresolved_goto_t *next;
    const char *label_name;
    unsigned int label_name_length;

    struct lex_defer_block_state_t *from_block;
    struct lex_defer_event_t *from_block_since_event;
    struct cushion_deferred_output_node_t *defer_output_node;

    struct lexer_pop_token_meta_t name_meta;
    enum lex_defer_statement_flags_t statement_flags;
};

struct lex_defer_feature_state_t
{
    enum lex_defer_scope_t scope;
    enum lex_defer_statement_flags_t statement_flags;
    unsigned int statement_parenthesis_counter;

    const char *expected_label_name_begin;
    const char *expected_label_name_end;

    struct lex_defer_block_state_t root_block;
    struct lex_defer_block_state_t *current_block;

    struct lex_defer_label_t *labels_first;
    struct lex_defer_unresolved_goto_t *unresolved_goto_first;

    unsigned int return_value_counter;
    struct cushion_token_list_item_t *recording_return_expression_first;
    struct cushion_token_list_item_t *recording_return_expression_last;
    struct lexer_pop_token_meta_t recording_return_return_meta;
};

enum lex_defer_preprocess_result_t
{
    /// \brief Defer preprocess checked the token and didn't consume it.
    LEX_DEFER_PREPROCESS_RESULT_KEEP = 0u,

    /// \brief Defer preprocess has consumed the token, so it should not appear in code.
    LEX_DEFER_PREPROCESS_RESULT_CONSUMED,
};

/// \breif Fully cleans up statement flags.
static inline void lex_defer_fresh_statement (struct lex_defer_feature_state_t *defer)
{
    defer->statement_flags = LEX_DEFER_STATEMENT_FLAG_FIRST_TOKEN;
    defer->statement_parenthesis_counter = 0u;
}

/// \breif Updates statement flags with inheritance, used for consecutive statements in one block.
static inline void lex_defer_next_statement (struct lex_defer_feature_state_t *defer)
{
    enum lex_defer_statement_flags_t inherited_flags = LEX_DEFER_STATEMENT_FLAG_NONE;
    if (defer->statement_flags & LEX_DEFER_STATEMENT_FLAG_HAS_JUMP)
    {
        inherited_flags |= LEX_DEFER_STATEMENT_FLAG_PREVIOUS_IS_JUMP;
    }

    lex_defer_fresh_statement (defer);
    defer->statement_flags |= inherited_flags;
}

/// \breif Properly updates statement flags after label is detected and lexed.
static inline void lex_defer_statement_on_label (struct lex_defer_feature_state_t *defer)
{
    defer->statement_flags &= ~LEX_DEFER_STATEMENT_FLAG_EXPECTING_LABEL;
    defer->statement_flags |= LEX_DEFER_STATEMENT_FLAG_LABELED | LEX_DEFER_STATEMENT_FLAG_FIRST_TOKEN;
}

static enum lex_defer_preprocess_result_t lex_defer_preprocess_global (
    struct cushion_lexer_file_state_t *state,
    struct cushion_token_t *current_token,
    struct lexer_pop_token_meta_t *current_token_meta)
{
    switch (current_token->type)
    {
    case CUSHION_TOKEN_TYPE_PUNCTUATOR:
        switch (current_token->punctuator_kind)
        {
        case CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS:
            ++state->defer_feature->statement_parenthesis_counter;
            break;

        case CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS:
            if (state->defer_feature->statement_parenthesis_counter == 0u)
            {
                cushion_instance_lexer_error (
                    state, current_token_meta,
                    "Defer feature context analyzer encountered unbalanced parenthesis and is unable to continue.");
                return LEX_DEFER_PREPROCESS_RESULT_KEEP;
            }

            --state->defer_feature->statement_parenthesis_counter;
            break;

        case CUSHION_PUNCTUATOR_KIND_LEFT_CURLY_BRACE:
            state->defer_feature->scope = LEX_DEFER_SCOPE_MAYBE_FUNCTION;

            if (state->defer_feature->statement_parenthesis_counter != 0u)
            {
                cushion_instance_lexer_error (state, current_token_meta,
                                              "Defer feature context analyzer encountered unbalanced parenthesis at "
                                              "the beginning of the scope and is unable to continue.");
                return LEX_DEFER_PREPROCESS_RESULT_KEEP;
            }

            state->defer_feature->root_block.owner = NULL;
            state->defer_feature->root_block.flags = LEX_DEFER_BLOCK_FLAG_NONE;
            state->defer_feature->root_block.depth = 0u;
            state->defer_feature->root_block.events_first = NULL;

            state->defer_feature->current_block = &state->defer_feature->root_block;
            state->defer_feature->labels_first = NULL;
            state->defer_feature->unresolved_goto_first = NULL;

            state->defer_feature->return_value_counter = 0u;
            state->defer_feature->recording_return_expression_first = NULL;
            state->defer_feature->recording_return_expression_last = NULL;
            lex_defer_fresh_statement (state->defer_feature);
            break;

        case CUSHION_PUNCTUATOR_KIND_RIGHT_CURLY_BRACE:
            cushion_instance_lexer_error (
                state, current_token_meta,
                "Defer feature context analyzer encountered unbalanced curly braces and is unable to continue.");
            return LEX_DEFER_PREPROCESS_RESULT_KEEP;

        case CUSHION_PUNCTUATOR_KIND_SEMICOLON:
            lex_defer_fresh_statement (state->defer_feature);
            break;

        default:
            break;
        }

        break;

    default:
        break;
    }

    return LEX_DEFER_PREPROCESS_RESULT_KEEP;
}

static void lex_defer_generate_defers (struct cushion_lexer_file_state_t *state,
                                       const struct lexer_pop_token_meta_t *current_token_meta,
                                       struct lex_defer_block_state_t *up_to_block,
                                       struct lex_defer_block_state_t *from_block,
                                       // When delayed resolution is used, new events might be added in from block,
                                       // so we need this pointer to take care of it in that case.
                                       struct lex_defer_event_t *since_event_in_from_block,
                                       enum lex_defer_statement_flags_t statement_flags_at_generation)
{
    unsigned int any_defers = 0u;
    struct lex_defer_block_state_t *block = from_block;
    struct lex_defer_block_state_t *since_child_block = NULL;

    while (block != up_to_block)
    {
        // Up to block should either be a parent or NULL, so we cannot end up with NULL block here.
        assert (block);
        struct lex_defer_event_t *event = block == from_block ? since_event_in_from_block : block->events_first;
        unsigned int child_block_encountered = since_child_block ? 0u : 1u;

        while (event)
        {
            switch (event->type)
            {
            case LEX_DEFER_EVENT_TYPE_BLOCK:
                if (event->child_block == since_child_block)
                {
                    child_block_encountered = 1u;
                }

                break;

            case LEX_DEFER_EVENT_TYPE_DEFER:
                if (child_block_encountered && event->defer.content_first)
                {
                    if (!any_defers && (statement_flags_at_generation & LEX_DEFER_STATEMENT_FLAG_LABELED))
                    {
                        // If statement is labeled, put empty statement to prevent errors when declaration is labeled.
                        cushion_instance_output_null_terminated (state->instance, ";");
                    }

                    any_defers = 1u;
                    output_extension_injector_content (state->instance, event->defer.content_first,
                                                       OUTPUT_EXTENSION_INJECTOR_FLAGS_JUMP_FORBIDDEN);
                }

                break;
            }

            event = event->next;
        }

        since_child_block = block;
        block = block->owner;
    }

    if (any_defers)
    {
        // If trigger statement was actually inside inlined block, we cannot easily format that one,
        // so we print an error. It can be formatted properly, we do not bother right now as most
        // projects don't use inlined blocks for things that are as important as jumps.

        if (statement_flags_at_generation & (LEX_DEFER_STATEMENT_FLAG_IF | LEX_DEFER_STATEMENT_FLAG_FOR |
                                             LEX_DEFER_STATEMENT_FLAG_WHILE | LEX_DEFER_STATEMENT_FLAG_DO))
        {
            cushion_instance_lexer_error (state, current_token_meta,
                                          "Defer feature tried to generate defers in inlined block context (statement "
                                          "after if/for/while/do without {} block). It is not supported right now.");
            return;
        }

        // Restore line marker.
        cushion_instance_output_null_terminated (state->instance, "\n");
        cushion_instance_output_line_marker (state->instance, current_token_meta->file, current_token_meta->line);
    }
}

static struct lex_defer_block_state_t *lex_defer_resolve_break_continue_target_block (
    struct cushion_lexer_file_state_t *state, const struct lexer_pop_token_meta_t *current_token_meta)
{
    struct lex_defer_block_state_t *block = state->defer_feature->current_block;
    while (block)
    {
        if (block->flags & LEX_DEFER_BLOCK_FLAG_BREAK_CONTINUE_RECEIVER)
        {
            // Actual target block is receiver owner as break exists current loop and continue exits and
            // reenters loop again, therefore defers must be executed up to the owner block.
            return block->owner;
        }

        block = block->owner;
    }

    cushion_instance_lexer_error (
        state, current_token_meta,
        "Defer feature failed to find receiving scope for the break/continue jump command. Dangling break/continue?");
    return NULL;
}

static void lex_defer_replay_return (struct cushion_lexer_file_state_t *state,
                                     const struct lexer_pop_token_meta_t *semicolon_meta)
{
    unsigned int non_void = 0u;
    struct cushion_token_list_item_t *token_item = state->defer_feature->recording_return_expression_first;

    while (token_item)
    {
        if (token_item->token.type != CUSHION_TOKEN_TYPE_GLUE)
        {
            non_void = 1u;
            break;
        }

        token_item = token_item->next;
    }

    lex_update_line_mark (state, state->defer_feature->recording_return_return_meta.file,
                          state->defer_feature->recording_return_return_meta.line);

    if (state->defer_feature->statement_flags & LEX_DEFER_STATEMENT_FLAG_LABELED)
    {
        // If statement is labeled, put empty statement to prevent errors when declaration is labeled.
        cushion_instance_output_null_terminated (state->instance, ";");
        // Remove flag to avoid excessive ";" when generating defers.
        state->defer_feature->statement_flags &= ~LEX_DEFER_STATEMENT_FLAG_LABELED;
    }

    if (non_void)
    {
        cushion_instance_output_null_terminated (state->instance, "typeof (");
        token_item = state->defer_feature->recording_return_expression_first;

        while (token_item)
        {
            lex_update_line_mark (state, token_item->file, token_item->line);
            cushion_instance_output_sequence (state->instance, token_item->token.begin, token_item->token.end);
            token_item = token_item->next;
        }

        cushion_instance_output_formatted (state->instance, ") cushion_cached_return_value_%u = ",
                                           (unsigned int) state->defer_feature->return_value_counter);

        lex_update_line_mark (state, state->defer_feature->recording_return_return_meta.file,
                              state->defer_feature->recording_return_return_meta.line);

        token_item = state->defer_feature->recording_return_expression_first;
        while (token_item)
        {
            lex_update_line_mark (state, token_item->file, token_item->line);
            cushion_instance_output_sequence (state->instance, token_item->token.begin, token_item->token.end);
            token_item = token_item->next;
        }

        lex_update_line_mark (state, semicolon_meta->file, semicolon_meta->line);
        cushion_instance_output_null_terminated (state->instance, ";");
    }

    lex_defer_generate_defers (state, semicolon_meta, NULL, state->defer_feature->current_block,
                               state->defer_feature->current_block->events_first,
                               state->defer_feature->statement_flags);
    if (non_void)
    {
        cushion_instance_output_formatted (state->instance, "return cushion_cached_return_value_%u;",
                                           (unsigned int) state->defer_feature->return_value_counter);
    }
    else
    {
        cushion_instance_output_null_terminated (state->instance, "return;");
    }

    ++state->defer_feature->return_value_counter;
    state->defer_feature->recording_return_expression_first = NULL;
    state->defer_feature->recording_return_expression_last = NULL;
}

static void lex_defer_generate_goto_defers (struct cushion_lexer_file_state_t *state,
                                            struct lex_defer_label_t *to_label,
                                            struct lex_defer_block_state_t *from_block,
                                            struct lex_defer_event_t *from_block_since_event,
                                            struct lexer_pop_token_meta_t *label_name_meta,
                                            enum lex_defer_statement_flags_t goto_statement_flags)
{
    struct lex_defer_block_state_t *merge_block_target = to_label->owner;
    struct lex_defer_block_state_t *merge_block_source = from_block;

    // Make depth equal before searching for the actual merge block.
    while (merge_block_target->depth > merge_block_source->depth)
    {
        merge_block_target = merge_block_target->owner;
    }

    while (merge_block_source->depth > merge_block_target->depth)
    {
        merge_block_source = merge_block_source->owner;
    }

    // After depth is equal, we can search for the actual merge block.
    while (merge_block_target != merge_block_source)
    {
        merge_block_target = merge_block_target->owner;
        merge_block_source = merge_block_source->owner;
    }

    struct lex_defer_block_state_t *merge_block = merge_block_target;
    // Should never be possible as root block will always be merge block in the worst case.
    assert (merge_block);
    lex_defer_generate_defers (state, label_name_meta, merge_block, from_block, from_block_since_event,
                               goto_statement_flags);

    // Check that goto will not result in defer execution without proper initialization.
    struct lex_defer_block_state_t *target_block = to_label->owner;

    while (target_block != merge_block)
    {
        struct lex_defer_event_t *event = target_block->events_first;
        while (event)
        {
            if (event->type == LEX_DEFER_EVENT_TYPE_DEFER)
            {
                cushion_instance_execution_error (
                    state->instance,
                    (struct cushion_error_context_t) {
                        .file = event->defer.source_file,
                        .line = event->defer.source_line,
                        .column = UINT_MAX,
                    },
                    "One of the defers that makes goto jump impossible, see error at the bottom.");
            }

            event = event->next;
        }

        target_block = target_block->owner;
    }

    // Check that there is no defers in merge block between jumps (otherwise uninitialized defers are introduced).

    struct lex_defer_block_state_t *source_block_pre_merge;
    if (from_block != merge_block)
    {
        source_block_pre_merge = from_block;
        while (source_block_pre_merge->owner != merge_block)
        {
            source_block_pre_merge = source_block_pre_merge->owner;
        }
    }
    else
    {
        source_block_pre_merge = NULL;
    }

    struct lex_defer_block_state_t *target_block_pre_merge;
    if (to_label->owner != merge_block)
    {
        target_block_pre_merge = to_label->owner;
        while (target_block_pre_merge->owner != merge_block)
        {
            target_block_pre_merge = target_block_pre_merge->owner;
        }
    }
    else
    {
        target_block_pre_merge = NULL;
    }

    if (target_block_pre_merge && source_block_pre_merge)
    {
        unsigned int defers_in_merge_block_in_between_check_in_progress = 0u;
        struct lex_defer_event_t *event = merge_block->events_first;

        while (event)
        {
            switch (event->type)
            {
            case LEX_DEFER_EVENT_TYPE_BLOCK:
                if (event->child_block == source_block_pre_merge)
                {
                    // Gone to the child block, nothing more to check.
                    goto defers_in_merge_block_in_between_check_finished;
                }
                else if (event->child_block == target_block_pre_merge)
                {
                    defers_in_merge_block_in_between_check_in_progress = 1u;
                }

                break;

            case LEX_DEFER_EVENT_TYPE_DEFER:
                if (defers_in_merge_block_in_between_check_in_progress)
                {
                    cushion_instance_execution_error (
                        state->instance,
                        (struct cushion_error_context_t) {
                            .file = event->defer.source_file,
                            .line = event->defer.source_line,
                            .column = UINT_MAX,
                        },
                        "One of the defers that makes goto jump impossible, see error at the bottom.");
                }

                break;
            }

            event = event->next;
        }

    defers_in_merge_block_in_between_check_finished:;
    }

    if (cushion_instance_is_error_signaled (state->instance))
    {
        cushion_instance_lexer_error (
            state, label_name_meta,
            "Defer feature forbids this goto jump, because jump from here to target location will add new "
            "defers to the scope without calling initialization logic. It cannot be resolved on static "
            "analysis level without additional runtime logic as static analysis cannot secure goto jump flow "
            "for every case.");
    }
}

static enum lex_defer_preprocess_result_t lex_defer_preprocess_maybe_function (
    struct cushion_lexer_file_state_t *state,
    struct cushion_token_t *current_token,
    struct lexer_pop_token_meta_t *current_token_meta)
{
    // Filter out the things that we're never interested in.
    switch (current_token->type)
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
        // Should never get here.
        assert (0u);
        return LEX_DEFER_PREPROCESS_RESULT_KEEP;

    case CUSHION_TOKEN_TYPE_NEW_LINE:
    case CUSHION_TOKEN_TYPE_COMMENT:
    case CUSHION_TOKEN_TYPE_END_OF_FILE:
        // Not interested at all.
        return LEX_DEFER_PREPROCESS_RESULT_KEEP;

    case CUSHION_TOKEN_TYPE_GLUE:
        // Interested only when recording return.
        if (state->defer_feature->statement_flags & LEX_DEFER_STATEMENT_FLAG_RECORDING_RETURN)
        {
            break;
        }

        return LEX_DEFER_PREPROCESS_RESULT_KEEP;

    case CUSHION_TOKEN_TYPE_IDENTIFIER:
    case CUSHION_TOKEN_TYPE_PUNCTUATOR:
    case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:
    case CUSHION_TOKEN_TYPE_NUMBER_FLOATING:
    case CUSHION_TOKEN_TYPE_DIGIT_IDENTIFIER_SEQUENCE:
    case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
    case CUSHION_TOKEN_TYPE_STRING_LITERAL:
    case CUSHION_TOKEN_TYPE_OTHER:
        break;
    }

    // If recording return, do the recording and ignore everything else.
    if (state->defer_feature->statement_flags & LEX_DEFER_STATEMENT_FLAG_RECORDING_RETURN)
    {
        switch (current_token->type)
        {
        case CUSHION_TOKEN_TYPE_IDENTIFIER:
            switch (current_token->identifier_kind)
            {
            case CUSHION_IDENTIFIER_KIND_IF:
            case CUSHION_IDENTIFIER_KIND_FOR:
            case CUSHION_IDENTIFIER_KIND_WHILE:
            case CUSHION_IDENTIFIER_KIND_DO:
            case CUSHION_IDENTIFIER_KIND_SWITCH:
            case CUSHION_IDENTIFIER_KIND_RETURN:
            case CUSHION_IDENTIFIER_KIND_BREAK:
            case CUSHION_IDENTIFIER_KIND_CONTINUE:
            case CUSHION_IDENTIFIER_KIND_GOTO:
                cushion_instance_lexer_error (
                    state, current_token_meta,
                    "Defer feature encountered unexpected keyword while recording return statement.");
                return LEX_DEFER_PREPROCESS_RESULT_KEEP;

            default:
                break;
            }

            break;

        case CUSHION_TOKEN_TYPE_PUNCTUATOR:
            switch (current_token->punctuator_kind)
            {
            case CUSHION_PUNCTUATOR_KIND_SEMICOLON:
            {
                lex_defer_replay_return (state, current_token_meta);
                lex_defer_next_statement (state->defer_feature);
                return LEX_DEFER_PREPROCESS_RESULT_CONSUMED;
            }

            default:
                break;
            }

            break;

        default:
            // Nothing to check.
            break;
        }

        struct cushion_token_list_item_t *saved =
            cushion_save_token_to_memory (state->instance, current_token, CUSHION_ALLOCATION_CLASS_TRANSIENT);

        saved->file = current_token_meta->file;
        saved->line = current_token_meta->line;

        if (state->defer_feature->recording_return_expression_last)
        {
            state->defer_feature->recording_return_expression_last->next = saved;
        }
        else
        {
            state->defer_feature->recording_return_expression_first = saved;
        }

        state->defer_feature->recording_return_expression_last = saved;
        return LEX_DEFER_PREPROCESS_RESULT_CONSUMED;
    }

    // If expecting goto label, check it and ignore everything else.
    if (state->defer_feature->statement_flags & LEX_DEFER_STATEMENT_FLAG_EXPECTING_GOTO_LABEL)
    {
        state->defer_feature->statement_flags &= ~LEX_DEFER_STATEMENT_FLAG_EXPECTING_GOTO_LABEL;
        if (current_token->type != CUSHION_TOKEN_TYPE_IDENTIFIER ||
            current_token->identifier_kind != CUSHION_IDENTIFIER_KIND_REGULAR)
        {
            cushion_instance_lexer_error (
                state, current_token_meta,
                "Expected goto label name as regular identifier (not reserved keyword and not other token type).");
            return LEX_DEFER_PREPROCESS_RESULT_KEEP;
        }

        struct lex_defer_label_t *label = state->defer_feature->labels_first;
        const unsigned int length = (unsigned int) (current_token->end - current_token->begin);

        while (label)
        {
            if (label->name_length == length && strncmp (label->name, current_token->begin, label->name_length) == 0)
            {
                break;
            }

            label = label->next;
        }

        if (label)
        {
            lex_defer_generate_goto_defers (state, label, state->defer_feature->current_block,
                                            state->defer_feature->current_block->events_first, current_token_meta,
                                            state->defer_feature->statement_flags);
        }
        else
        {
            struct lex_defer_unresolved_goto_t *unresolved = cushion_allocator_allocate (
                &state->instance->allocator, sizeof (struct lex_defer_unresolved_goto_t),
                _Alignof (struct lex_defer_unresolved_goto_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);

            unresolved->label_name = cushion_instance_copy_char_sequence_inside (
                state->instance, current_token->begin, current_token->end, CUSHION_ALLOCATION_CLASS_TRANSIENT);
            unresolved->label_name_length = (unsigned int) (current_token->end - current_token->begin);

            unresolved->from_block = state->defer_feature->current_block;
            unresolved->from_block_since_event = state->defer_feature->current_block->events_first;
            unresolved->defer_output_node =
                cushion_output_add_deferred_sink (state->instance, current_token_meta->file, current_token_meta->line);

            unresolved->name_meta = *current_token_meta;
            unresolved->statement_flags = state->defer_feature->statement_flags;

            unresolved->next = state->defer_feature->unresolved_goto_first;
            state->defer_feature->unresolved_goto_first = unresolved;
        }

        // Reprint the consumed goto prepending the label name.
        cushion_instance_output_null_terminated (state->instance, "goto ");

        // Keep the actual label.
        return LEX_DEFER_PREPROCESS_RESULT_KEEP;
    }

    // Do the first token specific checks first (after separate return/goto routines).
    if (state->defer_feature->statement_flags & LEX_DEFER_STATEMENT_FLAG_FIRST_TOKEN)
    {
        state->defer_feature->statement_flags &= ~LEX_DEFER_STATEMENT_FLAG_FIRST_TOKEN;
        if (current_token->type == CUSHION_TOKEN_TYPE_IDENTIFIER &&
            current_token->identifier_kind == CUSHION_IDENTIFIER_KIND_REGULAR)
        {
            // Might be a label instead of statement. Be aware of that.
            GUARDRAIL_ACQUIRE (defer, current_token->begin);
            state->defer_feature->expected_label_name_begin = current_token->begin;
            state->defer_feature->expected_label_name_end = current_token->end;
            state->defer_feature->statement_flags |= LEX_DEFER_STATEMENT_FLAG_EXPECTING_LABEL;
            return LEX_DEFER_PREPROCESS_RESULT_KEEP;
        }
    }

    // If we were expecting that current sequence might be label declaration, check it now.
    if (state->defer_feature->statement_flags & LEX_DEFER_STATEMENT_FLAG_EXPECTING_LABEL)
    {
        state->defer_feature->statement_flags &= ~LEX_DEFER_STATEMENT_FLAG_EXPECTING_LABEL;
        if (current_token->type == CUSHION_TOKEN_TYPE_PUNCTUATOR &&
            current_token->punctuator_kind == CUSHION_PUNCTUATOR_KIND_COLON)
        {
            // Found label, process it accordingly.
            state->defer_feature->expected_label_name_begin =
                GUARDRAIL_EXTRACT (defer, state->defer_feature->expected_label_name_begin);

            state->defer_feature->expected_label_name_end =
                GUARDRAIL_EXTRACT (defer, state->defer_feature->expected_label_name_end);

            struct lex_defer_label_t *label =
                cushion_allocator_allocate (&state->instance->allocator, sizeof (struct lex_defer_label_t),
                                            _Alignof (struct lex_defer_label_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);

            label->owner = state->defer_feature->current_block;
            label->name = cushion_instance_copy_char_sequence_inside (
                state->instance, state->defer_feature->expected_label_name_begin,
                state->defer_feature->expected_label_name_end, CUSHION_ALLOCATION_CLASS_TRANSIENT);

            label->name_length = (unsigned int) (state->defer_feature->expected_label_name_end -
                                                 state->defer_feature->expected_label_name_begin);

            label->next = state->defer_feature->labels_first;
            state->defer_feature->labels_first = label;
            state->defer_feature->current_block->flags |= LEX_DEFER_BLOCK_FLAG_HAS_LABELS;

            state->defer_feature->expected_label_name_begin = NULL;
            state->defer_feature->expected_label_name_end = NULL;

            struct lex_defer_unresolved_goto_t *unresolved_goto = state->defer_feature->unresolved_goto_first;
            struct lex_defer_unresolved_goto_t *unresolved_previous = NULL;

            while (unresolved_goto)
            {
                if (unresolved_goto->label_name_length == label->name_length &&
                    strncmp (unresolved_goto->label_name, label->name, label->name_length) == 0)
                {
                    cushion_output_select_sink (state->instance, unresolved_goto->defer_output_node);
                    lex_defer_generate_goto_defers (state, label, unresolved_goto->from_block,
                                                    unresolved_goto->from_block_since_event,
                                                    &unresolved_goto->name_meta, unresolved_goto->statement_flags);

                    cushion_output_select_sink (state->instance, NULL);
                    cushion_output_finish_sink (state->instance, unresolved_goto->defer_output_node);

                    if (unresolved_previous)
                    {
                        unresolved_previous->next = unresolved_goto->next;
                    }
                    else
                    {
                        state->defer_feature->unresolved_goto_first = unresolved_goto->next;
                    }
                }
                else
                {
                    unresolved_previous = unresolved_goto;
                }

                unresolved_goto = unresolved_goto->next;
            }

            GUARDRAIL_RELEASE (defer);
            lex_defer_statement_on_label (state->defer_feature);
            return LEX_DEFER_PREPROCESS_RESULT_KEEP;
        }
        else
        {
            // Not a label, just release guardrail and continue looking.
            GUARDRAIL_RELEASE (defer);
        }
    }

    switch (current_token->type)
    {
    case CUSHION_TOKEN_TYPE_IDENTIFIER:
        switch (current_token->identifier_kind)
        {
#    define CHECK_NO_SWITCH                                                                                            \
        if (state->defer_feature->statement_flags & LEX_DEFER_STATEMENT_FLAG_SWITCH)                                   \
        {                                                                                                              \
            cushion_instance_lexer_error (                                                                             \
                state, current_token_meta,                                                                             \
                "Encountered other flow keyword (if/for/while/do) after \"switch\": \"switch\" is expected to be "     \
                "followed by code block.");                                                                            \
            return LEX_DEFER_PREPROCESS_RESULT_KEEP;                                                                   \
        }

        case CUSHION_IDENTIFIER_KIND_IF:
            CHECK_NO_SWITCH
            state->defer_feature->statement_flags |= LEX_DEFER_STATEMENT_FLAG_IF;
            break;

        case CUSHION_IDENTIFIER_KIND_FOR:
            CHECK_NO_SWITCH
            state->defer_feature->statement_flags |= LEX_DEFER_STATEMENT_FLAG_FOR;
            break;

        case CUSHION_IDENTIFIER_KIND_WHILE:
            CHECK_NO_SWITCH
            state->defer_feature->statement_flags |= LEX_DEFER_STATEMENT_FLAG_WHILE;
            break;

        case CUSHION_IDENTIFIER_KIND_DO:
            CHECK_NO_SWITCH
            state->defer_feature->statement_flags |= LEX_DEFER_STATEMENT_FLAG_DO;
            break;
#    undef CHECK_NO_SWITCH

        case CUSHION_IDENTIFIER_KIND_SWITCH:
            state->defer_feature->statement_flags |= LEX_DEFER_STATEMENT_FLAG_SWITCH;
            break;

#    define CHECK_HAS_NO_JUMP                                                                                          \
        if (state->defer_feature->statement_flags & LEX_DEFER_STATEMENT_FLAG_HAS_JUMP)                                 \
        {                                                                                                              \
            cushion_instance_lexer_error (                                                                             \
                state, current_token_meta,                                                                             \
                "Defer feature encountered several jump instructions (return/break/continue/goto) in one "             \
                "statement, which is an error in itself.");                                                            \
            return LEX_DEFER_PREPROCESS_RESULT_KEEP;                                                                   \
        }

        case CUSHION_IDENTIFIER_KIND_RETURN:
        {
            CHECK_HAS_NO_JUMP
            state->defer_feature->statement_flags |= LEX_DEFER_STATEMENT_FLAG_HAS_JUMP;

            unsigned int any_defers = 0u;
            struct lex_defer_block_state_t *block = state->defer_feature->current_block;

            while (block)
            {
                struct lex_defer_event_t *event = block->events_first;
                while (event)
                {
                    if (event->type == LEX_DEFER_EVENT_TYPE_DEFER)
                    {
                        any_defers = 1u;
                        goto return_has_defers_check_finished;
                    }

                    event = event->next;
                }

                block = block->owner;
            }

        return_has_defers_check_finished:
            if (!any_defers)
            {
                // No defers, no need for return recording logic, can just mark jump and lex as regular statement.
                return LEX_DEFER_PREPROCESS_RESULT_KEEP;
            }

            // Start recording return for proper calculation and defer order.
            state->defer_feature->statement_flags |= LEX_DEFER_STATEMENT_FLAG_RECORDING_RETURN;
            state->defer_feature->recording_return_expression_first = NULL;
            state->defer_feature->recording_return_expression_last = NULL;
            state->defer_feature->recording_return_return_meta = *current_token_meta;
            return LEX_DEFER_PREPROCESS_RESULT_CONSUMED;
        }

        case CUSHION_IDENTIFIER_KIND_BREAK:
        case CUSHION_IDENTIFIER_KIND_CONTINUE:
        {
            CHECK_HAS_NO_JUMP
            state->defer_feature->statement_flags |= LEX_DEFER_STATEMENT_FLAG_HAS_JUMP;

            struct lex_defer_block_state_t *target_block =
                lex_defer_resolve_break_continue_target_block (state, current_token_meta);

            LEX_WHEN_ERROR (return LEX_DEFER_PREPROCESS_RESULT_KEEP)
            lex_defer_generate_defers (state, current_token_meta, target_block, state->defer_feature->current_block,
                                       state->defer_feature->current_block->events_first,
                                       state->defer_feature->statement_flags);
            break;
        }

        case CUSHION_IDENTIFIER_KIND_GOTO:
        {
            CHECK_HAS_NO_JUMP
            state->defer_feature->statement_flags |=
                LEX_DEFER_STATEMENT_FLAG_HAS_JUMP | LEX_DEFER_STATEMENT_FLAG_EXPECTING_GOTO_LABEL;
            return LEX_DEFER_PREPROCESS_RESULT_CONSUMED;
        }

#    undef CHECK_HAS_NO_JUMP

        default:
            break;
        }

        break;

    case CUSHION_TOKEN_TYPE_PUNCTUATOR:
        switch (current_token->punctuator_kind)
        {
        case CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS:
            ++state->defer_feature->statement_parenthesis_counter;
            break;

        case CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS:
            if (state->defer_feature->statement_parenthesis_counter == 0u)
            {
                cushion_instance_lexer_error (
                    state, current_token_meta,
                    "Defer feature context analyzer encountered unbalanced parenthesis and is unable to continue.");
                return LEX_DEFER_PREPROCESS_RESULT_KEEP;
            }

            --state->defer_feature->statement_parenthesis_counter;
            break;

        case CUSHION_PUNCTUATOR_KIND_LEFT_CURLY_BRACE:
        {
            if (state->defer_feature->statement_parenthesis_counter > 0u)
            {
                // Brace inside parenthesis -- usual case for initializers.
                break;
            }

            struct lex_defer_block_state_t *new_block = cushion_allocator_allocate (
                &state->instance->allocator, sizeof (struct lex_defer_block_state_t),
                _Alignof (struct lex_defer_block_state_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);

            new_block->flags = LEX_DEFER_BLOCK_FLAG_NONE;
            new_block->events_first = NULL;

            if (state->defer_feature->statement_flags & (LEX_DEFER_STATEMENT_FLAG_FOR | LEX_DEFER_STATEMENT_FLAG_WHILE |
                                                         LEX_DEFER_STATEMENT_FLAG_DO | LEX_DEFER_STATEMENT_FLAG_SWITCH))
            {
                new_block->flags |= LEX_DEFER_BLOCK_FLAG_BREAK_CONTINUE_RECEIVER;
            }

            if (state->defer_feature->statement_flags & LEX_DEFER_STATEMENT_FLAG_SWITCH)
            {
                new_block->flags |= LEX_DEFER_BLOCK_FLAG_SWITCH;
            }

            new_block->owner = state->defer_feature->current_block;
            new_block->depth = state->defer_feature->current_block->depth + 1u;

            struct lex_defer_event_t *block_event =
                cushion_allocator_allocate (&state->instance->allocator, sizeof (struct lex_defer_event_t),
                                            _Alignof (struct lex_defer_event_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);

            block_event->type = LEX_DEFER_EVENT_TYPE_BLOCK;
            block_event->child_block = new_block;
            block_event->next = state->defer_feature->current_block->events_first;
            state->defer_feature->current_block->events_first = block_event;

            state->defer_feature->current_block = new_block;
            lex_defer_fresh_statement (state->defer_feature);
            break;
        }

        case CUSHION_PUNCTUATOR_KIND_RIGHT_CURLY_BRACE:
        {
            if (state->defer_feature->statement_parenthesis_counter > 0u)
            {
                // Brace inside parenthesis -- usual case for initializers.
                break;
            }

            struct lex_defer_block_state_t *owner_block = state->defer_feature->current_block->owner;
            if ((state->defer_feature->statement_flags & LEX_DEFER_STATEMENT_FLAG_PREVIOUS_IS_JUMP) == 0u)
            {
                lex_defer_generate_defers (state, current_token_meta, owner_block, state->defer_feature->current_block,
                                           state->defer_feature->current_block->events_first,
                                           state->defer_feature->statement_flags);
            }

            lex_defer_fresh_statement (state->defer_feature);
            state->defer_feature->current_block = owner_block;

            if (!owner_block)
            {
                // Exiting from function entirely.
                if (state->defer_feature->unresolved_goto_first)
                {
                    struct lex_defer_unresolved_goto_t *unresolved = state->defer_feature->unresolved_goto_first;
                    while (unresolved)
                    {
                        cushion_instance_lexer_error (state, &unresolved->name_meta,
                                                      "Defer feature detected function end, but this goto jump target "
                                                      "is still not found. Perhaps, there is an error in label name?");

                        unresolved = unresolved->next;
                    }
                }

                state->defer_feature->scope = LEX_DEFER_SCOPE_GLOBAL;
            }

            break;
        }

        case CUSHION_PUNCTUATOR_KIND_SEMICOLON:
            // Inside parenthesis it might just be "for" statement, so we just ignore that.
            if (state->defer_feature->statement_parenthesis_counter == 0u)
            {
                // "return"s are processed separately through different routines.
                lex_defer_next_statement (state->defer_feature);
            }

            break;

        default:
            break;
        }

        break;

    default:
        // Nothing specific for us.
        break;
    }

    return LEX_DEFER_PREPROCESS_RESULT_KEEP;
}

static enum lex_defer_preprocess_result_t lex_defer_preprocess (struct cushion_lexer_file_state_t *state,
                                                                struct cushion_token_t *current_token,
                                                                struct lexer_pop_token_meta_t *current_token_meta)
{
    switch (state->defer_feature->scope)
    {
    case LEX_DEFER_SCOPE_GLOBAL:
        return lex_defer_preprocess_global (state, current_token, current_token_meta);

    case LEX_DEFER_SCOPE_MAYBE_FUNCTION:
        return lex_defer_preprocess_maybe_function (state, current_token, current_token_meta);
    }

    // Memory corruption if scope didn't match.
    assert (0);
    return LEX_DEFER_PREPROCESS_RESULT_KEEP;
}

static void lex_code_defer (struct cushion_lexer_file_state_t *state,
                            const struct lexer_pop_token_meta_t *defer_token_meta)
{
    // Can only fail if we're inside scan only context (and scan only context should not process regular code anyway)
    // or defer feature is disabled (should be checked prior to calling this function).
    assert (state->defer_feature);

    if (state->defer_feature->scope != LEX_DEFER_SCOPE_MAYBE_FUNCTION)
    {
        cushion_instance_lexer_error (state, defer_token_meta,
                                      "Encountered CUSHION_DEFER outside of function context.");
        return;
    }

    if (state->defer_feature->current_block->flags & LEX_DEFER_BLOCK_FLAG_SWITCH)
    {
        cushion_instance_lexer_error (state, defer_token_meta,
                                      "Encountered CUSHION_DEFER inside raw switch block. As switch is built on case "
                                      "labels, we cannot properly track defers inside. If you need defer inside "
                                      "switch, use block after case: \"case VALUE: { ... code ... }\".");
        return;
    }

    if ((state->defer_feature->current_block->flags & LEX_DEFER_BLOCK_FLAG_HAS_LABELS) ||
        (state->defer_feature->statement_flags & LEX_DEFER_STATEMENT_FLAG_LABELED))
    {
        cushion_instance_lexer_error (
            state, defer_token_meta,
            "Encountered CUSHION_DEFER in a block that already has labels inside. All defers should be declared before "
            "any label inside that block, otherwise it is impossible to properly control which defer should be called "
            "and which should not.");
        return;
    }

    if (state->defer_feature->statement_flags &
        (LEX_DEFER_STATEMENT_FLAG_IF | LEX_DEFER_STATEMENT_FLAG_FOR | LEX_DEFER_STATEMENT_FLAG_WHILE |
         LEX_DEFER_STATEMENT_FLAG_DO | LEX_DEFER_STATEMENT_FLAG_SWITCH))
    {
        cushion_instance_lexer_error (
            state, defer_token_meta,
            "Encountered CUSHION_DEFER in inline block (if/for/while/do that loops one statement instead of a block). "
            "This is not supported as it makes code generation more difficult and is not a good use case either way.");
        return;
    }

    const char *file = state->last_marked_file;
    const unsigned int line = state->last_token_line;

    // Defers only work in file context, not persistent context, therefore using last marked file is okay.
    struct cushion_token_list_item_t *content = lex_extension_injector_content (state, file);
    LEX_WHEN_ERROR (return)

    struct lex_defer_event_t *defer =
        cushion_allocator_allocate (&state->instance->allocator, sizeof (struct lex_defer_event_t),
                                    _Alignof (struct lex_defer_event_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);

    defer->type = LEX_DEFER_EVENT_TYPE_DEFER;
    defer->defer.source_file = file;
    defer->defer.source_line = line;
    defer->defer.content_first = content;

    defer->next = state->defer_feature->current_block->events_first;
    state->defer_feature->current_block->events_first = defer;
}

static struct cushion_statement_accumulator_t *find_statement_accumulator (struct cushion_instance_t *instance,
                                                                           const char *begin,
                                                                           const char *end)
{
    const unsigned int length = (unsigned int) (end - begin);
    struct cushion_statement_accumulator_t *accumulator = instance->statement_accumulators_first;

    while (accumulator)
    {
        if (accumulator->name_length == length && strncmp (accumulator->name, begin, length) == 0)
        {
            return accumulator;
        }

        accumulator = accumulator->next;
    }

    return NULL;
}

static struct cushion_statement_accumulator_ref_t *find_statement_accumulator_ref (struct cushion_instance_t *instance,
                                                                                   const char *begin,
                                                                                   const char *end)
{
    const unsigned int length = (unsigned int) (end - begin);
    struct cushion_statement_accumulator_ref_t *ref = instance->statement_accumulator_refs_first;

    while (ref)
    {
        if (ref->name_length == length && strncmp (ref->name, begin, length) == 0)
        {
            return ref;
        }

        ref = ref->next;
    }

    return NULL;
}

static unsigned int lex_is_push_macro_list_equal (struct cushion_token_list_item_t *first_list,
                                                  struct cushion_token_list_item_t *second_list)
{
    while (first_list && second_list)
    {
        if (first_list->token.type != second_list->token.type ||
            (first_list->token.end - first_list->token.begin) != (second_list->token.end - second_list->token.begin) ||
            strncmp (first_list->token.begin, second_list->token.begin,
                     first_list->token.end - first_list->token.begin) != 0)
        {
            return 0u;
        }

        first_list = first_list->next;
        second_list = second_list->next;
    }

    // Only returns true if both are NULL.
    return first_list == second_list;
}

static inline unsigned int statement_accumulator_has_equal_entry (struct cushion_statement_accumulator_t *accumulator,
                                                                  struct cushion_token_list_item_t *content)
{
    struct cushion_statement_accumulator_entry_t *other_entry = accumulator->entries_first;
    while (other_entry)
    {
        if (lex_is_push_macro_list_equal (content, other_entry->content_first))
        {
            return 1u;
        }

        other_entry = other_entry->next;
    }

    return 0u;
}

static void check_statement_accumulator_unordered_pushes (struct cushion_lexer_file_state_t *state,
                                                          struct cushion_statement_accumulator_t *accumulator,
                                                          const char *under_name,
                                                          unsigned int under_name_length)
{
    struct cushion_statement_accumulator_unordered_push_t *unordered_push =
        state->instance->statement_unordered_push_first;
    struct cushion_statement_accumulator_unordered_push_t *previous = NULL;

    while (unordered_push)
    {
        if (unordered_push->name_length == under_name_length &&
            strncmp (unordered_push->name, under_name, unordered_push->name_length) == 0)
        {
            if ((unordered_push->flags & CUSHION_STATEMENT_ACCUMULATOR_PUSH_FLAG_UNIQUE) == 0u ||
                !statement_accumulator_has_equal_entry (accumulator, unordered_push->entry_template.content_first))
            {
                struct cushion_statement_accumulator_entry_t *entry = cushion_allocator_allocate (
                    &state->instance->allocator, sizeof (struct cushion_statement_accumulator_entry_t),
                    _Alignof (struct cushion_statement_accumulator_entry_t), CUSHION_ALLOCATION_CLASS_PERSISTENT);

                *entry = unordered_push->entry_template;
                if (accumulator->entries_last)
                {
                    accumulator->entries_last->next = entry;
                }
                else
                {
                    accumulator->entries_first = entry;
                }

                accumulator->entries_last = entry;
            }

            if (previous)
            {
                previous->next = unordered_push->next;
            }
            else
            {
                state->instance->statement_unordered_push_first = unordered_push->next;
            }

            if (unordered_push == state->instance->statement_unordered_push_last)
            {
                state->instance->statement_unordered_push_last = previous;
            }
        }
        else
        {
            previous = unordered_push;
        }

        unordered_push = unordered_push->next;
    }
}

static void lex_code_statement_accumulator (struct cushion_lexer_file_state_t *state)
{
    const unsigned int start_line = state->last_token_line;
    struct cushion_token_t current_token;
    struct lexer_pop_token_meta_t current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS)
    {
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Expected \"(\" after CUSHION_STATEMENT_ACCUMULATOR.");
        return;
    }

    current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    // Remark: macro unwraps are intentionally not supported for accumulator and ref names as it would only make
    // accumulator management more complex for the user.

    if (current_token.type != CUSHION_TOKEN_TYPE_IDENTIFIER)
    {
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Expected identifier as argument for CUSHION_STATEMENT_ACCUMULATOR.");
        return;
    }

    if (find_statement_accumulator (state->instance, current_token.begin, current_token.end) ||
        find_statement_accumulator_ref (state->instance, current_token.begin, current_token.end))
    {
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Unable to create statement accumulator with name as it is already used "
                                      "for other accumulator or reference.");
        return;
    }

    lex_update_line_mark (state, current_token_meta.file, start_line);
    struct cushion_statement_accumulator_t *accumulator = cushion_allocator_allocate (
        &state->instance->allocator, sizeof (struct cushion_statement_accumulator_t),
        _Alignof (struct cushion_statement_accumulator_t), CUSHION_ALLOCATION_CLASS_PERSISTENT);

    accumulator->flags = CUSHION_STATEMENT_ACCUMULATOR_FLAG_NONE;
    accumulator->name = cushion_instance_copy_char_sequence_inside (
        state->instance, current_token.begin, current_token.end, CUSHION_ALLOCATION_CLASS_PERSISTENT);
    accumulator->name_length = (unsigned int) (current_token.end - current_token.begin);

    accumulator->entries_first = NULL;
    accumulator->entries_last = NULL;
    accumulator->output_node = cushion_output_add_deferred_sink (state->instance, state->last_marked_file, start_line);

    accumulator->next = state->instance->statement_accumulators_first;
    state->instance->statement_accumulators_first = accumulator;

    if (state->defer_feature && state->defer_feature->scope == LEX_DEFER_SCOPE_MAYBE_FUNCTION)
    {
        // Mark jump as forbidden if there is already any defers in the hierarchy.
        struct lex_defer_block_state_t *block = state->defer_feature->current_block;

        while (block)
        {
            struct lex_defer_event_t *event = block->events_first;
            while (event)
            {
                if (event->type == LEX_DEFER_EVENT_TYPE_DEFER)
                {
                    accumulator->flags |= CUSHION_STATEMENT_ACCUMULATOR_FLAG_JUMP_FORBIDDEN;
                    goto defer_detection_finished;
                }

                event = event->next;
            }

            block = block->owner;
        }

    defer_detection_finished:;
    }

    check_statement_accumulator_unordered_pushes (state, accumulator, accumulator->name, accumulator->name_length);
    current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS)
    {
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Expected \")\" after CUSHION_STATEMENT_ACCUMULATOR argument.");
        return;
    }
}

static void lex_code_statement_accumulator_push (struct cushion_lexer_file_state_t *state)
{
    const char *file = state->last_marked_file;
    const unsigned int line = state->last_token_line;

    struct cushion_token_t current_token;
    struct lexer_pop_token_meta_t current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS)
    {
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Expected \"(\" after CUSHION_STATEMENT_ACCUMULATOR_PUSH.");
        return;
    }

    current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    // Remark: macro unwraps are intentionally not supported for accumulator and ref names as it would only make
    // accumulator management more complex for the user.

    if (current_token.type != CUSHION_TOKEN_TYPE_IDENTIFIER)
    {
        cushion_instance_lexer_error (
            state, &current_token_meta,
            "Expected accumulator name identifier as argument for CUSHION_STATEMENT_ACCUMULATOR_PUSH.");
        return;
    }

    // Use guardrail to preserve accumulator or ref identifier.
    GUARDRAIL_ACQUIRE (statement_accumulator, current_token.begin);
    const char *name_begin = current_token.begin;
    const char *name_end = current_token.end;
    enum cushion_statement_accumulator_push_flags_t flags = CUSHION_STATEMENT_ACCUMULATOR_PUSH_FLAG_NONE;

    while (lexer_file_state_should_continue (state))
    {
        current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
        LEX_WHEN_ERROR (return)

        if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
            (current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_COMMA &&
             current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS))
        {
            cushion_instance_lexer_error (
                state, &current_token_meta,
                "Expected \",\" or \")\" after argument in CUSHION_STATEMENT_ACCUMULATOR_PUSH.");
            return;
        }

        if (current_token.punctuator_kind == CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS)
        {
            // No more flags.
            break;
        }

        current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
        LEX_WHEN_ERROR (return)

        // Remark: macro unwraps are intentionally not supported for accumulator push flags as it would only make
        // accumulator management more complex for the user.

        if (current_token.type != CUSHION_TOKEN_TYPE_IDENTIFIER)
        {
            cushion_instance_lexer_error (
                state, &current_token_meta,
                "Expected accumulator name identifier as argument for CUSHION_STATEMENT_ACCUMULATOR.");
            return;
        }

        const unsigned int length = (unsigned int) (current_token.end - current_token.begin);

#    define CHECK_FLAG(LITERAL, VALUE)                                                                                 \
        if (length == sizeof (LITERAL) - 1u && strncmp (current_token.begin, LITERAL, length) == 0)                    \
        {                                                                                                              \
            if (flags & VALUE)                                                                                         \
            {                                                                                                          \
                cushion_instance_lexer_error (state, &current_token_meta,                                              \
                                              "Flag \"%s\" of CUSHION_STATEMENT_ACCUMULATOR_PUSH is repeated twice."); \
                return;                                                                                                \
            }                                                                                                          \
                                                                                                                       \
            flags |= VALUE;                                                                                            \
        }                                                                                                              \
        else

        CHECK_FLAG ("unique", CUSHION_STATEMENT_ACCUMULATOR_PUSH_FLAG_UNIQUE)
        CHECK_FLAG ("optional", CUSHION_STATEMENT_ACCUMULATOR_PUSH_FLAG_OPTIONAL)
        CHECK_FLAG ("unordered", CUSHION_STATEMENT_ACCUMULATOR_PUSH_FLAG_UNORDERED)
        {
            // Copy flag name in order to properly pass it to error format.
            const char *flag_name = cushion_instance_copy_char_sequence_inside (
                state->instance, current_token.begin, current_token.end, CUSHION_ALLOCATION_CLASS_TRANSIENT);

            cushion_instance_lexer_error (state, &current_token_meta,
                                          "Got unknown flag \"%s\" in CUSHION_STATEMENT_ACCUMULATOR.", flag_name);
            return;
        }
#    undef CHECK_FLAG
    }

    LEX_WHEN_ERROR (return)
    name_begin = GUARDRAIL_EXTRACT (statement_accumulator, name_begin);
    name_end = GUARDRAIL_EXTRACT (statement_accumulator, name_end);

    struct cushion_statement_accumulator_t *accumulator =
        find_statement_accumulator (state->instance, name_begin, name_end);

    if (!accumulator)
    {
        struct cushion_statement_accumulator_ref_t *ref =
            find_statement_accumulator_ref (state->instance, name_begin, name_end);

        if (ref)
        {
            accumulator = ref->accumulator;
        }
    }

    const char *saved_accumulator_name = NULL;
    unsigned int saved_accumulator_name_length = 0u;

    if (!accumulator)
    {
        if (flags & CUSHION_STATEMENT_ACCUMULATOR_PUSH_FLAG_UNORDERED)
        {
            // Only makes sense to save name if there is no accumulator in sight and push is unordered.
            saved_accumulator_name = cushion_instance_copy_char_sequence_inside (state->instance, name_begin, name_end,
                                                                                 CUSHION_ALLOCATION_CLASS_PERSISTENT);
            saved_accumulator_name_length = (unsigned int) (name_end - name_begin);
        }
        else if ((flags & CUSHION_STATEMENT_ACCUMULATOR_PUSH_FLAG_OPTIONAL) == 0u)
        {
            const char *name_for_log = cushion_instance_copy_char_sequence_inside (
                state->instance, name_begin, name_end, CUSHION_ALLOCATION_CLASS_TRANSIENT);

            cushion_instance_lexer_error (
                state, &current_token_meta,
                "Unable to find accumulator or reference \"%s\" for "
                "CUSHION_STATEMENT_ACCUMULATOR_PUSH and push is neither optional nor unordered.",
                name_for_log);
            return;
        }
    }

    GUARDRAIL_RELEASE (statement_accumulator);
    const char *saved_file =
        cushion_instance_copy_null_terminated_inside (state->instance, file, CUSHION_ALLOCATION_CLASS_PERSISTENT);
    struct cushion_token_list_item_t *content = lex_extension_injector_content (state, saved_file);
    LEX_WHEN_ERROR (return)

    if (accumulator)
    {
        if ((flags & CUSHION_STATEMENT_ACCUMULATOR_PUSH_FLAG_UNIQUE) == 0u ||
            !statement_accumulator_has_equal_entry (accumulator, content))
        {
            struct cushion_statement_accumulator_entry_t *entry = cushion_allocator_allocate (
                &state->instance->allocator, sizeof (struct cushion_statement_accumulator_entry_t),
                _Alignof (struct cushion_statement_accumulator_entry_t), CUSHION_ALLOCATION_CLASS_PERSISTENT);

            entry->next = NULL;
            entry->source_file = saved_file;
            entry->source_line = line;
            entry->content_first = content;

            if (accumulator->entries_last)
            {
                accumulator->entries_last->next = entry;
            }
            else
            {
                accumulator->entries_first = entry;
            }

            accumulator->entries_last = entry;
        }
    }
    else
    {
        struct cushion_statement_accumulator_unordered_push_t *push = cushion_allocator_allocate (
            &state->instance->allocator, sizeof (struct cushion_statement_accumulator_unordered_push_t),
            _Alignof (struct cushion_statement_accumulator_unordered_push_t), CUSHION_ALLOCATION_CLASS_PERSISTENT);

        push->name = saved_accumulator_name;
        push->name_length = saved_accumulator_name_length;
        push->flags = flags;

        push->entry_template.next = NULL;
        push->entry_template.source_file =
            cushion_instance_copy_null_terminated_inside (state->instance, file, CUSHION_ALLOCATION_CLASS_PERSISTENT);
        push->entry_template.source_line = line;
        push->entry_template.content_first = content;

        if (state->instance->statement_unordered_push_last)
        {
            state->instance->statement_unordered_push_last->next = push;
        }
        else
        {
            state->instance->statement_unordered_push_first = push;
        }

        push->next = NULL;
        state->instance->statement_unordered_push_last = push;
    }
}

static void lex_code_statement_accumulator_ref (struct cushion_lexer_file_state_t *state)
{
    struct cushion_token_t current_token;
    struct lexer_pop_token_meta_t current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS)
    {
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Expected \"(\" after CUSHION_STATEMENT_ACCUMULATOR_REF.");
        return;
    }

    current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_IDENTIFIER)
    {
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Expected identifier as first argument for CUSHION_STATEMENT_ACCUMULATOR_REF.");
        return;
    }

    const unsigned int length = (unsigned int) (current_token.end - current_token.begin);
    struct cushion_statement_accumulator_ref_t *ref = state->instance->statement_accumulator_refs_first;

    while (ref)
    {
        if (ref->name_length == length && strncmp (ref->name, current_token.begin, length) == 0)
        {
            break;
        }

        ref = ref->next;
    }

    if (!ref)
    {
        ref = cushion_allocator_allocate (
            &state->instance->allocator, sizeof (struct cushion_statement_accumulator_ref_t),
            _Alignof (struct cushion_statement_accumulator_ref_t), CUSHION_ALLOCATION_CLASS_PERSISTENT);

        ref->name = cushion_instance_copy_char_sequence_inside (state->instance, current_token.begin, current_token.end,
                                                                CUSHION_ALLOCATION_CLASS_PERSISTENT);
        ref->name_length = (unsigned int) (current_token.end - current_token.begin);
        ref->accumulator = NULL;

        ref->next = state->instance->statement_accumulator_refs_first;
        state->instance->statement_accumulator_refs_first = ref;
    }

    current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_COMMA)
    {
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Expected \",\" after first argument of CUSHION_STATEMENT_ACCUMULATOR_REF.");
        return;
    }

    current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_IDENTIFIER)
    {
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Expected identifier as second argument for CUSHION_STATEMENT_ACCUMULATOR_REF.");
        return;
    }

    ref->accumulator = find_statement_accumulator (state->instance, current_token.begin, current_token.end);
    if (!ref->accumulator)
    {
        cushion_instance_lexer_error (
            state, &current_token_meta,
            "Cannot find accumulator reference as second argument CUSHION_STATEMENT_ACCUMULATOR_REF. Only real "
            "accumulators, not other references, are supported, because introducing reference support here might "
            "introduce hard to track issues in user code.");
        return;
    }

    check_statement_accumulator_unordered_pushes (state, ref->accumulator, ref->name, ref->name_length);
    current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS)
    {
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Expected \")\" after CUSHION_STATEMENT_ACCUMULATOR_REF arguments.");
        return;
    }
}

static void lex_code_statement_accumulator_unref (struct cushion_lexer_file_state_t *state)
{
    struct cushion_token_t current_token;
    struct lexer_pop_token_meta_t current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS)
    {
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Expected \"(\" after CUSHION_STATEMENT_ACCUMULATOR_UNREF.");
        return;
    }

    current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_IDENTIFIER)
    {
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Expected identifier as argument for CUSHION_STATEMENT_ACCUMULATOR_UNREF.");
        return;
    }

    const unsigned int length = (unsigned int) (current_token.end - current_token.begin);
    struct cushion_statement_accumulator_ref_t *ref = state->instance->statement_accumulator_refs_first;
    struct cushion_statement_accumulator_ref_t *ref_previous = NULL;

    while (ref)
    {
        if (ref->name_length == length && strncmp (ref->name, current_token.begin, length) == 0)
        {
            break;
        }

        ref_previous = ref;
        ref = ref->next;
    }

    // If ref is not found, it is technically not a mistake, it just means that someone tried to unbind reference
    // that was not yet bound, which is not an error in itself, more of a "cleanup not needed" event.
    if (ref)
    {
        // Just remove reference from the list.
        if (ref_previous)
        {
            ref_previous->next = ref->next;
        }
        else
        {
            state->instance->statement_accumulator_refs_first = ref->next;
        }
    }

    current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS)
    {
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Expected \")\" after CUSHION_STATEMENT_ACCUMULATOR_UNREF argument.");
        return;
    }
}

static void lex_code_snippet (struct cushion_lexer_file_state_t *state)
{
    struct cushion_token_t current_token;
    struct lexer_pop_token_meta_t current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    struct lexer_pop_token_meta_t first_token_meta = current_token_meta;
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS)
    {
        cushion_instance_lexer_error (state, &current_token_meta, "Expected \"(\" after CUSHION_SNIPPET.");
        return;
    }

    current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_IDENTIFIER ||
        current_token.identifier_kind != CUSHION_IDENTIFIER_KIND_REGULAR)
    {
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Expected non-keyword identifier for CUSHION_SNIPPET as first argument.");
        return;
    }

    struct cushion_macro_node_t *node =
        cushion_allocator_allocate (&state->instance->allocator, sizeof (struct cushion_macro_node_t),
                                    _Alignof (struct cushion_macro_node_t), CUSHION_ALLOCATION_CLASS_PERSISTENT);

    node->name = cushion_instance_copy_char_sequence_inside (state->instance, current_token.begin, current_token.end,
                                                             CUSHION_ALLOCATION_CLASS_PERSISTENT);

    node->flags = CUSHION_MACRO_FLAG_SNIPPET;
    node->replacement_list_first = NULL;
    node->parameters_first = NULL;

    current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_COMMA)
    {
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Expected \",\" after first argument for CUSHION_SNIPPET.");
        return;
    }

    struct cushion_token_list_item_t *content_first = NULL;
    struct cushion_token_list_item_t *content_last = NULL;
    unsigned int parenthesis_left = 1u;

    while (parenthesis_left > 0u && !cushion_instance_is_error_signaled (state->instance))
    {
        current_token_meta = lexer_file_state_pop_token (state, &current_token);
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
            cushion_instance_lexer_error (state, &current_token_meta,
                                          "Encountered preprocessor directive while lexing snippet. Not supported.");
            return;

        case CUSHION_TOKEN_TYPE_IDENTIFIER:
            switch (current_token.identifier_kind)
            {
            case CUSHION_IDENTIFIER_KIND_CUSHION_PRESERVE:
                cushion_instance_lexer_error (state, &current_token_meta,
                                              "Found __CUSHION_PRESERVE__ inside CUSHION_SNIPPET. Not supported.");
                return;

            case CUSHION_IDENTIFIER_KIND_CUSHION_WRAPPED:
                cushion_instance_lexer_error (state, &current_token_meta,
                                              "Found __CUSHION_WRAPPED__ inside CUSHION_SNIPPET. Not supported.");
                return;

            case CUSHION_IDENTIFIER_KIND_CUSHION_EVALUATED_ARGUMENT:
                cushion_instance_lexer_error (
                    state, &current_token_meta,
                    "Found __CUSHION_EVALUATED_ARGUMENT__ inside CUSHION_SNIPPET. Not supported.");
                return;

            default:
                goto append_token;
                break;
            }

            break;

        case CUSHION_TOKEN_TYPE_PUNCTUATOR:
            switch (current_token.punctuator_kind)
            {
            case CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS:
                ++parenthesis_left;
                goto append_token;
                break;

            case CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS:
                --parenthesis_left;

                if (parenthesis_left > 0u)
                {
                    goto append_token;
                }

                break;

            default:
                goto append_token;
                break;
            }

            break;

        case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:
        case CUSHION_TOKEN_TYPE_NUMBER_FLOATING:
        case CUSHION_TOKEN_TYPE_DIGIT_IDENTIFIER_SEQUENCE:
        case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
        case CUSHION_TOKEN_TYPE_STRING_LITERAL:
        case CUSHION_TOKEN_TYPE_OTHER:
            goto append_token;
            break;

        case CUSHION_TOKEN_TYPE_NEW_LINE:
        case CUSHION_TOKEN_TYPE_GLUE:
        case CUSHION_TOKEN_TYPE_COMMENT:
            // Glue and comments are ignored in replacement lists as replacements newer preserve formatting.
            break;

        case CUSHION_TOKEN_TYPE_END_OF_FILE:
            cushion_instance_lexer_error (state, &current_token_meta,
                                          "Encountered end of file while parsing CUSHION_SNIPPET.");
            return;

        append_token:
        {
            struct cushion_token_list_item_t *new_token_item =
                cushion_save_token_to_memory (state->instance, &current_token, CUSHION_ALLOCATION_CLASS_PERSISTENT);

            // We do not save file and line info in replacement lists as this is not how macros usually work.
            if (content_last)
            {
                content_last->next = new_token_item;
            }
            else
            {
                content_first = new_token_item;
            }

            content_last = new_token_item;
            break;
        }
        }
    }

    node->replacement_list_first = content_first;
    cushion_instance_macro_add (state->instance, node, lex_error_context (state, &first_token_meta));
}
#endif

static struct cushion_token_t lex_create_file_macro_token (struct cushion_lexer_file_state_t *state)
{
    const size_t file_name_length = strlen (state->tokenization.file_name);
    char *formatted_file_name = cushion_allocator_allocate (&state->instance->allocator, file_name_length + 3u,
                                                            _Alignof (char), CUSHION_ALLOCATION_CLASS_TRANSIENT);

    formatted_file_name[0u] = '"';
    memcpy (formatted_file_name + 1u, state->tokenization.file_name, file_name_length);
    formatted_file_name[file_name_length + 1u] = '"';
    formatted_file_name[file_name_length + 2u] = '\0';

    return (struct cushion_token_t) {
        .type = CUSHION_TOKEN_TYPE_STRING_LITERAL,
        .begin = formatted_file_name,
        .end = formatted_file_name + file_name_length + 2u,
        .symbolic_literal =
            {
                .encoding = CUSHION_TOKEN_SUBSEQUENCE_ENCODING_ORDINARY,
                .begin = formatted_file_name + 1u,
                .end = formatted_file_name + file_name_length + 1u,
            },
    };
}

static struct cushion_token_t lex_create_line_macro_token (struct cushion_lexer_file_state_t *state)
{
    // Other parts of code might expect stringized value of literal, so we need to create it, unfortunately.
    unsigned int value = state->last_token_line;
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
    value = state->last_token_line;

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

    return (struct cushion_token_t) {
        .type = CUSHION_TOKEN_TYPE_NUMBER_INTEGER,
        .begin = formatted_literal,
        .end = formatted_literal + string_length,
        .unsigned_number_value = state->last_token_line,
    };
}

static void lex_code_macro_pragma (struct cushion_lexer_file_state_t *state,
                                   const struct lexer_pop_token_meta_t *pragma_token_meta)
{
    const unsigned int start_line = pragma_token_meta->line;
    struct cushion_token_t current_token;
    struct lexer_pop_token_meta_t current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS)
    {
        cushion_instance_lexer_error (state, &current_token_meta, "Expected \"(\" after _Pragma.");
        return;
    }

    current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_STRING_LITERAL)
    {
        cushion_instance_lexer_error (state, &current_token_meta, "Expected string literal as argument of _Pragma.");
        return;
    }

    if (current_token.symbolic_literal.encoding != CUSHION_TOKEN_SUBSEQUENCE_ENCODING_ORDINARY)
    {
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Only ordinary encoding supported for _Pragma argument.");
        return;
    }

    // Currently, we cannot check whether we're already at new line, so we're adding new line just in case.
    // New line addition (even if it was necessary) breaks line numbering for errors,
    // therefore we need to add line directive before it too.

    // We need new line before #pragma, but lex_update_line_mark might not create it if we're already on the correct
    // line, so we check for that case and apply additional new line when it happens.
    if (state->last_marked_line == start_line && (state->last_marked_file == state->tokenization.file_name ||
                                                  strcmp (state->last_marked_file, state->tokenization.file_name) == 0))
    {
        cushion_instance_output_null_terminated (state->instance, "\n");
        lex_on_line_mark_manually_updated (state, state->last_marked_file, state->last_marked_line + 1u);
    }

    lex_update_line_mark (state, state->tokenization.file_name, start_line);
    cushion_instance_output_null_terminated (state->instance, "#pragma ");

    const char *output_begin_cursor = current_token.symbolic_literal.begin;
    const char *cursor = current_token.symbolic_literal.begin;

    while (cursor < current_token.symbolic_literal.end)
    {
        if (*cursor == '\\')
        {
            if (cursor + 1u >= current_token.symbolic_literal.end)
            {
                cushion_instance_lexer_error (state, &current_token_meta,
                                              "Encountered \"\\\" as the last symbol of _Pragma argument literal.");
                return;
            }

            if (output_begin_cursor < cursor)
            {
                cushion_instance_output_sequence (state->instance, output_begin_cursor, cursor);
            }

            ++cursor;
            switch (*cursor)
            {
            case '"':
            case '\\':
                // Allowed to be escaped.
                output_begin_cursor = cursor;
                break;

            default:
                cushion_instance_lexer_error (
                    state, &current_token_meta,
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
    lex_on_line_mark_manually_updated (state, state->tokenization.file_name, start_line + 1u);
    current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS)
    {
        cushion_instance_lexer_error (state, &current_token_meta, "Expected \")\" after _Pragma argument.");
        return;
    }
}

static unsigned int lex_code_identifier_is_replaced (struct cushion_lexer_file_state_t *state,
                                                     struct cushion_token_t *current_token,
                                                     const struct lexer_pop_token_meta_t *current_token_meta)
{
    // When we're doing scan only pass, we should not lex identifiers like this at all.
    assert (!(state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY));

    switch (current_token->identifier_kind)
    {
    case CUSHION_IDENTIFIER_KIND_FILE:
    {
        struct cushion_token_t token = lex_create_file_macro_token (state);
        lexer_file_state_reinsert_token (state, &token);
        return 1u;
    }

    case CUSHION_IDENTIFIER_KIND_LINE:
    {
        struct cushion_token_t token = lex_create_line_macro_token (state);
        lexer_file_state_reinsert_token (state, &token);
        return 1u;
    }

    case CUSHION_IDENTIFIER_KIND_CUSHION_PRESERVE:
        cushion_instance_lexer_error (state, current_token_meta,
                                      "Encountered cushion preserve keyword in unexpected context.");
        return 1u;

#if defined(CUSHION_EXTENSIONS)
    case CUSHION_IDENTIFIER_KIND_CUSHION_DEFER:
        if (cushion_instance_has_feature (state->instance, CUSHION_FEATURE_DEFER))
        {
            lex_code_defer (state, current_token_meta);
        }
        else
        {
            cushion_instance_lexer_error (state, current_token_meta,
                                          "Encountered CUSHION_DEFER, but this feature is not enabled.");
        }

        return 1u;

    case CUSHION_IDENTIFIER_KIND_CUSHION_WRAPPED:
        cushion_instance_lexer_error (state, current_token_meta,
                                      "Encountered cushion wrapped keyword in unexpected context.");
        return 1u;

#    define CHECK_IF_STATEMENT_ACCUMULATOR_FEATURE_ENABLED                                                             \
        if (!cushion_instance_has_feature (state->instance, CUSHION_FEATURE_STATEMENT_ACCUMULATOR))                    \
        {                                                                                                              \
            cushion_instance_lexer_error (                                                                             \
                state, current_token_meta,                                                                             \
                "Encountered statement accumulator keyword, but this feature is not enabled.");                        \
            return 1u;                                                                                                 \
        }

    case CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR:
        CHECK_IF_STATEMENT_ACCUMULATOR_FEATURE_ENABLED
        lex_code_statement_accumulator (state);
        return 1u;

    case CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_PUSH:
        CHECK_IF_STATEMENT_ACCUMULATOR_FEATURE_ENABLED
        lex_code_statement_accumulator_push (state);
        return 1u;

    case CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_REF:
        CHECK_IF_STATEMENT_ACCUMULATOR_FEATURE_ENABLED
        lex_code_statement_accumulator_ref (state);
        return 1u;

    case CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_UNREF:
        CHECK_IF_STATEMENT_ACCUMULATOR_FEATURE_ENABLED
        lex_code_statement_accumulator_unref (state);
        return 1u;

    case CUSHION_IDENTIFIER_KIND_CUSHION_SNIPPET:
        if (cushion_instance_has_feature (state->instance, CUSHION_FEATURE_SNIPPET))
        {
            lex_code_snippet (state);
        }
        else
        {
            cushion_instance_lexer_error (state, current_token_meta,
                                          "Encountered CUSHION_SNIPPET, but this feature is not enabled.");
        }

        return 1u;

    case CUSHION_IDENTIFIER_KIND_CUSHION_EVALUATED_ARGUMENT:
        cushion_instance_lexer_error (state, current_token_meta,
                                      "Encountered cushion evaluated argument keyword in unexpected context.");
        return 1u;

    case CUSHION_IDENTIFIER_KIND_CUSHION_REPLACEMENT_INDEX:
        cushion_instance_lexer_error (state, current_token_meta,
                                      "Encountered cushion replacement index keyword in unexpected context.");
        return 1u;
#endif

    case CUSHION_IDENTIFIER_KIND_MACRO_PRAGMA:
        lex_code_macro_pragma (state, current_token_meta);
        return 1u;

    default:
        break;
    }

    struct lex_replace_macro_result_t replace_result = lex_replace_identifier_if_macro (
        state, current_token, current_token_meta, LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_CODE);

    if (replace_result.replaced)
    {
        lexer_file_state_push_tokens (state, replace_result.tokens, LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT);
        return 1u;
    }

    return 0u;
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

static void make_lex_state_path_writeable_to_literal (struct cushion_lexer_file_state_t *state)
{
    // Currently, we convert all "\" and "\\" occurrences in path to "/", so it is as easy to paste it into code.
    // Should be updated for more platform-specific logic if causes issues on Windows for some reason.

    const char *input = state->file_name;
    char *output = state->file_name;

    while (*input)
    {
        if (*input == '\\')
        {
            // Skip additional "\" if it exists.
            if (*(input + 1u) == '\\')
            {
                ++input;
            }

            *output = '/';
        }
        else
        {
            *output = *input;
        }

        ++input;
        ++output;
    }

    *output = '\0';
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
    state->stack_exit_file = NULL;
    state->stack_exit_line = 1u;
    state->last_marked_file = state->file_name;
    state->last_marked_line = 1u;
    state->last_token_line = 1u;
    state->conditional_inclusion_node = NULL;

#if defined(CUSHION_EXTENSIONS)
    state->defer_feature = NULL;

    if ((state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u &&
        cushion_instance_has_feature (instance, CUSHION_FEATURE_DEFER))
    {
        state->defer_feature = cushion_allocator_allocate (
            &instance->allocator, sizeof (struct lex_defer_feature_state_t),
            _Alignof (struct lex_defer_feature_state_t), CUSHION_ALLOCATION_CLASS_TRANSIENT);

        state->defer_feature->scope = LEX_DEFER_SCOPE_GLOBAL;
        state->defer_feature->statement_flags = LEX_DEFER_STATEMENT_FLAG_FIRST_TOKEN;
        state->defer_feature->statement_parenthesis_counter = 0u;

        state->defer_feature->expected_label_name_begin = NULL;
        state->defer_feature->expected_label_name_end = NULL;
    }
#endif

    // We need to always convert file path to absolute in order to have proper line directives everywhere.
    if (cushion_convert_path_to_absolute (path, state->file_name) != CUSHION_INTERNAL_RESULT_OK)
    {
        cushion_instance_execution_error (state->instance,
                                          (struct cushion_error_context_t) {
                                              .file = path,
                                              .line = 1u,
                                              .column = UINT_MAX,
                                          },
                                          "Unable to convert path \"%s\" to absolute path.", path);

        cushion_allocator_reset_transient (&instance->allocator, allocation_marker);
        return;
    }

    make_lex_state_path_writeable_to_literal (state);
    cushion_instance_output_depfile_entry (state->instance, state->file_name);

    if ((state->flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u)
    {
        cushion_instance_output_line_marker (instance, state->file_name, 1u);
    }

    cushion_tokenization_state_init_for_file (&state->tokenization, state->file_name, input_file,
                                              &state->instance->allocator, CUSHION_ALLOCATION_CLASS_TRANSIENT);
    lex_update_tokenization_flags (state);

    struct cushion_token_t current_token;
    current_token.type = CUSHION_TOKEN_TYPE_NEW_LINE; // Just stub value.

    struct lexer_pop_token_meta_t current_token_meta = {
        .flags = LEXER_TOKEN_STACK_ITEM_FLAG_NONE,
        .file = state->tokenization.file_name,
        .line = state->tokenization.cursor_line,
    };

    while (lexer_file_state_should_continue (state))
    {
        const unsigned int previous_is_macro_replacement =
            current_token_meta.flags & LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT;

        const enum cushion_token_type_t previous_type = current_token.type;
        current_token_meta = lexer_file_state_pop_token (state, &current_token);

        if (cushion_instance_is_error_signaled (instance))
        {
            break;
        }

        // Preprocessing pass: check directives and other things that might be omitted in the result.
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
            lex_preprocessor_elif (state, &current_token, &current_token_meta);
            break;

        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFDEF:
            lex_preprocessor_elifdef (state, &current_token, &current_token_meta, 0u);
            break;

        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFNDEF:
            lex_preprocessor_elifdef (state, &current_token, &current_token_meta, 1u);
            break;

        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ELSE:
            lex_preprocessor_else (state, &current_token, &current_token_meta);
            break;

        case CUSHION_TOKEN_TYPE_PREPROCESSOR_ENDIF:
            lex_preprocessor_endif (state, &current_token, &current_token_meta);
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
            cushion_instance_lexer_error (state, &current_token_meta,
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
            if (lex_code_identifier_is_replaced (state, &current_token, &current_token_meta))
            {
                // Replaced, not really kept in here.
                break;
            }

            // Just a regular identifier that is kept inside output.
            goto token_kept;

        case CUSHION_TOKEN_TYPE_PUNCTUATOR:
        case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:
        case CUSHION_TOKEN_TYPE_NUMBER_FLOATING:
        case CUSHION_TOKEN_TYPE_DIGIT_IDENTIFIER_SEQUENCE:
        case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
        case CUSHION_TOKEN_TYPE_STRING_LITERAL:
        case CUSHION_TOKEN_TYPE_GLUE:
        case CUSHION_TOKEN_TYPE_OTHER:
            // Something real, we need to keep it and output it.
            goto token_kept;

        case CUSHION_TOKEN_TYPE_NEW_LINE:
        case CUSHION_TOKEN_TYPE_COMMENT:
            // Just skip them.
            break;

        case CUSHION_TOKEN_TYPE_END_OF_FILE:
            if (state->conditional_inclusion_node)
            {
                cushion_instance_lexer_error (
                    state, &current_token_meta,
                    "Encountered end of file, but conditional inclusion started line %u at is not closed.",
                    state->conditional_inclusion_node->line);
            }

#if defined(CUSHION_EXTENSIONS)
            if (state->defer_feature && state->defer_feature->scope != LEX_DEFER_SCOPE_GLOBAL)
            {
                cushion_instance_lexer_error (state, &current_token_meta,
                                              "Encountered end of file, but defer feature thinks it is still in "
                                              "function scope. Unclosed function?");
            }
#endif

            if ((flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u)
            {
                // Output new line at the end of file as a rule.
                cushion_instance_output_null_terminated (state->instance, "\n");
            }

            break;
        }

        // Omit unless explicitly jumped to kept phase.
        continue;

    token_kept:
        // Shouldn't get tokens that can be kept while in excluded mode.
        assert (!state->conditional_inclusion_node ||
                state->conditional_inclusion_node->state != CONDITIONAL_INCLUSION_STATE_EXCLUDED);

        // Output the kept token if not in scan only mode.
        if ((flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u)
        {
#if defined(CUSHION_EXTENSIONS)
#    define PUT_FAKE_SPACE                                                                                             \
        /* Might be important for return recording. */                                                                 \
        if (state->defer_feature)                                                                                      \
        {                                                                                                              \
            struct cushion_token_t fake_space;                                                                         \
            fake_space.type = CUSHION_TOKEN_TYPE_GLUE;                                                                 \
            fake_space.begin = " ";                                                                                    \
            fake_space.end = fake_space.begin + 1u;                                                                    \
                                                                                                                       \
            if (lex_defer_preprocess (state, &fake_space, &current_token_meta) == LEX_DEFER_PREPROCESS_RESULT_KEEP)    \
            {                                                                                                          \
                cushion_instance_output_null_terminated (instance, " ");                                               \
            }                                                                                                          \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            cushion_instance_output_null_terminated (instance, " ");                                                   \
        }
#else
#    define PUT_FAKE_SPACE cushion_instance_output_null_terminated (instance, " ");
#endif

            // Check line number and add marker if needed.
            if (lex_update_line_mark (state, current_token_meta.file, current_token_meta.line))
            {
                // Everything is done in conditional, if is only needed for else clauses.
            }
            // Add separator for macro replacement tokens if needed.
            else if (previous_is_macro_replacement &&
                     lex_is_separator_needed_for_token_pair (previous_type, current_token.type) &&
                     // Special case: _Pragma is always separate by new lines, therefore we don't need whitespace.
                     (current_token.type != CUSHION_TOKEN_TYPE_IDENTIFIER ||
                      current_token.identifier_kind != CUSHION_IDENTIFIER_KIND_MACRO_PRAGMA))
            {
                PUT_FAKE_SPACE
            }
            // If previous was comment, add separator just in case.
            else if (previous_type == CUSHION_TOKEN_TYPE_COMMENT && current_token.type != CUSHION_TOKEN_TYPE_GLUE)
            {
                PUT_FAKE_SPACE
            }
#undef PUT_FAKE_SPACE

#if defined(CUSHION_EXTENSIONS)
            // Process defer feature if enabled.
            if (state->defer_feature)
            {
                if (lex_defer_preprocess (state, &current_token, &current_token_meta) ==
                    LEX_DEFER_PREPROCESS_RESULT_CONSUMED)
                {
                    // Skip the token.
                    continue;
                }

                if (cushion_instance_is_error_signaled (instance))
                {
                    break;
                }
            }
#endif

            // Now we can properly output the processed token.
            cushion_instance_output_sequence (instance, current_token.begin, current_token.end);
        }
    }

    // Currently there is no safe way to reset transient data except for the end of file lexing.
    // For the most cases, we would never use more than 1 or 2 pages for file (with 1mb pages),
    // so there is usually no need for aggressive memory reuse.
    cushion_allocator_reset_transient (&instance->allocator, allocation_marker);
}

#if defined(CUSHION_EXTENSIONS)
void cushion_lex_finalize_statement_accumulators (struct cushion_instance_t *instance)
{
    struct cushion_statement_accumulator_unordered_push_t *unordered_push = instance->statement_unordered_push_first;
    while (unordered_push)
    {
        if ((unordered_push->flags & CUSHION_STATEMENT_ACCUMULATOR_PUSH_FLAG_OPTIONAL) == 0u)
        {
            struct cushion_error_context_t error_context = {
                .file = unordered_push->entry_template.source_file,
                .line = unordered_push->entry_template.source_line,
                .column = UINT_MAX,
            };

            cushion_instance_execution_error (
                instance, error_context,
                "Failed to resolve non-optional unordered push: target accumulator was never found.");
        }

        unordered_push = unordered_push->next;
    }

    struct cushion_statement_accumulator_t *accumulator = instance->statement_accumulators_first;
    while (accumulator)
    {
        enum output_extension_injector_content_flags_t flags = OUTPUT_EXTENSION_INJECTOR_FLAGS_NONE;
        if (accumulator->flags & CUSHION_STATEMENT_ACCUMULATOR_FLAG_JUMP_FORBIDDEN)
        {
            flags |= OUTPUT_EXTENSION_INJECTOR_FLAGS_JUMP_FORBIDDEN;
        }

        cushion_output_select_sink (instance, accumulator->output_node);
        struct cushion_statement_accumulator_entry_t *entry = accumulator->entries_first;

        while (entry)
        {
            output_extension_injector_content (instance, entry->content_first, flags);
            entry = entry->next;
        }

        if (accumulator->entries_first)
        {
            // Restore file and line information to accumulator initial state.
            cushion_instance_output_null_terminated (instance, "\n");
            cushion_instance_output_line_marker (instance, accumulator->output_node->source_file,
                                                 accumulator->output_node->source_line);
        }

        cushion_output_finish_sink (instance, accumulator->output_node);
        accumulator = accumulator->next;
    }
}
#endif
