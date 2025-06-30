#include "internal.h"

struct re2c_tags_t
{
    /*!stags:re2c format = 'const char *@@;';*/
};

static inline void cushion_instance_tokenization_error (struct cushion_instance_t *instance,
                                                        struct cushion_tokenization_state_t *tokenization,
                                                        const char *format,
                                                        ...)
{
    va_list variadic_arguments;
    va_start (variadic_arguments, format);
    cushion_instance_execution_error_internal (instance,
                                               (struct cushion_error_context_t) {
                                                   .file = tokenization->file_name,
                                                   .line = tokenization->cursor_line,
                                                   .column = tokenization->cursor_column,
                                               },
                                               format, variadic_arguments);
    va_end (variadic_arguments);
}

void cushion_tokenization_state_init_for_argument_string (struct cushion_tokenization_state_t *state,
                                                          const char *string,
                                                          struct cushion_allocator_t *allocator,
                                                          enum cushion_allocation_class_t allocation_class)
{
    state->file_name = "<argument-string>";
    state->state = CUSHION_TOKENIZATION_MODE_REGULAR; // Already a part of define, not a new line, actually.
    state->flags = 0u;

    const size_t length = strlen (string);
    // We don't actually change limit if there is no refill, so it is okay to cast.
    state->limit = (char *) string + length;
    state->cursor = string;
    state->marker = string;
    state->token = string;

#if defined(CUSHION_EXTENSIONS)
    state->guardrail_defer = NULL;
    state->guardrail_defer_base = NULL;

    state->guardrail_statement_accumulator = NULL;
    state->guardrail_statement_accumulator_base = NULL;
#endif

    state->cursor_line = 1u;
    state->cursor_column = 1u;
    state->marker_line = 1u;
    state->marker_column = 1u;

    state->saved = NULL;
    state->saved_line = 1u;
    state->saved_column = 1u;
    state->input_file_optional = NULL;

    state->tags = cushion_allocator_allocate (allocator, sizeof (struct re2c_tags_t), _Alignof (struct re2c_tags_t),
                                              allocation_class);
}

void cushion_tokenization_state_init_for_file (struct cushion_tokenization_state_t *state,
                                               const char *path,
                                               FILE *file,
                                               struct cushion_allocator_t *allocator,
                                               enum cushion_allocation_class_t allocation_class)
{
    state->file_name = path;
    state->state = CUSHION_TOKENIZATION_MODE_NEW_LINE;
    state->flags = 0u;

    state->limit = state->input_buffer + CUSHION_INPUT_BUFFER_SIZE - 1u;
    state->cursor = state->input_buffer + CUSHION_INPUT_BUFFER_SIZE - 1u;
    state->marker = state->input_buffer + CUSHION_INPUT_BUFFER_SIZE - 1u;
    state->token = state->input_buffer + CUSHION_INPUT_BUFFER_SIZE - 1u;
    *state->limit = '\0';

#if defined(CUSHION_EXTENSIONS)
    state->guardrail_defer = NULL;
    state->guardrail_defer_base = NULL;

    state->guardrail_statement_accumulator = NULL;
    state->guardrail_statement_accumulator_base = NULL;
#endif

    state->cursor_line = 1u;
    state->cursor_column = 1u;
    state->marker_line = 1u;
    state->marker_column = 1u;

    state->saved = NULL;
    state->saved_line = 1u;
    state->saved_column = 1u;
    state->input_file_optional = file;

    state->tags = cushion_allocator_allocate (allocator, sizeof (struct re2c_tags_t), _Alignof (struct re2c_tags_t),
                                              allocation_class);
}

static enum cushion_internal_result_t re2c_refill_buffer (struct cushion_instance_t *instance,
                                                          struct cushion_tokenization_state_t *state)
{
    if (!state->input_file_optional)
    {
        // No file -> no refill, it is that simple.
        return CUSHION_INTERNAL_RESULT_FAILED;
    }

    const char *preserve_from = state->token;
    if (state->saved && state->saved < preserve_from)
    {
        preserve_from = state->saved;
    }

#if defined(CUSHION_EXTENSIONS)
    const char *guardrail_feature_name = NULL;

    if (state->guardrail_defer && state->guardrail_defer < preserve_from)
    {
        preserve_from = state->guardrail_defer;
        guardrail_feature_name = "defer";
    }

    if (state->guardrail_statement_accumulator && state->guardrail_statement_accumulator < preserve_from)
    {
        preserve_from = state->guardrail_statement_accumulator;
        guardrail_feature_name = "statement accumulator";
    }
#endif

    const size_t shift = preserve_from - state->input_buffer;
    const size_t used = state->limit - preserve_from;

    if (shift == 0u)
    {
        if (used == 0u)
        {
            // Not a lexeme overflow, just end of file.
            return CUSHION_INTERNAL_RESULT_FAILED;
        }

#if defined(CUSHION_EXTENSIONS)
        if (guardrail_feature_name)
        {
            cushion_instance_tokenization_error (instance, state,
                                                 "Encountered lexeme overflow from guardrail for the %s feature.",
                                                 guardrail_feature_name);
        }
        else
#endif
        {
            cushion_instance_tokenization_error (instance, state, "Encountered lexeme overflow.");
        }

        return CUSHION_INTERNAL_RESULT_FAILED;
    }

    // Shift buffer contents (discard everything up to the current token).
    memmove (state->input_buffer, preserve_from, used);
    state->limit -= shift;
    state->cursor -= shift;
    state->marker -= shift;
    state->token -= shift;

#if defined(CUSHION_EXTENSIONS)
    if (state->guardrail_defer)
    {
        state->guardrail_defer -= shift;
    }

    if (state->guardrail_statement_accumulator)
    {
        state->guardrail_statement_accumulator -= shift;
    }
#endif

    if (state->saved)
    {
        state->saved -= shift;
    }

#if !defined(_MSC_VER) || defined(__clang__)
#    pragma GCC diagnostic push
    // Looks like we have false-positive here on GCC.
#    pragma GCC diagnostic ignored "-Warray-bounds"
#endif

    const char **first_tag = (const char **) state->tags;
    const char **last_tag = first_tag + sizeof (struct re2c_tags_t) / sizeof (char *);

    while (first_tag != last_tag)
    {
        if (*first_tag)
        {
            *first_tag -= shift;
        }

        ++first_tag;
    }

#if !defined(_MSC_VER) || defined(__clang__)
#    pragma GCC diagnostic pop
#endif

    // Fill free space at the end of buffer with new data from file.
    unsigned long read =
        (unsigned long) fread (state->limit, 1u, CUSHION_INPUT_BUFFER_SIZE - used - 1u, state->input_file_optional);

    if (read == 0u)
    {
        // End of file, return non-zero code and re2c will process it properly.
        *state->limit = '\0';
        return CUSHION_INTERNAL_RESULT_FAILED;
    }

    state->limit += read;
    *state->limit = '\0';
    return CUSHION_INTERNAL_RESULT_OK;
}

static inline void re2c_yyskip (struct cushion_tokenization_state_t *state)
{
    if (*state->cursor == '\n')
    {
        ++state->cursor_line;
        state->cursor_column = 0u;
    }

    ++state->cursor;
    ++state->cursor_column;
}

static inline void re2c_yybackup (struct cushion_tokenization_state_t *state)
{
    state->marker = state->cursor;
    state->marker_line = state->cursor_line;
    state->marker_column = state->cursor_column;
}

static inline void re2c_yyrestore (struct cushion_tokenization_state_t *state)
{
    state->cursor = state->marker;
    state->cursor_line = state->marker_line;
    state->cursor_column = state->marker_column;
}

static inline void re2c_save_cursor (struct cushion_tokenization_state_t *state)
{
    state->saved = state->cursor;
    state->saved_line = state->cursor_line;
    state->saved_column = state->cursor_column;
}

static inline void re2c_clear_saved_cursor (struct cushion_tokenization_state_t *state)
{
    state->saved = NULL;
    state->saved_line = 0u;
    state->saved_column = 0u;
}

static inline void re2c_restore_saved_cursor (struct cushion_tokenization_state_t *state)
{
    state->cursor = state->saved;
    state->cursor_line = state->saved_line;
    state->cursor_column = state->saved_column;
}

/*!re2c
 re2c:api = custom;
 re2c:api:style = free-form;
 re2c:define:YYCTYPE = "unsigned char";
 re2c:define:YYLESSTHAN = "state->limit - state->cursor < @@{len}";
 re2c:define:YYPEEK = "*state->cursor";
 re2c:define:YYSKIP = "re2c_yyskip (state);";
 re2c:define:YYBACKUP = "re2c_yybackup (state);";
 re2c:define:YYRESTORE = "re2c_yyrestore (state);";
 re2c:define:YYFILL = "re2c_refill_buffer (instance, state) == CUSHION_INTERNAL_RESULT_OK";
 re2c:define:YYSTAGP = "@@{tag} = state->cursor;";
 re2c:define:YYSTAGN = "@@{tag} = NULL;";
 re2c:define:YYSHIFTSTAG  = "@@{tag} += @@{shift};";
 re2c:eof = 0;
 re2c:tags = 1;
 re2c:flags:utf-8 = 1;
 re2c:flags:case-ranges = 0;
 re2c:tags:expression = "state->tags->@@";

 id_start = [a-zA-Z_];
 id_continue = [a-zA-Z_0-9]*;
 */

// Non-ASCII characters are not supported in code as we do not expect them in identifiers.
/*!rules:re2c:check_unsupported_in_code
 [^\x00-\x7F]
 {
     cushion_instance_tokenization_error (
         instance, state,
         "Encountered non-ASCII character outside of comments and string literals."
         "This version of Cushion is built without unicode support for anything outside of comments and literals.");
     return;
 }
 */

struct cushion_token_list_item_t *cushion_save_token_to_memory (struct cushion_instance_t *instance,
                                                                const struct cushion_token_t *token,
                                                                enum cushion_allocation_class_t allocation_class)
{
    struct cushion_token_list_item_t *target =
        cushion_allocator_allocate (&instance->allocator, sizeof (struct cushion_token_list_item_t),
                                    _Alignof (struct cushion_token_list_item_t), allocation_class);

    target->next = NULL;
    // By default, file and line data is not initialized and initialization is left to the user.
    target->file = NULL;
    target->line = 0u;

#if defined(CUSHION_EXTENSIONS)
    target->flags = CUSHION_TOKEN_LIST_ITEM_FLAG_NONE;
#endif

    target->token.type = token->type;
    target->token.begin =
        cushion_instance_copy_char_sequence_inside (instance, token->begin, token->end, allocation_class);
    target->token.end = target->token.begin + (token->end - token->begin);

    // Now properly recalculate subsequences to make sure that they point to copied out text.
    switch (token->type)
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
    case CUSHION_TOKEN_TYPE_PREPROCESSOR_DEFINE:
    case CUSHION_TOKEN_TYPE_PREPROCESSOR_UNDEF:
    case CUSHION_TOKEN_TYPE_PREPROCESSOR_LINE:
    case CUSHION_TOKEN_TYPE_PREPROCESSOR_PRAGMA:
    case CUSHION_TOKEN_TYPE_NUMBER_FLOATING:
    case CUSHION_TOKEN_TYPE_DIGIT_IDENTIFIER_SEQUENCE:
    case CUSHION_TOKEN_TYPE_NEW_LINE:
    case CUSHION_TOKEN_TYPE_GLUE:
    case CUSHION_TOKEN_TYPE_COMMENT:
    case CUSHION_TOKEN_TYPE_END_OF_FILE:
    case CUSHION_TOKEN_TYPE_OTHER:
        break;

    case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
    case CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
        target->token.header_path.begin = target->token.begin + (token->header_path.begin - token->begin);
        target->token.header_path.end = target->token.begin + (token->header_path.end - token->begin);
        break;

    case CUSHION_TOKEN_TYPE_IDENTIFIER:
        target->token.identifier_kind = token->identifier_kind;
        break;

    case CUSHION_TOKEN_TYPE_PUNCTUATOR:
        target->token.punctuator_kind = token->punctuator_kind;
        break;

    case CUSHION_TOKEN_TYPE_NUMBER_INTEGER:
        target->token.unsigned_number_value = token->unsigned_number_value;
        break;

    case CUSHION_TOKEN_TYPE_CHARACTER_LITERAL:
    case CUSHION_TOKEN_TYPE_STRING_LITERAL:
        target->token.symbolic_literal.encoding = token->symbolic_literal.encoding;
        target->token.symbolic_literal.begin = target->token.begin + (token->symbolic_literal.begin - token->begin);
        target->token.symbolic_literal.end = target->token.begin + (token->symbolic_literal.end - token->begin);
        break;
    }

    return target;
}

static enum cushion_internal_result_t tokenize_decimal_value (const char *begin,
                                                              const char *end,
                                                              unsigned long long *output)
{
    unsigned long long result = 0u;
    while (begin < end)
    {
        unsigned long long result_before = result;
        switch (*begin)
        {
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            result = result * 10u + (*begin - '0');
            break;

        default:
            break;
        }

        if (result < result_before)
        {
            // Overflow.
            return CUSHION_INTERNAL_RESULT_FAILED;
        }

        ++begin;
    }

    *output = result;
    return CUSHION_INTERNAL_RESULT_OK;
}

static enum cushion_internal_result_t tokenize_octal_value (const char *begin,
                                                            const char *end,
                                                            unsigned long long *output)
{
    unsigned long long result = 0u;
    while (begin < end)
    {
        unsigned long long result_before = result;
        switch (*begin)
        {
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
            result = result * 8u + (*begin - '0');
            break;

        default:
            break;
        }

        if (result < result_before)
        {
            // Overflow.
            return CUSHION_INTERNAL_RESULT_FAILED;
        }

        ++begin;
    }

    *output = result;
    return CUSHION_INTERNAL_RESULT_OK;
}

static enum cushion_internal_result_t tokenize_hex_value (const char *begin,
                                                          const char *end,
                                                          unsigned long long *output)
{
    unsigned long long result = 0u;
    while (begin < end)
    {
        unsigned long long result_before = result;
        switch (*begin)
        {
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            result = result * 16u + (*begin - '0');
            break;

        case 'A':
        case 'B':
        case 'C':
        case 'D':
        case 'E':
        case 'F':
            result = result * 16u + (*begin - 'A');
            break;

        case 'a':
        case 'b':
        case 'c':
        case 'd':
        case 'e':
        case 'f':
            result = result * 16u + (*begin - 'a');
            break;

        default:
            break;
        }

        if (result < result_before)
        {
            // Overflow.
            return CUSHION_INTERNAL_RESULT_FAILED;
        }

        ++begin;
    }

    *output = result;
    return CUSHION_INTERNAL_RESULT_OK;
}

static enum cushion_internal_result_t tokenize_binary_value (const char *begin,
                                                             const char *end,
                                                             unsigned long long *output)
{
    unsigned long long result = 0u;
    while (begin < end)
    {
        unsigned long long result_before = result;
        switch (*begin)
        {
        case '0':
        case '1':
            result = result * 2u + (*begin - '0');
            break;

        default:
            break;
        }

        if (result < result_before)
        {
            // Overflow.
            return CUSHION_INTERNAL_RESULT_FAILED;
        }

        ++begin;
    }

    *output = result;
    return CUSHION_INTERNAL_RESULT_OK;
}

void cushion_tokenization_next_token (struct cushion_instance_t *instance,
                                      struct cushion_tokenization_state_t *state,
                                      struct cushion_token_t *output)
{
    const char *marker_sub_begin = NULL;
    const char *marker_sub_end = NULL;

    /*!re2c
     new_line = [\x0d]? [\x0a];
     whitespace = [\x09\x0b\x0c\x0d\x20];
     backslash = [\x5c];
     identifier = id_start id_continue*;
     multi_line_comment = "/" "*" ([^*] | ("*" [^/]))* "*" "/";
     */

start_next_token:
    state->token = state->cursor;

#define PREPROCESSOR_EMIT_TOKEN(TOKEN)                                                                                 \
    re2c_clear_saved_cursor (state);                                                                                   \
    output->type = TOKEN;                                                                                              \
    output->begin = state->token;                                                                                      \
    output->end = state->cursor;                                                                                       \
    return

#define PREPROCESSOR_EMIT_TOKEN_IDENTIFIER(KIND)                                                                       \
    output->identifier_kind = KIND;                                                                                    \
    PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_IDENTIFIER)

#define PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR(KIND)                                                                       \
    output->punctuator_kind = KIND;                                                                                    \
    PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_PUNCTUATOR)

#define PREPROCESSOR_EMIT_TOKEN_CHARACTER_LITERAL(ENCODING)                                                            \
    output->symbolic_literal.encoding = ENCODING;                                                                      \
    output->symbolic_literal.begin = marker_sub_begin;                                                                 \
    output->symbolic_literal.end = marker_sub_end;                                                                     \
    PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_CHARACTER_LITERAL)

#define PREPROCESSOR_EMIT_TOKEN_STRING_LITERAL(ENCODING)                                                               \
    output->symbolic_literal.encoding = ENCODING;                                                                      \
    output->symbolic_literal.begin = marker_sub_begin;                                                                 \
    output->symbolic_literal.end = marker_sub_end;                                                                     \
    PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_STRING_LITERAL)

    switch (state->state)
    {
    case CUSHION_TOKENIZATION_MODE_REGULAR:
    {
    regular_routine:
        if (state->flags & CUSHION_TOKENIZATION_FLAGS_SKIP_REGULAR)
        {
            // Separate routine for breezing through anything that is not a preprocessor directive.
        skip_regular_routine:
            state->token = state->cursor;

            /*!re2c
             new_line { state->state = CUSHION_TOKENIZATION_MODE_NEW_LINE; goto start_next_token; }
             * { goto skip_regular_routine; }
             $ { PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_END_OF_FILE); }
             */
        }

        /*!re2c
         whitespace+ { PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_GLUE); }
         "\\" new_line { PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_GLUE); }

         "//" [^\n]* { PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_COMMENT); }
         multi_line_comment { PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_COMMENT); }

         new_line
         {
             state->state = CUSHION_TOKENIZATION_MODE_NEW_LINE;
             PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_NEW_LINE);
         }

         "__VA_ARGS__" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_VA_ARGS); }
         "__VA_OPT__" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_VA_OPT); }

         "__CUSHION_PRESERVE__" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_CUSHION_PRESERVE); }
         "CUSHION_DEFER" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_CUSHION_DEFER); }
         "__CUSHION_WRAPPED__" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_CUSHION_WRAPPED); }
         "CUSHION_STATEMENT_ACCUMULATOR"
         { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR); }
         "CUSHION_STATEMENT_ACCUMULATOR_PUSH"
         { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_PUSH); }
         "CUSHION_STATEMENT_ACCUMULATOR_REF"
         { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_REF); }
         "CUSHION_STATEMENT_ACCUMULATOR_UNREF"
         { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_UNREF); }
         "CUSHION_SNIPPET" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_CUSHION_SNIPPET); }
         "__CUSHION_EVALUATED_ARGUMENT__"
         { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_CUSHION_EVALUATED_ARGUMENT); }
         "__CUSHION_REPLACEMENT_INDEX__"
         { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_CUSHION_REPLACEMENT_INDEX); }

         "__FILE__" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_FILE); }
         "__LINE__" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_LINE); }

         "defined" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_DEFINED); }
         "__has_include" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_HAS_INCLUDE); }
         "__has_embed" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_HAS_EMBED); }
         "__has_c_attribute" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_HAS_C_ATTRIBUTE); }
         "_Pragma" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_MACRO_PRAGMA); }

         "if" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_IF); }
         "for" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_FOR); }
         "while" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_WHILE); }
         "do" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_DO); }
         "switch" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_SWITCH); }

         "return" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_RETURN); }
         "break" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_BREAK); }
         "continue" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_CONTINUE); }
         "goto" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_GOTO); }

         "default" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_DEFAULT); }

         identifier { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (CUSHION_IDENTIFIER_KIND_REGULAR); }

         "[" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_LEFT_SQUARE_BRACKET); }
         "]" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_RIGHT_SQUARE_BRACKET); }

         "(" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS); }
         ")" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS); }

         "{" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_LEFT_CURLY_BRACE); }
         "}" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_RIGHT_CURLY_BRACE); }

         "." { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_MEMBER_ACCESS); }
         "->" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_POINTER_ACCESS); }

         "++" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_INCREMENT); }
         "--" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_DECREMENT); }

         "&" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_BITWISE_AND); }
         "|" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_BITWISE_OR); }
         "^" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_BITWISE_XOR); }
         "~" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_BITWISE_INVERSE); }

         "+" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_PLUS); }
         "-" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_MINUS); }
         "*" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_MULTIPLY); }
         "/" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_DIVIDE); }
         "%" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_MODULO); }

         "!" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_LOGICAL_NOT); }
         "&&" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_LOGICAL_AND); }
         "||" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_LOGICAL_OR); }
         "<" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_LOGICAL_LESS); }
         ">" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_LOGICAL_GREATER); }
         "<=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_LOGICAL_LESS_OR_EQUAL); }
         ">=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_LOGICAL_GREATER_OR_EQUAL); }
         "==" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_LOGICAL_EQUAL); }
         "!=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_LOGICAL_NOT_EQUAL); }

         "<<" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_LEFT_SHIFT); }
         ">>" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_RIGHT_SHIFT); }

         "?" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_QUESTION_MARK); }
         ":" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_COLON); }
         "::" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_DOUBLE_COLON); }
         ";" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_SEMICOLON); }
         "," { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_COMMA); }
         "..." { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_TRIPLE_DOT); }
         "#" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_HASH); }
         "##" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_DOUBLE_HASH); }

         "=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_ASSIGN); }
         "+=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_PLUS_ASSIGN); }
         "-=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_MINUS_ASSIGN); }
         "*=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_MULTIPLY_ASSIGN); }
         "/=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_DIVIDE_ASSIGN); }
         "<<=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_LEFT_SHIFT_ASSIGN); }
         ">>=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_RIGHT_SHIFT_ASSIGN); }
         "&=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_BITWISE_AND_ASSIGN); }
         "|=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_BITWISE_OR_ASSIGN); }
         "^=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (CUSHION_PUNCTUATOR_KIND_BITWISE_XOR_ASSIGN); }

         unsigned_integer_suffix = [uU];
         long_integer_suffix = [lL];
         long_long_integer_suffix = "ll" | "LL";
         bit_precise_integer_suffix = "wb" | "WB";
         integer_suffix =
             (unsigned_integer_suffix? (long_integer_suffix | long_long_integer_suffix | bit_precise_integer_suffix)) |
             (long_integer_suffix? unsigned_integer_suffix) |
             (long_long_integer_suffix? unsigned_integer_suffix) |
             (bit_precise_integer_suffix? unsigned_integer_suffix);

         decimal_integer = @marker_sub_begin ("0" | ([1-9] [0-9']*)) @marker_sub_end;
         octal_integer = "0" [oO]? @marker_sub_begin [0-7']+ @marker_sub_end;
         hex_integer = "0" [xX] @marker_sub_begin [0-9a-fA-F']+ @marker_sub_end;
         binary_integer = "0" [bB] @marker_sub_begin [01']+ @marker_sub_end;

         decimal_integer integer_suffix?
         {
             if (tokenize_decimal_value (marker_sub_begin, marker_sub_end, &output->unsigned_number_value) !=
                 CUSHION_INTERNAL_RESULT_OK)
             {
                 cushion_instance_tokenization_error (instance, state, "Failed to parse number due to overflow.");
                 return;
             }

             PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_NUMBER_INTEGER);
         }

         octal_integer integer_suffix?
         {
             if (tokenize_octal_value (marker_sub_begin, marker_sub_end, &output->unsigned_number_value) !=
                 CUSHION_INTERNAL_RESULT_OK)
             {
                 cushion_instance_tokenization_error (instance, state, "Failed to parse number due to overflow.");
                 return;
             }

             PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_NUMBER_INTEGER);
         }

         hex_integer integer_suffix?
         {
             if (tokenize_hex_value (marker_sub_begin, marker_sub_end, &output->unsigned_number_value) !=
                 CUSHION_INTERNAL_RESULT_OK)
             {
                 cushion_instance_tokenization_error (instance, state, "Failed to parse number due to overflow.");
                 return;
             }

             PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_NUMBER_INTEGER);
         }

         binary_integer integer_suffix?
         {
             if (tokenize_binary_value (marker_sub_begin, marker_sub_end, &output->unsigned_number_value) !=
                 CUSHION_INTERNAL_RESULT_OK)
             {
                 cushion_instance_tokenization_error (instance, state, "Failed to parse number due to overflow.");
                 return;
             }

             PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_NUMBER_INTEGER);
         }

         digit_sequence = [0-9] [0-9']*;
         hex_digit_sequence = [0-9a-fA-F] [0-9a-fA-F']*;
         real_floating_suffix = [fF] | [lL] | "df" | "dd" | "dl" | "DF" | "DD" | "DL";
         complex_floating_suffix = [iIjJ];
         floating_suffix =
             (real_floating_suffix complex_floating_suffix?) |
             (complex_floating_suffix real_floating_suffix?);

         // Decimal floating literal.
         ((digit_sequence? "." digit_sequence) | (digit_sequence "."?)) ([eE] "-"? digit_sequence)? floating_suffix?
         {
             PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_NUMBER_FLOATING);
         }

         // Hexadecimal floating literal.
         "0" [xX] ((hex_digit_sequence? "." hex_digit_sequence) | (hex_digit_sequence "."?))
         [pP] "-"? digit_sequence floating_suffix?
         {
             PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_NUMBER_FLOATING);
         }

         [0-9] identifier
         {
             PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_DIGIT_IDENTIFIER_SEQUENCE);
         }

         simple_escape_sequence = "\\" (['"?\\abfnrtv] | ([0-9]+));
         // For now, we only support simple escape sequences, but that might be changed in the future.
         escape_sequence = simple_escape_sequence;
         character_literal_sequence = (escape_sequence | [^'\\\n])*;
         string_literal_sequence = (escape_sequence | [^"\\\n])*;

         "'" @marker_sub_begin character_literal_sequence @marker_sub_end "'"
         {
             PREPROCESSOR_EMIT_TOKEN_CHARACTER_LITERAL (CUSHION_TOKEN_SUBSEQUENCE_ENCODING_ORDINARY);
         }

         "u8'" @marker_sub_begin character_literal_sequence @marker_sub_end "'"
         {
             PREPROCESSOR_EMIT_TOKEN_CHARACTER_LITERAL (CUSHION_TOKEN_SUBSEQUENCE_ENCODING_UTF8);
         }

         "u'" @marker_sub_begin character_literal_sequence @marker_sub_end "'"
         {
             PREPROCESSOR_EMIT_TOKEN_CHARACTER_LITERAL (CUSHION_TOKEN_SUBSEQUENCE_ENCODING_UTF16);
         }

         "U'" @marker_sub_begin character_literal_sequence @marker_sub_end "'"
         {
             PREPROCESSOR_EMIT_TOKEN_CHARACTER_LITERAL (CUSHION_TOKEN_SUBSEQUENCE_ENCODING_UTF32);
         }

         "L'" @marker_sub_begin character_literal_sequence @marker_sub_end "'"
         {
             PREPROCESSOR_EMIT_TOKEN_CHARACTER_LITERAL (CUSHION_TOKEN_SUBSEQUENCE_ENCODING_WIDE);
         }

         "\"" @marker_sub_begin string_literal_sequence @marker_sub_end "\""
         {
             PREPROCESSOR_EMIT_TOKEN_STRING_LITERAL (CUSHION_TOKEN_SUBSEQUENCE_ENCODING_ORDINARY);
         }

         "u8\"" @marker_sub_begin string_literal_sequence @marker_sub_end "\""
         {
             PREPROCESSOR_EMIT_TOKEN_STRING_LITERAL (CUSHION_TOKEN_SUBSEQUENCE_ENCODING_UTF8);
         }

         "u\"" @marker_sub_begin string_literal_sequence @marker_sub_end "\""
         {
             PREPROCESSOR_EMIT_TOKEN_STRING_LITERAL (CUSHION_TOKEN_SUBSEQUENCE_ENCODING_UTF16);
         }

         "U\"" @marker_sub_begin string_literal_sequence @marker_sub_end "\""
         {
             PREPROCESSOR_EMIT_TOKEN_STRING_LITERAL (CUSHION_TOKEN_SUBSEQUENCE_ENCODING_UTF32);
         }

         "L\"" @marker_sub_begin string_literal_sequence @marker_sub_end "\""
         {
             PREPROCESSOR_EMIT_TOKEN_STRING_LITERAL (CUSHION_TOKEN_SUBSEQUENCE_ENCODING_WIDE);
         }

         !use:check_unsupported_in_code;
         * { PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_OTHER); }
         $ { PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_END_OF_FILE); }
         */

        break;
    }

    case CUSHION_TOKENIZATION_MODE_NEW_LINE:
    {
        // Reset state to regular as next tokens would use regular anyway.
        state->state = CUSHION_TOKENIZATION_MODE_REGULAR;
        goto new_line_check_for_preprocessor_begin;

    new_line_preprocessor_found:
        re2c_save_cursor (state);

    new_line_preprocessor_determine_type:
        state->token = state->cursor;

        /*!re2c
         !use:check_unsupported_in_code;

         // Whitespaces and comments prepending preprocessor command are just skipped.
         whitespace+ { goto new_line_preprocessor_determine_type; }
         multi_line_comment { goto new_line_preprocessor_determine_type; }

         "if" { PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_PREPROCESSOR_IF); }
         "ifdef" { PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_PREPROCESSOR_IFDEF); }
         "ifndef" { PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_PREPROCESSOR_IFNDEF); }
         "elif" { PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIF); }
         "elifdef" { PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFDEF); }
         "elifndef" { PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFNDEF); }
         "else" { PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_PREPROCESSOR_ELSE); }
         "endif" { PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_PREPROCESSOR_ENDIF); }

         "include"
         {
             state->state = CUSHION_TOKENIZATION_MODE_INCLUDE;
             PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_PREPROCESSOR_INCLUDE);
         }

         "define" { PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_PREPROCESSOR_DEFINE); }
         "undef" { PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_PREPROCESSOR_UNDEF); }
         "line"
         {
             state->state = CUSHION_TOKENIZATION_MODE_LINE;
             PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_PREPROCESSOR_LINE);
         }

         "pragma" { PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_PREPROCESSOR_PRAGMA); }

         identifier { goto new_line_preprocessor_do_not_care; }
         * { goto new_line_preprocessor_do_not_care; }
         $ { goto new_line_preprocessor_do_not_care; }
         */

        // This is a not preprocessor things that we care about. Just lex as hash and continue.
    new_line_preprocessor_do_not_care:
        if (state->flags & CUSHION_TOKENIZATION_FLAGS_SKIP_REGULAR)
        {
            // If we're skipping regulars, no need to output punctuator.
            goto skip_regular_routine;
        }

        re2c_restore_saved_cursor (state);
        state->token = state->cursor;

        output->type = CUSHION_TOKEN_TYPE_PUNCTUATOR;
        output->end = state->cursor;
        output->punctuator_kind = CUSHION_PUNCTUATOR_KIND_HASH;
        return;

    new_line_check_for_preprocessor_begin:
        re2c_save_cursor (state);

        /*!re2c
         "#" { goto new_line_preprocessor_found; }
         * { goto new_line_check_for_preprocessor_no_preprocessor; }
         $ { goto new_line_check_for_preprocessor_no_preprocessor; }
         */

    new_line_check_for_preprocessor_no_preprocessor:
        re2c_restore_saved_cursor (state);
        // Nothing specific for new line found.
        goto regular_routine;
    }

    case CUSHION_TOKENIZATION_MODE_INCLUDE:
    {
        if (state->flags & CUSHION_TOKENIZATION_FLAGS_SKIP_REGULAR)
        {
            goto skip_regular_routine;
        }

        re2c_save_cursor (state);
    include_routine:
        state->token = state->cursor;

        /*!re2c
         // Whitespaces and comments prepending include path are just skipped.
         whitespace+ { goto include_routine; }
         multi_line_comment { goto include_routine; }

         "<" @marker_sub_begin [^\n>]+ @marker_sub_end ">"
         {
             output->header_path.begin = marker_sub_begin;
             output->header_path.end = marker_sub_end;
             PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM);
         }

         "\"" @marker_sub_begin [^\n"]+ @marker_sub_end "\""
         {
             output->header_path.begin = marker_sub_begin;
             output->header_path.end = marker_sub_end;
             PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_USER);
         }

         * { goto include_not_header_path; }
         $ { goto include_not_header_path; }
         */

    include_not_header_path:
        // Nothing specific for the include directive found.
        re2c_restore_saved_cursor (state);
        state->token = state->cursor;
        goto regular_routine;
    }

    case CUSHION_TOKENIZATION_MODE_LINE:
    {
        if (state->flags & CUSHION_TOKENIZATION_FLAGS_SKIP_REGULAR)
        {
            goto skip_regular_routine;
        }

        re2c_save_cursor (state);
    line_routine:
        state->token = state->cursor;

        /*!re2c
         // Whitespaces and comments in line directive are just skipped.
         whitespace+ { goto line_routine; }
         multi_line_comment { goto line_routine; }

         // Line number in format supported for #line by standard.
         @marker_sub_begin [0-9]+ @marker_sub_end
         {
             if (tokenize_decimal_value (marker_sub_begin, marker_sub_end, &output->unsigned_number_value) !=
                 CUSHION_INTERNAL_RESULT_OK)
             {
                 cushion_instance_tokenization_error (instance, state, "Failed to line number for #line directive.");
                 return;
             }

             PREPROCESSOR_EMIT_TOKEN (CUSHION_TOKEN_TYPE_NUMBER_INTEGER);
         }

         // Unsupported number formats for #line.
         "0" [oOxXbB]? @marker_sub_begin [0-9a-fA-F']+ @marker_sub_end
         {
             cushion_instance_tokenization_error (instance, state,
                     "Got line number in #line directive in format unsupported by standard.");
             return;
         }

         * { goto line_not_a_line_number; }
         $ { goto line_not_a_line_number; }
         */

    line_not_a_line_number:
        // Nothing specific for the line directive found.
        re2c_restore_saved_cursor (state);
        state->token = state->cursor;
        goto regular_routine;
    }
    }

#undef PREPROCESSOR_EMIT_TOKEN_CHARACTER_LITERAL
#undef PREPROCESSOR_EMIT_TOKEN_STRING_LITERAL
#undef PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR
#undef PREPROCESSOR_EMIT_TOKEN_IDENTIFIER
#undef PREPROCESSOR_EMIT_TOKEN

    cushion_instance_tokenization_error (instance, state, "Unexpected way to exit tokenizer, internal error.");
}
