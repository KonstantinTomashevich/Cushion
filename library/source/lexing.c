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
    new_token->line = token->type == CUSHION_TOKEN_TYPE_NEW_LINE ? state->tokenization.cursor_line - 1u :
                                                                   state->tokenization.cursor_line;

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

    if (state->last_marked_line == meta->line &&
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

    return meta;
}

static inline void lexer_file_state_path_init (struct cushion_lexer_file_state_t *state, const char *data)
{
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
#endif

        default:
            break;
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
    CHECK_KIND ("CUSHION_STATEMENT_ACCUMULATOR_REF", CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_REF)
    CHECK_KIND ("CUSHION_STATEMENT_ACCUMULATOR_UNREF", CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_UNREF)

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
    struct cushion_token_list_item_t *wrapped_tokens;
    struct cushion_token_list_item_t *current_token;
    unsigned int replacement_line;
    struct macro_replacement_token_list_t result;
    struct macro_replacement_token_list_t sub_list;
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

static inline void macro_replacement_context_process_identifier_into_sub_list (
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
                return;
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

        break;
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

        break;
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
        }
        else
        {
            // Just a regular identifier.
            macro_replacement_token_list_append (state, &context->sub_list, &context->current_token->token,
                                                 state->tokenization.file_name, context->replacement_line);
        }

        break;
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

                if (context.current_token->token.identifier_kind == CUSHION_IDENTIFIER_KIND_VA_ARGS)
                {
                    if ((macro->flags & CUSHION_MACRO_FLAG_VARIADIC_PARAMETERS) == 0u)
                    {
                        cushion_instance_execution_error (
                            state->instance, macro_replacement_error_context (state, &context),
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
                    macro_replacement_token_list_append (state, &context.result, &stringized_token,
                                                         state->tokenization.file_name, context.replacement_line);
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
                        state->instance, macro_replacement_error_context (state, &context),
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
                macro_replacement_token_list_append (state, &context.result, &context.current_token->token,
                                                     state->tokenization.file_name, context.replacement_line);
                break;
            }

            break;

        case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:
        case CUSHION_TOKEN_TYPE_NUMBER_FLOATING:
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

static inline unsigned int lex_is_preprocessor_token_type (enum cushion_token_type_t token_type)
{
    switch (token_type)
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
        return 1u;

    default:
        return 0u;
    }
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

static struct cushion_token_list_item_t *lex_replace_identifier_if_macro (
    struct cushion_lexer_file_state_t *state,
    struct cushion_token_t *identifier_token,
    const struct lexer_pop_token_meta_t *identifier_token_meta,
    enum lex_replace_identifier_if_macro_context_t context);

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
            cushion_instance_lexer_error (state, &meta,
                                          "Encountered preprocessor directive while lexing preserved preprocessor "
                                          "directive. Shouldn't be possible at all, can be an internal error.");
            return;

        case CUSHION_TOKEN_TYPE_IDENTIFIER:
        {
            switch (current_token.identifier_kind)
            {
            case CUSHION_IDENTIFIER_KIND_CUSHION_PRESERVE:
                cushion_instance_lexer_error (state, &meta,
                                              "Encountered cushion preserve keyword in unexpected context.");
                return;

            case CUSHION_IDENTIFIER_KIND_CUSHION_WRAPPED:
                cushion_instance_lexer_error (state, &meta,
                                              "Encountered cushion wrapped keyword in preserved preprocessor context.");
                return;

            default:
                break;
            }

            struct cushion_token_list_item_t *macro_tokens = lex_replace_identifier_if_macro (
                state, &current_token, &meta, LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_EVALUATION);

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

static struct cushion_token_list_item_t *lex_replace_identifier_if_macro (
    struct cushion_lexer_file_state_t *state,
    struct cushion_token_t *identifier_token,
    const struct lexer_pop_token_meta_t *identifier_token_meta,
    enum lex_replace_identifier_if_macro_context_t context)
{
    struct cushion_macro_node_t *macro =
        cushion_instance_macro_search (state->instance, identifier_token->begin, identifier_token->end);

    if (!macro || (macro->flags & CUSHION_MACRO_FLAG_PRESERVED))
    {
        // No need to unwrap.
        return NULL;
    }

    const unsigned int start_line = state->last_marked_line;
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
            return NULL;
        }
#endif
        break;
    }

    struct lex_macro_argument_t *arguments_first = NULL;
    struct lex_macro_argument_t *arguments_last = NULL;

    if (macro->flags & CUSHION_MACRO_FLAG_FUNCTION)
    {
        // Scan for the opening parenthesis.
        struct cushion_token_t current_token;
        struct lexer_pop_token_meta_t current_token_meta;

        struct cushion_token_list_item_t *argument_tokens_first = NULL;
        struct cushion_token_list_item_t *argument_tokens_last = NULL;
        unsigned int skipping_until_significant = 1u;

        while (skipping_until_significant && !cushion_instance_is_error_signaled (state->instance))
        {
            current_token_meta = lexer_file_state_pop_token (state, &current_token);
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
                    cushion_instance_lexer_error (
                        state, &current_token_meta,
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
            cushion_instance_lexer_error (state, &current_token_meta, "Expected \"(\" after function-line macro name.");
            return NULL;
        }

        unsigned int parenthesis_counter = 1u;
        struct cushion_macro_parameter_node_t *parameter = macro->parameters_first;
        unsigned int parameterless_function_line =
            !parameter && (macro->flags & CUSHION_MACRO_FLAG_VARIADIC_PARAMETERS) == 0u;

        while (parenthesis_counter > 0u && !cushion_instance_is_error_signaled (state->instance))
        {
            current_token_meta = lexer_file_state_pop_token (state, &current_token);
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
                        cushion_instance_lexer_error (
                            state, &current_token_meta,
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
                    // Just skip new line, line number after token replacement would be managed through directives.
                    break;

                case LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_EVALUATION:
                    cushion_instance_lexer_error (
                        state, &current_token_meta,
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
                cushion_instance_lexer_error (state, &current_token_meta,
                                              "Got to the end of file while parsing arguments of function-like macro.");
                break;

            default:
            append_argument_token:
            {
                if (!parameter && (macro->flags & CUSHION_MACRO_FLAG_VARIADIC_PARAMETERS) == 0u)
                {
                    cushion_instance_lexer_error (state, &current_token_meta,
                                                  "Encountered more parameters for function-line macro than expected.");
                    break;
                }

                struct cushion_token_list_item_t *argument_tokens_new_token =
                    cushion_save_token_to_memory (state->instance, &current_token, CUSHION_ALLOCATION_CLASS_TRANSIENT);

                argument_tokens_new_token->file = state->tokenization.file_name;
                argument_tokens_new_token->line = start_line;

                if (argument_tokens_last)
                {
                    argument_tokens_last->next = argument_tokens_new_token;
                }
                else
                {
                    argument_tokens_first = argument_tokens_new_token;
                }

                argument_tokens_last = argument_tokens_new_token;
                break;
            }
            }
        }

        LEX_WHEN_ERROR (return NULL)
        if (parameter)
        {
            cushion_instance_lexer_error (state, &current_token_meta,
                                          "Encountered less parameters for function-line macro than expected.");
            return NULL;
        }
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
        struct cushion_token_t current_token;
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

        LEX_WHEN_ERROR (return NULL)
        if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR ||
            current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_LEFT_CURLY_BRACE)
        {
            cushion_instance_lexer_error (state, &current_token_meta,
                                          "Expected \"{\" after function-line macro with wrapped block arguments.");
            return NULL;
        }

#    define APPEND_WRAPPED_TOKEN(LIST_NAME, TOKEN_TO_SAVE, LINE)                                                       \
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
                    APPEND_WRAPPED_TOKEN (wrapped_tokens, &current_token, state->tokenization.cursor_line)
                }

                break;

            case CUSHION_TOKEN_TYPE_NEW_LINE:
                // Minus one, because new line started at previous line technically.
                APPEND_WRAPPED_TOKEN (wrapped_tokens, &current_token, state->tokenization.cursor_line - 1u)
                break;

            case CUSHION_TOKEN_TYPE_END_OF_FILE:
                cushion_instance_lexer_error (state, &current_token_meta,
                                              "Got to the end of file while parsing wrapped block for "
                                              "function-like macro with wrapped block feature enabled.");
                break;

            default:
                APPEND_WRAPPED_TOKEN (wrapped_tokens, &current_token, state->tokenization.cursor_line)
                break;
            }
        }

        LEX_WHEN_ERROR (return NULL)
#    undef APPEND_WRAPPED_TOKEN
    }
#endif

    return lex_do_macro_replacement (state, macro, arguments_first, wrapped_tokens_first, start_line);
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
        .line = state->last_marked_line,
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

    return 0u;
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
                return state->tokenization.cursor_line;

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
                struct cushion_token_list_item_t *macro_tokens = lex_replace_identifier_if_macro (
                    state, &current_token, &meta, LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_EVALUATION);

                if (!macro_tokens)
                {
                    cushion_instance_lexer_error (
                        state, &meta,
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
                cushion_instance_lexer_error (
                    state, &meta,
                    "Encountered unexpected punctuator while evaluating preprocessor conditional expression.");
                return 0;
            }

            // Should never happen unless memory is corrupted.
            assert (0);
            return 0;

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
    unsigned int start_line = state->tokenization.cursor_line;
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
    unsigned int start_line = state->tokenization.cursor_line;
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
    const unsigned int start_line = state->tokenization.cursor_line;
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
    unsigned int start_line = state->tokenization.cursor_line;

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
    const unsigned int start_line = state->tokenization.cursor_line;
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

        if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR &&
            current_token.punctuator_kind == CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS)
        {
            cushion_instance_lexer_error (state, &current_token_meta,
                                          "Expected \")\" or \",\" while reading macro parameter name list.");
            return;
        }

        break;
    }

    case CUSHION_TOKEN_TYPE_NEW_LINE:
        // No replacement list, go directly to registration.
        goto register_macro;

    case CUSHION_TOKEN_TYPE_END_OF_FILE:
        // No replacement list, go directly to registration.
        goto register_macro;

    case CUSHION_TOKEN_TYPE_GLUE:
    case CUSHION_TOKEN_TYPE_COMMENT:
        break;

    default:
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Expected whitespaces, comments, \"(\", line end or file end after macro name.");
        return;
    }

    // Lex replacement list.
    enum cushion_lex_replacement_list_result_t lex_result =
        cushion_lex_replacement_list_from_lexer (state->instance, state, &node->replacement_list_first, &node->flags);
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

register_macro:
    // Register generated macro.
    cushion_instance_macro_add (state->instance, node, lex_error_context (state, &first_token_meta));
    lex_update_tokenization_flags (state);
}

static void lex_preprocessor_undef (struct cushion_lexer_file_state_t *state)
{
    const unsigned int start_line = state->tokenization.cursor_line;
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

        lex_preprocessor_expect_new_line (state);
        LEX_WHEN_ERROR (return)
        lex_update_tokenization_flags (state);
        return;
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
        .line = state->last_marked_line,
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

static struct cushion_token_t lex_create_file_macro_token (struct cushion_lexer_file_state_t *state);
static struct cushion_token_t lex_create_line_macro_token (struct cushion_lexer_file_state_t *state);

/// \details Defers and statement accumulator pushes should have the same rules for their content,
///          therefore they're called extensions injectors and this logic is for them.
static struct cushion_token_list_item_t *lex_extension_injector_content (struct cushion_lexer_file_state_t *state,
                                                                         const char *safe_to_use_file_name)
{
    struct cushion_token_t current_token;
    struct lexer_pop_token_meta_t current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return NULL)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR &&
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
                struct cushion_token_list_item_t *macro_tokens = lex_replace_identifier_if_macro (
                    state, &current_token, &current_token_meta, LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_CODE);

                if (macro_tokens)
                {
                    lexer_file_state_push_tokens (state, macro_tokens, LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT);
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

static struct cushion_statement_accumulator_t *find_statement_accumulator (struct cushion_instance_t *instance,
                                                                           const char *begin,
                                                                           const char *end)
{
    const unsigned int length = end - begin;
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
    const unsigned int length = end - begin;
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
    // TODO: For defer implementation in the future: if used in context with defers enabled, add flag that forbids
    //       any jump-like instruction inside pushes for this accumulator.

    const unsigned int start_line = state->last_marked_line;
    struct cushion_token_t current_token;
    struct lexer_pop_token_meta_t current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR &&
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

    accumulator->name = cushion_instance_copy_char_sequence_inside (
        state->instance, current_token.begin, current_token.end, CUSHION_ALLOCATION_CLASS_PERSISTENT);
    accumulator->name_length = current_token.end - current_token.begin;

    accumulator->entries_first = NULL;
    accumulator->entries_last = NULL;
    accumulator->output_node = cushion_output_add_deferred_sink (state->instance, state->last_marked_file, start_line);

    accumulator->next = state->instance->statement_accumulators_first;
    state->instance->statement_accumulators_first = accumulator;

    check_statement_accumulator_unordered_pushes (state, accumulator, accumulator->name, accumulator->name_length);
    current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR &&
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
    const unsigned int line = state->last_marked_line;

    struct cushion_token_t current_token;
    struct lexer_pop_token_meta_t current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR &&
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
        cushion_instance_lexer_error (
            state, &current_token_meta,
            "Expected accumulator name identifier as argument for CUSHION_STATEMENT_ACCUMULATOR.");
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

        const unsigned int length = current_token.end - current_token.begin;

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
            saved_accumulator_name_length = name_end - name_begin;
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

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR &&
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

    const unsigned int length = current_token.end - current_token.begin;
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
        ref->name_length = current_token.end - current_token.begin;
        ref->accumulator = NULL;

        ref->next = state->instance->statement_accumulator_refs_first;
        state->instance->statement_accumulator_refs_first = ref;
    }

    current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR &&
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

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR &&
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

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR &&
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

    const unsigned int length = current_token.end - current_token.begin;
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

    if (!ref)
    {
        cushion_instance_lexer_error (
            state, &current_token_meta,
            "Unable to find statement accumulator reference for CUSHION_STATEMENT_ACCUMULATOR_UNREF.");
        return;
    }

    // Just remove reference from the list.
    if (ref_previous)
    {
        ref_previous->next = ref->next;
    }
    else
    {
        state->instance->statement_accumulator_refs_first = ref->next;
    }

    current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR &&
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS)
    {
        cushion_instance_lexer_error (state, &current_token_meta,
                                      "Expected \")\" after CUSHION_STATEMENT_ACCUMULATOR_UNREF argument.");
        return;
    }
}
#endif

static struct cushion_token_t lex_create_file_macro_token (struct cushion_lexer_file_state_t *state)
{
    const size_t file_name_length = strlen (state->tokenization.file_name);
    char *formatted_file_name = cushion_allocator_allocate (&state->instance->allocator, file_name_length + 3u,
                                                            _Alignof (char), CUSHION_ALLOCATION_CLASS_TRANSIENT);

    formatted_file_name[0u] = '"';
    memcpy (formatted_file_name + 1u, state->tokenization.file_name, file_name_length);
    formatted_file_name[file_name_length - 2u] = '"';
    formatted_file_name[file_name_length - 1u] = '\0';

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
    unsigned int value = state->last_marked_line;
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

    return (struct cushion_token_t) {
        .type = CUSHION_TOKEN_TYPE_NUMBER_INTEGER,
        .begin = formatted_literal,
        .end = formatted_literal + string_length,
        .unsigned_number_value = state->last_marked_line,
    };
}

static void lex_code_macro_pragma (struct cushion_lexer_file_state_t *state)
{
    const unsigned int start_line = state->last_marked_line;
    struct cushion_token_t current_token;
    struct lexer_pop_token_meta_t current_token_meta = lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR &&
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

    if (current_token.type != CUSHION_TOKEN_TYPE_PUNCTUATOR &&
        current_token.punctuator_kind != CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS)
    {
        cushion_instance_lexer_error (state, &current_token_meta, "Expected \")\" after _Pragma argument.");
        return;
    }
}

static void lex_code_identifier (struct cushion_lexer_file_state_t *state,
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
        return;
    }

    case CUSHION_IDENTIFIER_KIND_LINE:
    {
        struct cushion_token_t token = lex_create_line_macro_token (state);
        lexer_file_state_reinsert_token (state, &token);
        return;
    }

    case CUSHION_IDENTIFIER_KIND_CUSHION_PRESERVE:
        cushion_instance_lexer_error (state, current_token_meta,
                                      "Encountered cushion preserve keyword in unexpected context.");
        return;

#if defined(CUSHION_EXTENSIONS)
    case CUSHION_IDENTIFIER_KIND_CUSHION_DEFER:
        cushion_instance_lexer_error (state, current_token_meta, "Cushion defer feature is not yet implemented.");
        return;

    case CUSHION_IDENTIFIER_KIND_CUSHION_WRAPPED:
        cushion_instance_lexer_error (state, current_token_meta,
                                      "Encountered cushion wrapped keyword in unexpected context.");
        return;

#    define CHECK_IF_STATEMENT_ACCUMULATOR_FEATURE_ENABLED                                                             \
        if (!cushion_instance_has_feature (state->instance, CUSHION_FEATURE_STATEMENT_ACCUMULATOR))                    \
        {                                                                                                              \
            cushion_instance_lexer_error (                                                                             \
                state, current_token_meta,                                                                             \
                "Encountered statement accumulator keyword, but this feature is not enabled.");                        \
            return;                                                                                                    \
        }

    case CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR:
        CHECK_IF_STATEMENT_ACCUMULATOR_FEATURE_ENABLED
        lex_code_statement_accumulator (state);
        return;

    case CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_PUSH:
        CHECK_IF_STATEMENT_ACCUMULATOR_FEATURE_ENABLED
        lex_code_statement_accumulator_push (state);
        return;

    case CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_REF:
        CHECK_IF_STATEMENT_ACCUMULATOR_FEATURE_ENABLED
        lex_code_statement_accumulator_ref (state);
        return;

    case CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_UNREF:
        CHECK_IF_STATEMENT_ACCUMULATOR_FEATURE_ENABLED
        lex_code_statement_accumulator_unref (state);
        return;
#endif

    case CUSHION_IDENTIFIER_KIND_MACRO_PRAGMA:
        lex_code_macro_pragma (state);
        return;

    default:
        break;
    }

    struct cushion_token_list_item_t *macro_tokens = lex_replace_identifier_if_macro (
        state, current_token, current_token_meta, LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_CODE);

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
            if (*input == '\\')
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
    state->conditional_inclusion_node = NULL;

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

        if (current_token.type == CUSHION_TOKEN_TYPE_NEW_LINE || current_token.type == CUSHION_TOKEN_TYPE_COMMENT)
        {
            // We treat these tokens as insignificant and fix lines and line numbers after them using separate logic.
            continue;
        }

        if ((flags & CUSHION_LEX_FILE_FLAG_SCAN_ONLY) == 0u &&
            (!state->conditional_inclusion_node ||
             state->conditional_inclusion_node->state != CONDITIONAL_INCLUSION_STATE_EXCLUDED))
        {
            // Check line number and add marker if needed.
            if (lex_is_preprocessor_token_type (current_token.type) ||
                lex_update_line_mark (state, current_token_meta.file, current_token_meta.line))
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
                cushion_instance_output_null_terminated (instance, " ");
            }
            // If previous was comment, add separator just in case.
            else if (previous_type == CUSHION_TOKEN_TYPE_COMMENT &&
                     (current_token.type != CUSHION_TOKEN_TYPE_GLUE &&
                      current_token.type != CUSHION_TOKEN_TYPE_NEW_LINE &&
                      !lex_is_preprocessor_token_type (current_token.type)))
            {
                cushion_instance_output_null_terminated (state->instance, " ");
            }
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
            lex_code_identifier (state, &current_token, &current_token_meta);
            break;

        case CUSHION_TOKEN_TYPE_PUNCTUATOR:
        case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:
        case CUSHION_TOKEN_TYPE_NUMBER_FLOATING:
        case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
        case CUSHION_TOKEN_TYPE_STRING_LITERAL:
        case CUSHION_TOKEN_TYPE_GLUE:
        case CUSHION_TOKEN_TYPE_OTHER:
            cushion_instance_output_sequence (instance, current_token.begin, current_token.end);
            break;

        case CUSHION_TOKEN_TYPE_NEW_LINE:
        case CUSHION_TOKEN_TYPE_COMMENT:
            // Processed separately as part of line fixing routine.
            break;

        case CUSHION_TOKEN_TYPE_END_OF_FILE:
            if (state->conditional_inclusion_node)
            {
                cushion_instance_lexer_error (
                    state, &current_token_meta,
                    "Encountered end of file, but conditional inclusion started line %u at is not closed.",
                    state->conditional_inclusion_node->line);
            }

            break;
        }
    }

    // Currently there is no safe way to reset transient data except for the end of file lexing.
    // For the most cases, we would never use more than 1 or 2 pages for file (with 1mb pages),
    // so there is usually no need for aggressive memory reuse.
    cushion_allocator_reset_transient (&instance->allocator, allocation_marker);
}

#if defined(CUSHION_EXTENSIONS)
static void output_extension_injector_context (struct cushion_instance_t *instance,
                                               struct cushion_token_list_item_t *content)
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

            default:
                cushion_instance_output_sequence (instance, content->token.begin, content->token.end);
                break;
            }
            break;

        case CUSHION_TOKEN_TYPE_PUNCTUATOR:
        case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:
        case CUSHION_TOKEN_TYPE_NUMBER_FLOATING:
        case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
        case CUSHION_TOKEN_TYPE_STRING_LITERAL:
        case CUSHION_TOKEN_TYPE_GLUE:
        case CUSHION_TOKEN_TYPE_OTHER:
            cushion_instance_output_sequence (instance, content->token.begin, content->token.end);
            break;
        }

        previous_is_macro = content->flags & CUSHION_TOKEN_LIST_ITEM_FLAG_INJECTED_MACRO_REPLACEMENT;
        content = content->next;
    }
}

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
        cushion_output_select_sink (instance, accumulator->output_node);
        struct cushion_statement_accumulator_entry_t *entry = accumulator->entries_first;

        while (entry)
        {
            output_extension_injector_context (instance, entry->content_first);
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
