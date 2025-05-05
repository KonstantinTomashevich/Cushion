#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cushion.h>

// Common generic utility functions.

static inline uintptr_t apply_alignment (uintptr_t address_or_size, uintptr_t alignment)
{
    const uintptr_t modulo = address_or_size % alignment;
    if (modulo != 0u)
    {
        address_or_size += alignment - modulo;
    }

    return address_or_size;
}

static inline unsigned int hash_djb2_char_sequence (const char *begin, const char *end)
{
    unsigned int hash_value = 5381u;
    while (begin != end)
    {
        hash_value = (hash_value << 5u) + hash_value + (unsigned char) *begin;
        ++begin;
    }

    return hash_value;
}

static inline unsigned int hash_djb2_null_terminated (const char *string)
{
    return hash_djb2_char_sequence (string, string + strlen (string));
}

enum result_t
{
    RESULT_OK = 0u,
    RESULT_FAILED = 1u,
};

// Memory management section: common utility for memory management.

struct stack_group_allocator_t
{
    struct stack_group_allocator_page_t *first_page;
    struct stack_group_allocator_page_t *current_page;
};

struct stack_group_allocator_page_t
{
    struct stack_group_allocator_page_t *next;
    void *top;
    uint8_t data[CUSHION_ALLOCATOR_PAGE_SIZE];
};

static void stack_group_allocator_init (struct stack_group_allocator_t *instance)
{
    instance->first_page = malloc (sizeof (struct stack_group_allocator_page_t));
    instance->first_page->next = NULL;
    instance->first_page->top = instance->first_page->data;
    instance->current_page = instance->first_page;
}

static void *stack_group_allocator_page_allocate (struct stack_group_allocator_page_t *page,
                                                  uintptr_t size,
                                                  uintptr_t alignment)
{
    uint8_t *address = (uint8_t *) apply_alignment ((uintptr_t) page->top, alignment);
    uint8_t *new_top = address + size;

    if (new_top <= page->data + CUSHION_ALLOCATOR_PAGE_SIZE)
    {
        page->top = new_top;
        return address;
    }

    return NULL;
}

static void *stack_group_allocator_allocate (struct stack_group_allocator_t *allocator,
                                             uintptr_t size,
                                             uintptr_t alignment)
{
    void *result = stack_group_allocator_page_allocate (allocator->current_page, size, alignment);
    if (!result)
    {
        if (!allocator->current_page->next)
        {
            struct stack_group_allocator_page_t *new_page = malloc (sizeof (struct stack_group_allocator_page_t));
            new_page->next = NULL;
            new_page->top = new_page->data;
            allocator->current_page->next = new_page;
        }
        else
        {
            allocator->current_page = allocator->current_page->next;
        }

        result = stack_group_allocator_page_allocate (allocator->current_page, size, alignment);
    }

    if (!result)
    {
        fprintf (stderr, "Internal error: failed to allocate %lu bytes with %lu alignment.", (unsigned long) size,
                 (unsigned long) alignment);
        abort ();
    }

    return result;
}

static void stack_group_allocator_reset (struct stack_group_allocator_t *allocator)
{
    struct stack_group_allocator_page_t *page = allocator->first_page;
    while (page)
    {
        page->top = page->data;
        page = page->next;
    }

    allocator->current_page = allocator->first_page;
}

static void stack_group_allocator_shrink (struct stack_group_allocator_t *allocator)
{
    struct stack_group_allocator_page_t *page = allocator->current_page->next;
    allocator->current_page->next = NULL;

    while (page)
    {
        struct stack_group_allocator_page_t *next = page->next;
        free (page);
        page = next;
    }
}

static void stack_group_allocator_shutdown (struct stack_group_allocator_t *allocator)
{
    struct stack_group_allocator_page_t *page = allocator->first_page;
    while (page)
    {
        struct stack_group_allocator_page_t *next = page->next;
        free (page);
        page = next;
    }
}

// Context implementation generic part.

enum context_state_flag_t
{
    CONTEXT_STATE_FLAG_EXECUTION = 1u << 0u,
    CONTEXT_STATE_FLAG_ERRED = 1u << 1u,
};

struct context_t
{
    unsigned int state_flags;
    unsigned int features;
    unsigned int options;

    struct include_node_t *includes_first;
    struct include_node_t *includes_last;

    struct macro_node_t *macro_buckets[CUSHION_MACRO_BUCKETS];

    struct stack_group_allocator_t allocator;

    struct input_node_t *inputs_first;
    struct input_node_t *inputs_last;

    FILE *output;
    FILE *cmake_depfile_output;

    char *output_path;
    char *cmake_depfile_path;
};

enum include_type_t
{
    INCLUDE_TYPE_FULL = 0u,
    INCLUDE_TYPE_SCAN,
};

struct include_node_t
{
    struct include_node_t *next;
    enum include_type_t type;
    char *path;
};

struct input_node_t
{
    struct input_node_t *next;
    char *path;
};

struct macro_node_t
{
    struct macro_node_t *next;
    unsigned int name_hash;
    const char *name;
    const char *value;

    /// \details Macro parameters can be treated just as temporary macros for the most cases.
    ///          We would just change their value during every macro instantiation.
    ///          And we would only use them during initial macro instantiation pass.
    struct macro_node_t *parameters_first;

    // TODO: Variadic arguments.
};

static inline void context_clean_configuration (struct context_t *instance)
{
    instance->state_flags = 0u;
    instance->features = 0u;
    instance->options = 0u;
    instance->includes_first = NULL;
    instance->includes_last = NULL;

    instance->inputs_first = NULL;
    instance->inputs_last = NULL;
    instance->output_path = NULL;
    instance->cmake_depfile_path = NULL;

    for (unsigned int index = 0u; index < CUSHION_MACRO_BUCKETS; ++index)
    {
        instance->macro_buckets[index] = NULL;
    }
}

static inline char *context_copy_string_inside (struct context_t *instance, const char *string)
{
    size_t length = strlen (string);
    char *copied = stack_group_allocator_allocate (&instance->allocator, length + 1u, _Alignof (char));
    memcpy (copied, string, length);
    copied[length] = '\0';
    return copied;
}

static inline void context_includes_add (struct context_t *instance, struct include_node_t *node)
{
    node->next = NULL;
    if (instance->includes_last)
    {
        instance->includes_last->next = node;
        instance->includes_last = node;
    }
    else
    {
        instance->includes_first = node;
        instance->includes_last = node;
    }
}

static inline struct macro_node_t *macro_search_in_list (struct macro_node_t *list,
                                                         unsigned int name_hash,
                                                         const char *name)
{
    // We expect null-terminated name for ease of use and comparison.
    // Any char sequence can be temporary converted into null-terminated one by temporary changing last character.
    while (list)
    {
        if (list->name_hash == name_hash && strcmp (list->name, name) == 0)
        {
            return list;
        }

        list = list->next;
    }

    return NULL;
}

/* TODO: Implemented for the future, comment for now to silence unused function warning.
static struct macro_node_t *context_macro_search (struct context_t *instance, const char *name)
{
    unsigned int name_hash = hash_djb2_null_terminated (name);
    struct macro_node_t *list = instance->macro_buckets[name_hash % CUSHION_MACRO_BUCKETS];
    return macro_search_in_list (list, name_hash, name);
}
*/

struct re2c_state_t;

static inline void context_execution_error (struct context_t *instance,
                                            struct re2c_state_t *state,
                                            const char *format,
                                            ...);

static void context_macro_add (struct context_t *instance, struct macro_node_t *node)
{
    node->name_hash = hash_djb2_null_terminated (node->name);
    struct macro_node_t *bucket_list = instance->macro_buckets[node->name_hash % CUSHION_MACRO_BUCKETS];
    struct macro_node_t *already_here = macro_search_in_list (bucket_list, node->name_hash, node->name);

    if (already_here)
    {
        if ((instance->options & (1u << CUSHION_OPTION_FORBID_MACRO_REDEFINITION)) &&
            (instance->state_flags & CONTEXT_STATE_FLAG_EXECUTION))
        {
            context_execution_error (instance, NULL, "Encountered macro \"%s\" redefinition.", node->name);
        }
        else
        {
            // Just replace previous node content and exit.
            already_here->value = node->value;
            already_here->parameters_first = node->parameters_first;
        }

        return;
    }

    // New macro, just insert it.
    node->next = bucket_list;
    instance->macro_buckets[node->name_hash % CUSHION_MACRO_BUCKETS] = node;
}

/* TODO: Implemented for the future, comment for now to silence unused function warning.
static void context_macro_remove (struct context_t *instance, const char *name)
{
    unsigned int name_hash = hash_djb2_null_terminated (name);
    // Logic is almost the same as for search, but we need to keep previous pointer here for proper removal.
    struct macro_node_t *list = instance->macro_buckets[name_hash % CUSHION_MACRO_BUCKETS];
    struct macro_node_t *previous = NULL;

    while (list)
    {
        if (list->name_hash == name_hash && strcmp (list->name, name) == 0)
        {
            // Found it.
            // Removal is just pointer operation, as we keep all the garbage in stack group allocator for simplicity.

            if (previous)
            {
                previous->next = list->next;
            }
            else
            {
                instance->macro_buckets[name_hash % CUSHION_MACRO_BUCKETS] = list->next;
            }

            return;
        }

        previous = list;
        list = list->next;
    }
}
*/

static enum result_t context_output (struct context_t *instance, const char *begin, const char *end)
{
    if (instance->output)
    {
        size_t length = end - begin;
        return fwrite (begin, 1u, length, instance->output) == length ? RESULT_OK : RESULT_FAILED;
    }

    return RESULT_OK;
}

// re2c support section: structs and functions to properly setup re2c for tokenization.

struct re2c_tags_t
{
    /*!stags:re2c format = 'const char *@@;';*/
};

enum re2c_tokenization_state_t
{
    RE2C_TOKENIZATION_STATE_NEW_LINE = 0u,
    RE2C_TOKENIZATION_STATE_REGULAR,
};

enum re2c_tokenization_flags_t
{
    /// \brief If true, glue characters (white spaces and escaped newlines) that are not part of the tokens,
    ///        will be put directly to the output.
    RE2C_TOKENIZATION_FLAGS_OUTPUT_GLUE = 1u << 0u,

    /// \brief Simplified mode that skips any tokens that are not preprocessor directives.
    /// \details Useful for blazing through scan only headers and conditionally excluded source parts.
    ///          Should be disabled when tokenizing conditional expressions as their expressions are regular tokens.
    RE2C_TOKENIZATION_FLAGS_SKIP_REGULAR = 1u << 1u,
};

struct re2c_state_t
{
    const char *file_name;
    enum re2c_tokenization_state_t state;
    enum re2c_tokenization_flags_t flags;

    char *limit;
    const char *cursor;
    const char *marker;
    const char *token;

#if defined(CUSHION_EXTENSIONS)
    /// \brief If not NULL, prevents code after it from being lost during refill, the same way as token does.
    /// \details Guardrail is mostly needed to preserve previous token data if requested by the lexer.
    ///          However, lexer should use it carefully to avoid overflows.
    ///          It should never be needed when extensions are disabled, because usual preprocessing does not need
    ///          to look at previous tokens in any case.
    const char *guardrail;
#endif

    unsigned int cursor_line;
    unsigned int cursor_column;
    unsigned int marker_line;
    unsigned int marker_column;

    const char *saved;
    unsigned int saved_line;
    unsigned int saved_column;

    struct re2c_tags_t tags;

    FILE *input_file_optional;
    char input_buffer[CUSHION_INPUT_BUFFER_SIZE];
};

static inline void context_execution_error (struct context_t *instance,
                                            struct re2c_state_t *state,
                                            const char *format,
                                            ...)
{
    va_list variadic_arguments;
    va_start (variadic_arguments, format);

    if (state)
    {
        fprintf (stderr, "[%s:%u:%u] ", state->file_name, (unsigned int) state->cursor_line,
                 (unsigned int) state->cursor_column);
    }
    else
    {
        fprintf (stderr, "[<no-file>:0:0] ");
    }

    vfprintf (stderr, format, variadic_arguments);
    va_end (variadic_arguments);

    fprintf (stderr, "\n");
    instance->state_flags |= CONTEXT_STATE_FLAG_ERRED;
}

static enum result_t re2c_refill_buffer (struct context_t *instance, struct re2c_state_t *state)
{
    if (!state->input_file_optional)
    {
        // No file -> no refill, it is that simple.
        return RESULT_OK;
    }

    const char *preserve_from = state->token;
    if (state->saved && state->saved < preserve_from)
    {
        preserve_from = state->saved;
    }

#if defined(CUSHION_EXTENSIONS)
    if (state->guardrail && state->guardrail < preserve_from)
    {
        preserve_from = state->guardrail;
    }
#endif

    const size_t shift = preserve_from - state->input_buffer;
    const size_t used = state->limit - state->token;

    if (shift < 1u)
    {
#if defined(CUSHION_EXTENSIONS)
        context_execution_error (instance, state, "Encountered lexeme overflow, %s.",
                                 state->guardrail == preserve_from ? "guardrail is a culprit (can be an internal bug)" :
                                                                     "guardrail was not used");
#else
        context_execution_error (instance, state, "Encountered lexeme overflow.");
#endif
        return RESULT_FAILED;
    }

    // Shift buffer contents (discard everything up to the current token).
    memmove (state->input_buffer, state->token, used);
    state->limit -= shift;
    state->cursor -= shift;
    state->marker -= shift;
    state->token -= shift;

#if defined(CUSHION_EXTENSIONS)
    if (state->guardrail)
    {
        state->guardrail -= shift;
    }
#endif

#if !defined(_MSC_VER) || defined(__clang__)
#    pragma GCC diagnostic push
    // Looks like we have false-positive here on GCC.
#    pragma GCC diagnostic ignored "-Warray-bounds"
#endif

    const char **first_tag = (const char **) &state->tags;
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
    unsigned long read = fread (state->limit, 1u, CUSHION_INPUT_BUFFER_SIZE - used - 1u, state->input_file_optional);

    if (read == 0u)
    {
        // End of file, return non-zero code and re2c will process it properly.
        return RESULT_FAILED;
    }

    state->limit += read;
    *state->limit = '\0';
    return RESULT_OK;
}

static inline void re2c_yyskip (struct re2c_state_t *state)
{
    if (*state->cursor == '\n')
    {
        ++state->cursor_line;
        state->cursor_column = 0u;
    }

    ++state->cursor;
    ++state->cursor_column;
}

static inline void re2c_yybackup (struct re2c_state_t *state)
{
    state->marker = state->cursor;
    state->marker_line = state->cursor_line;
    state->marker_column = state->cursor_column;
}

static inline void re2c_yyrestore (struct re2c_state_t *state)
{
    state->cursor = state->marker;
    state->cursor_line = state->marker_line;
    state->cursor_column = state->marker_column;
}

static inline void re2c_save_cursor (struct re2c_state_t *state)
{
    state->saved = state->cursor;
    state->saved_line = state->cursor_line;
    state->saved_column = state->cursor_column;
}

static inline void re2c_clear_saved_cursor (struct re2c_state_t *state)
{
    state->saved = NULL;
    state->saved_line = 0u;
    state->saved_column = 0u;
}

static inline void re2c_restore_saved_cursor (struct re2c_state_t *state)
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
 re2c:define:YYFILL   = "re2c_refill_buffer (instance, state) == RESULT_OK";
 re2c:define:YYSTAGP = "@@{tag} = state->cursor;";
 re2c:define:YYSTAGN = "@@{tag} = NULL;";
 re2c:define:YYSHIFTSTAG  = "@@{tag} += @@{shift};";
 re2c:eof = 0;
 re2c:tags = 1;
 re2c:flags:utf-8 = 1;
 re2c:flags:case-ranges = 1;
 re2c:tags:expression = "state->tags.@@";
 */

// re2c tokenizer section.

/*!include:re2c "identifiers.re" */

enum token_type_t
{
    TOKEN_TYPE_PREPROCESSOR_IF = 0u,
    TOKEN_TYPE_PREPROCESSOR_IFDEF,
    TOKEN_TYPE_PREPROCESSOR_IFNDEF,
    TOKEN_TYPE_PREPROCESSOR_ELIF,
    TOKEN_TYPE_PREPROCESSOR_ELIFDEF,
    TOKEN_TYPE_PREPROCESSOR_ELIFNDEF,
    TOKEN_TYPE_PREPROCESSOR_ELSE,
    TOKEN_TYPE_PREPROCESSOR_ENDIF,
    TOKEN_TYPE_PREPROCESSOR_INCLUDE_SYSTEM,
    TOKEN_TYPE_PREPROCESSOR_INCLUDE_USER,
    TOKEN_TYPE_PREPROCESSOR_DEFINE_OBJECT,
    TOKEN_TYPE_PREPROCESSOR_DEFINE_FUNCTION,
    TOKEN_TYPE_PREPROCESSOR_UNDEF,
    TOKEN_TYPE_PREPROCESSOR_PRAGMA_ONCE,

    TOKEN_TYPE_IDENTIFIER,
    TOKEN_TYPE_PUNCTUATOR,

    // Only integer numbers can participate in preprocessor conditional expressions.
    // Therefore, we calculate integer values, but pass non-integer ones just as strings.

    TOKEN_TYPE_NUMBER_INTEGER,
    TOKEN_TYPE_NUMBER_FLOATING,

    TOKEN_TYPE_CHARACTER_LITERAL,
    TOKEN_TYPE_STRING_LITERAL,

    TOKEN_TYPE_NEW_LINE,

    /// \brief Special token for processing end of file that might break grammar in lexer.
    TOKEN_TYPE_END_OF_FILE,

    /// \brief Everything that is not one of the above, goes into here.
    /// \details As in standard draft, "each non-white-space character that cannot be one of the above" form tokens.
    TOKEN_TYPE_OTHER,
};

/// \brief Some preprocessing identifiers like __VA_ARGS__ or Cushion control identifiers need additional care.
/// \details When extensions are enabled, some keywords like return also need additional care.
enum identifier_kind_t
{
    IDENTIFIER_KIND_REGULAR = 0u,

    IDENTIFIER_KIND_VA_ARGS,
    IDENTIFIER_KIND_VA_OPT,

    IDENTIFIER_KIND_CUSHION_DISABLE,
    IDENTIFIER_KIND_CUSHION_PRESERVE,

    IDENTIFIER_KIND_CUSHION_DEFER,
    IDENTIFIER_KIND_CUSHION_WRAPPED,
    IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR,
    IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_PUSH,
    IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_REFERENCE,

    IDENTIFIER_KIND_TYPEDEF,
    IDENTIFIER_KIND_TYPEOF,
    IDENTIFIER_KIND_CONSTEXPR,
    IDENTIFIER_KIND_AUTO,
    IDENTIFIER_KIND_REGISTER,
    IDENTIFIER_KIND_STATIC,
    IDENTIFIER_KIND_EXTERN,
    IDENTIFIER_KIND_CONST,
    IDENTIFIER_KIND_INLINE,
    IDENTIFIER_KIND_STRUCT,
    IDENTIFIER_KIND_ENUM,

    IDENTIFIER_KIND_RETURN,
    IDENTIFIER_KIND_BREAK,
    IDENTIFIER_KIND_CONTINUE,
};

enum punctuator_kind_t
{
    PUNCTUATOR_KIND_LEFT_SQUARE_BRACKET = 0u, // [
    PUNCTUATOR_KIND_RIGHT_SQUARE_BRACKET,     // ]

    PUNCTUATOR_KIND_LEFT_PARENTHESIS,  // (
    PUNCTUATOR_KIND_RIGHT_PARENTHESIS, // )

    PUNCTUATOR_KIND_LEFT_CURLY_BRACE,  // {
    PUNCTUATOR_KIND_RIGHT_CURLY_BRACE, // }

    PUNCTUATOR_KIND_MEMBER_ACCESS,  // .
    PUNCTUATOR_KIND_POINTER_ACCESS, // ->

    PUNCTUATOR_KIND_INCREMENT, // ++
    PUNCTUATOR_KIND_DECREMENT, // --

    PUNCTUATOR_KIND_BITWISE_AND,     // &
    PUNCTUATOR_KIND_BITWISE_OR,      // |
    PUNCTUATOR_KIND_BITWISE_XOR,     // ^
    PUNCTUATOR_KIND_BITWISE_INVERSE, // ~

    PUNCTUATOR_KIND_PLUS,     // +
    PUNCTUATOR_KIND_MINUS,    // -
    PUNCTUATOR_KIND_MULTIPLY, // *
    PUNCTUATOR_KIND_DIVIDE,   // /
    PUNCTUATOR_KIND_MODULO,   // %

    PUNCTUATOR_KIND_LOGICAL_NOT,              // !
    PUNCTUATOR_KIND_LOGICAL_AND,              // &&
    PUNCTUATOR_KIND_LOGICAL_OR,               // ||
    PUNCTUATOR_KIND_LOGICAL_LESS,             // <
    PUNCTUATOR_KIND_LOGICAL_GREATER,          // >
    PUNCTUATOR_KIND_LOGICAL_LESS_OR_EQUAL,    // <=
    PUNCTUATOR_KIND_LOGICAL_GREATER_OR_EQUAL, // >=
    PUNCTUATOR_KIND_LOGICAL_EQUAL,            // ==

    PUNCTUATOR_KIND_LEFT_SHIFT,  // <<
    PUNCTUATOR_KIND_RIGHT_SHIFT, // >>

    PUNCTUATOR_KIND_QUESTION_MARK, // ?
    PUNCTUATOR_KIND_COLON,         // :
    PUNCTUATOR_KIND_DOUBLE_COLON,  // ::
    PUNCTUATOR_KIND_SEMICOLON,     // ;
    PUNCTUATOR_KIND_COMMA,         // ,
    PUNCTUATOR_KIND_TRIPLE_DOT,    // ...
    PUNCTUATOR_KIND_HASH,          // #
    PUNCTUATOR_KIND_DOUBLE_HASH,   // ##

    PUNCTUATOR_KIND_ASSIGN,             // =
    PUNCTUATOR_KIND_PLUS_ASSIGN,        // +=
    PUNCTUATOR_KIND_MINUS_ASSIGN,       // -=
    PUNCTUATOR_KIND_MULTIPLY_ASSIGN,    // *=
    PUNCTUATOR_KIND_DIVIDE_ASSIGN,      // /=
    PUNCTUATOR_KIND_LEFT_SHIFT_ASSIGN,  // <<=
    PUNCTUATOR_KIND_RIGHT_SHIFT_ASSIGN, // >>=
    PUNCTUATOR_KIND_BITWISE_AND_ASSIGN, // &=
    PUNCTUATOR_KIND_BITWISE_OR_ASSIGN,  // |=
    PUNCTUATOR_KIND_BITWISE_XOR_ASSIGN, // ^=
};

struct token_subsequence_t
{
    const char *begin;
    const char *end;
};

enum token_subsequence_encoding_t
{
    TOKEN_SUBSEQUENCE_ENCODING_ORDINARY = 0u,
    TOKEN_SUBSEQUENCE_ENCODING_UTF8,
    TOKEN_SUBSEQUENCE_ENCODING_UTF16,
    TOKEN_SUBSEQUENCE_ENCODING_UTF32,
    TOKEN_SUBSEQUENCE_ENCODING_WIDE,
};

struct encoded_token_subsequence_t
{
    enum token_subsequence_encoding_t encoding;
    const char *begin;
    const char *end;
};

struct token_t
{
    enum token_type_t type;
    const char *begin;
    const char *end;

    union
    {
        struct token_subsequence_t header_path;
        struct token_subsequence_t define_identifier;
        enum identifier_kind_t identifier_kind;
        enum punctuator_kind_t punctuator_kind;
        unsigned long long unsigned_number_value;
        struct encoded_token_subsequence_t character_literal;
        struct encoded_token_subsequence_t string_literal;
    };
};

static enum result_t tokenize_decimal_value (const char *begin, const char *end, unsigned long long *output)
{
    unsigned long long result = 0u;
    while (begin < end)
    {
        unsigned long long result_before = result;
        switch (*begin)
        {
        case '0' ... '9':
            result = result * 10u + (*begin - '0');
            break;

        default:
            break;
        }

        if (result < result_before)
        {
            // Overflow.
            return RESULT_FAILED;
        }

        ++begin;
    }

    *output = result;
    return RESULT_OK;
}

static enum result_t tokenize_octal_value (const char *begin, const char *end, unsigned long long *output)
{
    unsigned long long result = 0u;
    while (begin < end)
    {
        unsigned long long result_before = result;
        switch (*begin)
        {
        case '0' ... '7':
            result = result * 8u + (*begin - '0');
            break;

        default:
            break;
        }

        if (result < result_before)
        {
            // Overflow.
            return RESULT_FAILED;
        }

        ++begin;
    }

    *output = result;
    return RESULT_OK;
}

static enum result_t tokenize_hex_value (const char *begin, const char *end, unsigned long long *output)
{
    unsigned long long result = 0u;
    while (begin < end)
    {
        unsigned long long result_before = result;
        switch (*begin)
        {
        case '0' ... '9':
            result = result * 16u + (*begin - '0');
            break;

        case 'A' ... 'F':
            result = result * 16u + (*begin - 'A');
            break;

        case 'a' ... 'f':
            result = result * 16u + (*begin - 'a');
            break;

        default:
            break;
        }

        if (result < result_before)
        {
            // Overflow.
            return RESULT_FAILED;
        }

        ++begin;
    }

    *output = result;
    return RESULT_OK;
}

static enum result_t tokenize_binary_value (const char *begin, const char *end, unsigned long long *output)
{
    unsigned long long result = 0u;
    while (begin < end)
    {
        unsigned long long result_before = result;
        switch (*begin)
        {
        case '0' ... '1':
            result = result * 2u + (*begin - '0');
            break;

        default:
            break;
        }

        if (result < result_before)
        {
            // Overflow.
            return RESULT_FAILED;
        }

        ++begin;
    }

    *output = result;
    return RESULT_OK;
}

static enum result_t tokenization_next_token (struct context_t *instance,
                                              struct re2c_state_t *state,
                                              struct token_t *output)
{
    const char *marker_sub_begin;
    const char *marker_sub_end;

    /*!re2c
     new_line = [\x0d]? [\x0a];
     whitespace = [\x09\x0b\x0c\x0d\x20];
     backslash = [\x5c];
     identifier = id_start id_continue*;
     */

tokenization_start_next_token:
    state->token = state->cursor;
    output->begin = state->token;

    switch (state->state)
    {
    case RE2C_TOKENIZATION_STATE_NEW_LINE:
    {
        // Reset state to regular as next tokens would use regular anyway.
        state->state = RE2C_TOKENIZATION_STATE_REGULAR;
        goto tokenization_new_line_check_for_preprocessor_begin;

    tokenization_new_line_preprocessor_found:
        re2c_save_cursor (state);

#define PREPROCESSOR_EMIT_TOKEN(TOKEN)                                                                                 \
    re2c_clear_saved_cursor (state);                                                                                   \
    output->type = TOKEN;                                                                                              \
    output->end = state->cursor;                                                                                       \
    return RESULT_OK

    tokenization_new_line_preprocessor_determine_type:
        /*!re2c
         !use:check_unsupported_in_code;

         // Whitespaces here will be part of the token, therefore we do not treat them as glue.
         whitespace+ { goto tokenization_new_line_preprocessor_determine_type; }

         "if" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_IF); }
         "ifdef" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_IFDEF); }
         "ifndef" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_IFNDEF); }
         "elif" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_ELIF); }
         "elifdef" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_ELIFDEF); }
         "elifndef" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_ELIFNDEF); }
         "else" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_ELSE); }
         "endif" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_ENDIF); }

         "include" whitespace+ "<" @marker_sub_begin [^\n>]+ @marker_sub_end ">"
         {
             output->header_path.begin = marker_sub_begin;
             output->header_path.end = marker_sub_end;
             PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_INCLUDE_SYSTEM);
         }

         "include" whitespace+ "\"" @marker_sub_begin [^\n"]+ @marker_sub_end ">"
         {
             output->header_path.begin = marker_sub_begin;
             output->header_path.end = marker_sub_end;
             PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_INCLUDE_USER);
         }

         // Catch includes that we were not able to match properly.
         "include"
         {
             context_execution_error (instance, state, "Unable to properly parse include directive.");
             return RESULT_FAILED;
         }

         "define" whitespace+ @marker_sub_begin identifier @marker_sub_end "("
         {
             output->define_identifier.begin = marker_sub_begin;
             output->define_identifier.end = marker_sub_end;
             PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_DEFINE_FUNCTION);
         }

         "define" whitespace+ @marker_sub_begin identifier @marker_sub_end
         {
             output->define_identifier.begin = marker_sub_begin;
             output->define_identifier.end = marker_sub_end;
             PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_DEFINE_OBJECT);
         }

         "undef" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_UNDEF); }
         "pragma" whitespace+ "once" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_PRAGMA_ONCE); }

         // Fallthrough.
         identifier { }
         * { }
         $ { }
         */

        // This is a not preprocessor things that we care about. Just lex as hash and continue.
        if (state->flags & RE2C_TOKENIZATION_FLAGS_SKIP_REGULAR)
        {
            // If we're skipping regulars, no need to output punctuator.
            goto tokenization_skip_regular_routine;
        }

        re2c_restore_saved_cursor (state);
        output->type = TOKEN_TYPE_PUNCTUATOR;
        output->end = state->cursor;
        output->punctuator_kind = PUNCTUATOR_KIND_HASH;
        return RESULT_OK;

    tokenization_new_line_check_for_preprocessor_begin:
        re2c_save_cursor (state);

        /*!re2c
         "#" { goto tokenization_new_line_preprocessor_found; }
         * { }
         $ { }
         */

        // Intentional fallthrough. Nothing specific for new line found.
        re2c_restore_saved_cursor (state);
    }

    case RE2C_TOKENIZATION_STATE_REGULAR:
    {
        if (state->flags & RE2C_TOKENIZATION_FLAGS_SKIP_REGULAR)
        {
            // Separate routine for breezing through anything that is not a preprocessor directive.
        tokenization_skip_regular_routine:
            state->token = state->cursor;
            output->begin = state->token;

            /*!re2c
             new_line { state->state = RE2C_TOKENIZATION_STATE_NEW_LINE; goto tokenization_start_next_token; }
             * { goto tokenization_skip_regular_routine; }
             $ { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_END_OF_FILE); }
             */
        }

#define FOUND_GLUE                                                                                                     \
    if (state->flags & RE2C_TOKENIZATION_FLAGS_OUTPUT_GLUE)                                                            \
    {                                                                                                                  \
        context_output (instance, state->token, state->cursor);                                                        \
    }                                                                                                                  \
    goto tokenization_start_next_token

#define FOUND_COMMENT                                                                                                  \
    if ((state->flags & RE2C_TOKENIZATION_FLAGS_OUTPUT_GLUE) && (instance->options & CUSHION_OPTION_KEEP_COMMENTS))    \
    {                                                                                                                  \
        context_output (instance, state->token, state->cursor);                                                        \
    }                                                                                                                  \
    goto tokenization_start_next_token

#define PREPROCESSOR_EMIT_TOKEN_IDENTIFIER(KIND)                                                                       \
    output->identifier_kind = KIND;                                                                                    \
    PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_IDENTIFIER)

#define PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR(KIND)                                                                       \
    output->punctuator_kind = KIND;                                                                                    \
    PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PUNCTUATOR)

#define PREPROCESSOR_EMIT_TOKEN_CHARACTER_LITERAL(ENCODING)                                                            \
    output->character_literal.encoding = ENCODING;                                                                     \
    output->character_literal.begin = marker_sub_begin;                                                                \
    output->character_literal.end = marker_sub_end;                                                                    \
    PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_CHARACTER_LITERAL)

#define PREPROCESSOR_EMIT_TOKEN_STRING_LITERAL(ENCODING)                                                               \
    output->character_literal.encoding = ENCODING;                                                                     \
    output->character_literal.begin = marker_sub_begin;                                                                \
    output->character_literal.end = marker_sub_end;                                                                    \
    PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_STRING_LITERAL)

        /*!re2c
         whitespace+ { FOUND_GLUE; }
         // Escaped new line, just a glue, not a real token.
         "\\" new_line { FOUND_GLUE; }

         "//" [^\n]* { FOUND_COMMENT; }
         "/" "*" (. | new_line)* "*" "/" { FOUND_COMMENT; }

         "/" "*" (. | new_line)*
         {
             context_execution_error (instance, state, "Encountered multiline comment that was never closed.");
             return RESULT_FAILED;
         }

         new_line
         {
             state->state = RE2C_TOKENIZATION_STATE_NEW_LINE;
             PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_NEW_LINE);
         }

         "__VA_ARGS__" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_VA_ARGS); }
         "__VA_OPT__" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_VA_OPT); }

         "__CUSHION_DISABLE__" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_CUSHION_DISABLE); }
         "__CUSHION_PRESERVE__" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_CUSHION_PRESERVE); }

         "CUSHION_DEFER" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_CUSHION_DEFER); }
         "__CUSHION_WRAPPED__" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_CUSHION_WRAPPED); }
         "CUSHION_STATEMENT_ACCUMULATOR"
         { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR); }
         "CUSHION_STATEMENT_ACCUMULATOR_PUSH"
         { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_PUSH); }
         "CUSHION_STATEMENT_ACCUMULATOR_REFERENCE"
         { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_REFERENCE); }

         "typedef" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_TYPEDEF); }
         "typeof" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_TYPEOF); }
         "constexpr" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_CONSTEXPR); }
         "auto" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_AUTO); }
         "register" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_REGISTER); }
         "static" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_STATIC); }
         "extern" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_EXTERN); }
         "const" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_CONST); }
         "inline" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_INLINE); }
         "struct" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_STRUCT); }
         "enum" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_ENUM); }

         "return" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_RETURN); }
         "break" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_BREAK); }
         "continue" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_CONTINUE); }

         identifier { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_REGULAR); }

         "[" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_LEFT_SQUARE_BRACKET); }
         "]" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_RIGHT_SQUARE_BRACKET); }

         "(" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_LEFT_PARENTHESIS); }
         ")" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_RIGHT_PARENTHESIS); }

         "{" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_LEFT_CURLY_BRACE); }
         "}" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_RIGHT_CURLY_BRACE); }

         "." { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_MEMBER_ACCESS); }
         "->" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_POINTER_ACCESS); }

         "++" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_INCREMENT); }
         "--" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_DECREMENT); }

         "&" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_BITWISE_AND); }
         "|" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_BITWISE_OR); }
         "^" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_BITWISE_XOR); }
         "~" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_BITWISE_INVERSE); }

         "+" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_PLUS); }
         "-" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_MINUS); }
         "*" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_MULTIPLY); }
         "/" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_DIVIDE); }
         "%" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_MODULO); }

         "!" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_LOGICAL_NOT); }
         "&&" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_LOGICAL_AND); }
         "||" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_LOGICAL_OR); }
         "<" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_LOGICAL_LESS); }
         ">" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_LOGICAL_GREATER); }
         "<=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_LOGICAL_LESS_OR_EQUAL); }
         ">=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_LOGICAL_GREATER_OR_EQUAL); }
         "==" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_LOGICAL_EQUAL); }

         "<<" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_LEFT_SHIFT); }
         ">>" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_RIGHT_SHIFT); }

         "?" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_QUESTION_MARK); }
         ":" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_COLON); }
         "::" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_DOUBLE_COLON); }
         ";" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_SEMICOLON); }
         "," { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_COMMA); }
         "..." { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_TRIPLE_DOT); }
         "#" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_HASH); }
         "##" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_DOUBLE_HASH); }

         "=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_ASSIGN); }
         "+=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_PLUS_ASSIGN); }
         "-=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_MINUS_ASSIGN); }
         "*=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_MULTIPLY_ASSIGN); }
         "/=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_DIVIDE_ASSIGN); }
         "<<=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_LEFT_SHIFT_ASSIGN); }
         ">>=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_RIGHT_SHIFT_ASSIGN); }
         "&=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_BITWISE_AND_ASSIGN); }
         "|=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_BITWISE_OR_ASSIGN); }
         "^=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_BITWISE_XOR_ASSIGN); }

         unsigned_integer_suffix = [uU];
         long_integer_suffix = [lL];
         long_long_integer_suffix = "ll" | "LL";
         bit_precise_integer_suffix = "wb" | "WB";
         integer_suffix =
             (unsigned_integer_suffix (long_integer_suffix | long_long_integer_suffix | bit_precise_integer_suffix)?) |
             (long_integer_suffix? unsigned_integer_suffix) |
             (long_long_integer_suffix? unsigned_integer_suffix) |
             (bit_precise_integer_suffix? unsigned_integer_suffix);

         // Decimal integer number. Minus in front is not a part of literal per specification.
         @marker_sub_begin [1-9] [0-9']* @marker_sub_end integer_suffix?
         {
             if (tokenize_decimal_value (marker_sub_begin, marker_sub_end, &output->unsigned_number_value) != RESULT_OK)
             {
                 context_execution_error (instance, state, "Failed to parse number due to overflow.");
                 return RESULT_FAILED;
             }

             PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_NUMBER_INTEGER);
         }

         // Octal integer number. Minus in front is not a part of literal per specification.
         // And yes, historically just prepending zero is enough to make literal octal.
         "0" [oO]? @marker_sub_begin [0-7']+ @marker_sub_end integer_suffix?
         {
             if (tokenize_octal_value (marker_sub_begin, marker_sub_end, &output->unsigned_number_value) != RESULT_OK)
             {
                 context_execution_error (instance, state, "Failed to parse number due to overflow.");
                 return RESULT_FAILED;
             }

             PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_NUMBER_INTEGER);
         }

         // Catch user errors when zero prefix made literal octal, but it is not actually octal.
         "0" [0-9']+
         {
             context_execution_error (
                 instance, state,
                 "Caught octal (zero prefixed) integer literal that is not actually octal. "
                 "Yes, by specification zero prefix is enough to make literal octal.");
             return RESULT_FAILED;
         }

         // Hexadecimal integer number. Minus in front is not a part of literal per specification.
         "0" [xX] @marker_sub_begin [0-9a-fA-F']+ @marker_sub_end integer_suffix?
         {
             if (tokenize_hex_value (marker_sub_begin, marker_sub_end, &output->unsigned_number_value) != RESULT_OK)
             {
                 context_execution_error (instance, state, "Failed to parse number due to overflow.");
                 return RESULT_FAILED;
             }

             PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_NUMBER_INTEGER);
         }

         // Binary integer number. Minus in front is not a part of literal per specification.
         "0" [bB] @marker_sub_begin [01']+ @marker_sub_end integer_suffix?
         {
             if (tokenize_binary_value (marker_sub_begin, marker_sub_end, &output->unsigned_number_value) != RESULT_OK)
             {
                 context_execution_error (instance, state, "Failed to parse number due to overflow.");
                 return RESULT_FAILED;
             }

             PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_NUMBER_INTEGER);
         }

         digit_sequence = [0-9] [0-9']*;
         hex_digit_sequence = [0-9a-fA-F] [0-9a-fA-F']*;
         real_floating_suffix = [fF] | [lL] | "df" | "dd" | "dl" | "DF" | "DD" | "DL";
         complex_floating_suffix = [iIjJ];
         floating_suffix =
             (real_floating_suffix complex_floating_suffix?) |
             (complex_floating_suffix real_floating_suffix?);

         // Decimal floating literal.
         (digit_sequence? "." digit_sequence) | (digit_sequence ".") ([eE]? "-"? digit_sequence)? floating_suffix?
         {
             PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_NUMBER_FLOATING);
         }

         // Hexadecimal floating literal.
         "0" [xX] ((hex_digit_sequence? "." hex_digit_sequence) | (hex_digit_sequence "."?))
         [pP] "-"? digit_sequence floating_suffix?
         {
             PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_NUMBER_FLOATING);
         }

         simple_escape_sequence = "\\" ['"?\\abfnrtv];
         // For now, we only support simple escape sequences, but that might be changed in the future.
         escape_sequence = simple_escape_sequence;
         character_literal_sequence = (escape_sequence | [^'\\\n])*;
         string_literal_sequence = (escape_sequence | [^"\\\n])*;

         "'" @marker_sub_begin character_literal_sequence @marker_sub_end "'"
         {
             PREPROCESSOR_EMIT_TOKEN_CHARACTER_LITERAL (TOKEN_SUBSEQUENCE_ENCODING_ORDINARY);
         }

         "u8'" @marker_sub_begin character_literal_sequence @marker_sub_end "'"
         {
             PREPROCESSOR_EMIT_TOKEN_CHARACTER_LITERAL (TOKEN_SUBSEQUENCE_ENCODING_UTF8);
         }

         "u'" @marker_sub_begin character_literal_sequence @marker_sub_end "'"
         {
             PREPROCESSOR_EMIT_TOKEN_CHARACTER_LITERAL (TOKEN_SUBSEQUENCE_ENCODING_UTF16);
         }

         "U'" @marker_sub_begin character_literal_sequence @marker_sub_end "'"
         {
             PREPROCESSOR_EMIT_TOKEN_CHARACTER_LITERAL (TOKEN_SUBSEQUENCE_ENCODING_UTF32);
         }

         "L'" @marker_sub_begin character_literal_sequence @marker_sub_end "'"
         {
             PREPROCESSOR_EMIT_TOKEN_CHARACTER_LITERAL (TOKEN_SUBSEQUENCE_ENCODING_WIDE);
         }

         "\"" @marker_sub_begin string_literal_sequence @marker_sub_end "\""
         {
             PREPROCESSOR_EMIT_TOKEN_STRING_LITERAL (TOKEN_SUBSEQUENCE_ENCODING_ORDINARY);
         }

         "u8\"" @marker_sub_begin string_literal_sequence @marker_sub_end "\""
         {
             PREPROCESSOR_EMIT_TOKEN_STRING_LITERAL (TOKEN_SUBSEQUENCE_ENCODING_UTF8);
         }

         "u\"" @marker_sub_begin string_literal_sequence @marker_sub_end "\""
         {
             PREPROCESSOR_EMIT_TOKEN_STRING_LITERAL (TOKEN_SUBSEQUENCE_ENCODING_UTF16);
         }

         "U\"" @marker_sub_begin string_literal_sequence @marker_sub_end "\""
         {
             PREPROCESSOR_EMIT_TOKEN_STRING_LITERAL (TOKEN_SUBSEQUENCE_ENCODING_UTF32);
         }

         "L\"" @marker_sub_begin string_literal_sequence @marker_sub_end "\""
         {
             PREPROCESSOR_EMIT_TOKEN_STRING_LITERAL (TOKEN_SUBSEQUENCE_ENCODING_WIDE);
         }

         !use:check_unsupported_in_code;
         * { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_OTHER); }
         $ { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_END_OF_FILE); }
         */

#undef PREPROCESSOR_EMIT_TOKEN_CHARACTER_LITERAL
#undef PREPROCESSOR_EMIT_TOKEN_STRING_LITERAL
#undef PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR
#undef PREPROCESSOR_EMIT_TOKEN_IDENTIFIER
#undef FOUND_GLUE
#undef PREPROCESSOR_EMIT_TOKEN
        break;
    }
    }

    context_execution_error (instance, state, "Unexpected way to exit tokenizer, internal error.");
    return RESULT_FAILED;
}

// Implementation section: implementing the actual interface.

cushion_context_t cushion_context_create (void)
{
    struct context_t *instance = malloc (sizeof (struct context_t));
    stack_group_allocator_init (&instance->allocator);
    context_clean_configuration (instance);

    cushion_context_t result = {.value = instance};
    return result;
}

void cushion_context_configure_feature (cushion_context_t context, enum cushion_feature_t feature, unsigned int enabled)
{
    struct context_t *instance = context.value;
    assert (1u << feature);

    if (enabled)
    {
        instance->features |= (1u << feature);
    }
    else
    {
        instance->features &= ~(1u << feature);
    }
}

void cushion_context_configure_option (cushion_context_t context, enum cushion_option_t option, unsigned int enabled)
{
    struct context_t *instance = context.value;
    assert (1u << option);

    if (enabled)
    {
        instance->options |= (1u << option);
    }
    else
    {
        instance->options &= ~(1u << option);
    }
}

void cushion_context_configure_input (cushion_context_t context, const char *path)
{
    struct context_t *instance = context.value;
    struct input_node_t *node = stack_group_allocator_allocate (&instance->allocator, sizeof (struct input_node_t),
                                                                _Alignof (struct input_node_t));

    node->path = context_copy_string_inside (instance, path);
    node->next = NULL;

    if (instance->inputs_last)
    {
        instance->inputs_last->next = node;
        instance->inputs_last = node;
    }
    else
    {
        instance->inputs_first = node;
        instance->inputs_last = node;
    }
}

void cushion_context_configure_output (cushion_context_t context, const char *path)
{
    struct context_t *instance = context.value;
    instance->output_path = context_copy_string_inside (instance, path);
}

void cushion_context_configure_cmake_depfile (cushion_context_t context, const char *path)
{
    struct context_t *instance = context.value;
    instance->cmake_depfile_path = context_copy_string_inside (instance, path);
}

void cushion_context_configure_define (cushion_context_t context, const char *name, const char *value)
{
    struct context_t *instance = context.value;
    struct macro_node_t *new_node = stack_group_allocator_allocate (&instance->allocator, sizeof (struct macro_node_t),
                                                                    _Alignof (struct macro_node_t));

    new_node->name = name;
    new_node->value = value;
    new_node->parameters_first = NULL;
    context_macro_add (instance, new_node);
}

void cushion_context_configure_include_full (cushion_context_t context, const char *path)
{
    struct context_t *instance = context.value;
    struct include_node_t *node = stack_group_allocator_allocate (&instance->allocator, sizeof (struct include_node_t),
                                                                  _Alignof (struct include_node_t));

    node->type = INCLUDE_TYPE_FULL;
    node->path = context_copy_string_inside (instance, path);
    context_includes_add (instance, node);
}

void cushion_context_configure_include_scan_only (cushion_context_t context, const char *path)
{
    struct context_t *instance = context.value;
    struct include_node_t *node = stack_group_allocator_allocate (&instance->allocator, sizeof (struct include_node_t),
                                                                  _Alignof (struct include_node_t));

    node->type = INCLUDE_TYPE_SCAN;
    node->path = context_copy_string_inside (instance, path);
    context_includes_add (instance, node);
}

enum cushion_result_t cushion_context_execute (cushion_context_t context)
{
    struct context_t *instance = context.value;
    enum cushion_result_t result = CUSHION_RESULT_OK;
    instance->state_flags = CONTEXT_STATE_FLAG_EXECUTION;

    if (!instance->inputs_first)
    {
        fprintf (stderr, "Missing inputs in configuration.\n");
        result = CUSHION_RESULT_PARTIAL_CONFIGURATION;
    }

    if (!instance->output_path)
    {
        fprintf (stderr, "Missing output path in configuration.\n");
        result = CUSHION_RESULT_PARTIAL_CONFIGURATION;
    }

#if !defined(CUSHION_EXTENSIONS)
    if (instance->features)
    {
        fprintf (stderr, "Received features selection in clean build (Cushion was built without extensions).\n");
        result = CUSHION_RESULT_UNSUPPORTED_FEATURES;
    }
#endif

    if (result == CUSHION_RESULT_OK)
    {
        // TODO: Implement. Do things. This call is to silence unused function warnings.
        struct re2c_state_t state;
        struct token_t token;
        tokenization_next_token (instance, &state, &token);
    }

    // Reset all the configuration.
    context_clean_configuration (instance);

    // Shrink and reset memory usage.
    stack_group_allocator_shrink (&instance->allocator);
    stack_group_allocator_reset (&instance->allocator);

    return result;
}

void cushion_context_destroy (cushion_context_t context)
{
    struct context_t *instance = context.value;
    stack_group_allocator_shutdown (&instance->allocator);
    free (instance);
}
