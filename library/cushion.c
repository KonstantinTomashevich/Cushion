#include <assert.h>
#include <limits.h>
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

static inline uintptr_t apply_alignment_reversed (uintptr_t address_or_size, uintptr_t alignment)
{
    return address_or_size - address_or_size % alignment;
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
    void *top_transient;
    void *bottom_persistent;
    uint8_t data[CUSHION_ALLOCATOR_PAGE_SIZE];
};

enum allocation_class_t
{
    /// \brief Allocated in some scope and will be deallocated when exiting the scope.
    ALLOCATION_CLASS_TRANSIENT = 0u,

    /// \brief Persistent allocator for the whole execution.
    ALLOCATION_CLASS_PERSISTENT,
};

struct stack_group_allocator_transient_marker_t
{
    struct stack_group_allocator_page_t *page;
    void *top_transient;
};

static void stack_group_allocator_init (struct stack_group_allocator_t *instance)
{
    instance->first_page = malloc (sizeof (struct stack_group_allocator_page_t));
    instance->first_page->next = NULL;
    instance->first_page->top_transient = instance->first_page->data;
    instance->first_page->bottom_persistent = instance->first_page->data + CUSHION_ALLOCATOR_PAGE_SIZE;
    instance->current_page = instance->first_page;
}

static void *stack_group_allocator_page_allocate (struct stack_group_allocator_page_t *page,
                                                  uintptr_t size,
                                                  uintptr_t alignment,
                                                  enum allocation_class_t class)
{
    switch (class)
    {
    case ALLOCATION_CLASS_TRANSIENT:
    {
        uint8_t *address = (uint8_t *) apply_alignment ((uintptr_t) page->top_transient, alignment);
        uint8_t *new_top = address + size;

        if (new_top <= (uint8_t *) page->bottom_persistent)
        {
            page->top_transient = new_top;
            return address;
        }

        break;
    }

    case ALLOCATION_CLASS_PERSISTENT:
    {
        uint8_t *address =
            (uint8_t *) apply_alignment_reversed (((uintptr_t) page->bottom_persistent) - size, alignment);

        if (address >= (uint8_t *) page->top_transient)
        {
            page->bottom_persistent = address;
            return address;
        }

        break;
    }
    }

    return NULL;
}

static void *stack_group_allocator_allocate (struct stack_group_allocator_t *allocator,
                                             uintptr_t size,
                                             uintptr_t alignment,
                                             enum allocation_class_t class)
{
    void *result = stack_group_allocator_page_allocate (allocator->current_page, size, alignment, class);
    if (!result)
    {
        if (!allocator->current_page->next)
        {
            struct stack_group_allocator_page_t *new_page = malloc (sizeof (struct stack_group_allocator_page_t));
            new_page->next = NULL;
            new_page->top_transient = new_page->data;
            new_page->bottom_persistent = new_page->data + CUSHION_ALLOCATOR_PAGE_SIZE;
            allocator->current_page->next = new_page;
        }
        else
        {
            allocator->current_page = allocator->current_page->next;
        }

        result = stack_group_allocator_page_allocate (allocator->current_page, size, alignment, class);
    }

    if (!result)
    {
        fprintf (stderr, "Internal error: failed to allocate %lu bytes with %lu alignment.", (unsigned long) size,
                 (unsigned long) alignment);
        abort ();
    }

    return result;
}

static struct stack_group_allocator_transient_marker_t stack_group_allocator_get_transient_marker (
    struct stack_group_allocator_t *allocator)
{
    return (struct stack_group_allocator_transient_marker_t) {
        .page = allocator->current_page,
        .top_transient = allocator->current_page->top_transient,
    };
}

static void stack_group_allocator_reset_transient (struct stack_group_allocator_t *allocator,
                                                   struct stack_group_allocator_transient_marker_t transient_marker)
{
    allocator->current_page = transient_marker.page;
    allocator->current_page->top_transient = transient_marker.top_transient;
    struct stack_group_allocator_page_t *page = allocator->current_page->next;

    while (page)
    {
        page->top_transient = page->data;
        page = page->next;
    }
}

static void stack_group_allocator_reset_all (struct stack_group_allocator_t *allocator)
{
    struct stack_group_allocator_page_t *page = allocator->first_page;
    while (page)
    {
        page->top_transient = page->data;
        page->bottom_persistent = page->data + CUSHION_ALLOCATOR_PAGE_SIZE;
        page = page->next;
    }

    allocator->current_page = allocator->first_page;
}

static void stack_group_allocator_shrink (struct stack_group_allocator_t *allocator)
{
    struct stack_group_allocator_page_t *page = allocator->current_page->next;
    struct stack_group_allocator_page_t *previous = allocator->current_page;

    while (page)
    {
        struct stack_group_allocator_page_t *next = page->next;
        if (page->top_transient == page->data && page->bottom_persistent == page->data + CUSHION_ALLOCATOR_PAGE_SIZE)
        {
            previous->next = next;
            free (page);
        }
        else
        {
            previous = page;
        }

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

    struct macro_node_t *unresolved_macros_first;
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

enum macro_flags_t
{
    MACRO_FLAG_NONE = 0u,

    /// \brief It is a function-like macro. Even macro without arguments can be function-like.
    MACRO_FLAG_FUNCTION = 1u << 0u,

    MACRO_FLAG_VARIADIC_PARAMETERS = 1u << 1u,
    MACRO_FLAG_PRESERVED = 1u << 2u,
};

struct macro_parameter_node_t
{
    struct macro_parameter_node_t *next;
    unsigned int name_hash;
    const char *name;
};

struct macro_node_t
{
    struct macro_node_t *next;
    unsigned int name_hash;
    const char *name;
    enum macro_flags_t flags;

    union
    {
        /// \brief String value when we're gathering macro values from configuration.
        const char *value;

        /// \brief Actual replacement list tokens. Produced when execution has started.
        struct token_list_item_t *replacement_list_first;
    };

    struct macro_parameter_node_t *parameters_first;
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

    instance->unresolved_macros_first = NULL;
}

static inline char *context_copy_string_inside (struct context_t *instance,
                                                const char *string,
                                                enum allocation_class_t allocation_class)
{
    size_t length = strlen (string);
    char *copied =
        stack_group_allocator_allocate (&instance->allocator, length + 1u, _Alignof (char), allocation_class);
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
                                                         const char *name_begin,
                                                         const char *name_end)
{
    while (list)
    {
        if (list->name_hash == name_hash && strlen (list->name) == (size_t) (name_end - name_begin) &&
            strncmp (list->name, name_begin, name_end - name_begin) == 0)
        {
            return list;
        }

        list = list->next;
    }

    return NULL;
}

static struct macro_node_t *context_macro_search (struct context_t *instance,
                                                  const char *name_begin,
                                                  const char *name_end)
{
    unsigned int name_hash = hash_djb2_char_sequence (name_begin, name_end);
    struct macro_node_t *list = instance->macro_buckets[name_hash % CUSHION_MACRO_BUCKETS];
    return macro_search_in_list (list, name_hash, name_begin, name_end);
}

struct re2c_state_t;

static inline void context_execution_error (struct context_t *instance,
                                            struct re2c_state_t *state,
                                            const char *format,
                                            ...);

static inline void context_signal_error (struct context_t *instance)
{
    instance->state_flags |= CONTEXT_STATE_FLAG_ERRED;
}

static inline unsigned int context_is_error_signaled (struct context_t *instance)
{
    return instance->state_flags & CONTEXT_STATE_FLAG_ERRED;
}

static inline unsigned int context_has_option (struct context_t *instance, enum cushion_option_t option)
{
    return instance->options & (1u << option);
}

static void context_macro_add (struct context_t *instance, struct macro_node_t *node)
{
    node->name_hash = hash_djb2_null_terminated (node->name);
    // To have consistent behavior, calculate parameter hashes here too.
    struct macro_parameter_node_t *parameter = node->parameters_first;

    while (parameter)
    {
        parameter->name_hash = hash_djb2_null_terminated (parameter->name);
        parameter = parameter->next;
    }

    struct macro_node_t *bucket_list = instance->macro_buckets[node->name_hash % CUSHION_MACRO_BUCKETS];
    struct macro_node_t *already_here =
        macro_search_in_list (bucket_list, node->name_hash, node->name, node->name + strlen (node->name));

    if (already_here)
    {
        if (context_has_option (instance, CUSHION_OPTION_FORBID_MACRO_REDEFINITION) &&
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

static void context_macro_remove (struct context_t *instance, const char *name_begin, const char *name_end)
{
    unsigned int name_hash = hash_djb2_char_sequence (name_begin, name_end);
    // Logic is almost the same as for search, but we need to keep previous pointer here for proper removal.
    struct macro_node_t *list = instance->macro_buckets[name_hash % CUSHION_MACRO_BUCKETS];
    struct macro_node_t *previous = NULL;

    while (list)
    {
        if (list->name_hash == name_hash && strlen (list->name) == (size_t) (name_end - name_begin) &&
            strncmp (list->name, name_begin, name_end - name_begin) == 0)
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

static void context_output_sequence (struct context_t *instance, const char *begin, const char *end)
{
    if (instance->output)
    {
        // TODO: Proper deferred output needed for the future when statement accumulators are added.

        size_t length = end - begin;
        if (fwrite (begin, 1u, length, instance->output) != length)
        {
            fprintf (stderr, "Failed to output preprocessed code.\n");
            context_signal_error (instance);
        }
    }
}

static inline void context_output_null_terminated (struct context_t *instance, const char *string)
{
    return context_output_sequence (instance, string, string + strlen (string));
}

static inline void context_output_line_marker (struct context_t *instance, unsigned int line, const char *path)
{
    if (instance->output)
    {
        // TODO: Proper deferred output needed for the future when statement accumulators are added.

        if (fprintf (instance->output, "#line %u \"%s\"\n", line, path) != 0)
        {
            fprintf (stderr, "Failed to output preprocessed code.\n");
            context_signal_error (instance);
        }
    }
}

// re2c support section: structs and functions to properly setup re2c for tokenization.

struct re2c_tags_t
{
    /*!stags:re2c format = 'const char *@@;';*/
};

enum re2c_tokenization_state_t
{
    RE2C_TOKENIZATION_STATE_REGULAR = 0u,
    RE2C_TOKENIZATION_STATE_NEW_LINE,
    RE2C_TOKENIZATION_STATE_INCLUDE,
};

enum re2c_tokenization_flags_t
{
    RE2C_TOKENIZATION_FLAGS_NONE = 0u,

    /// \brief Simplified mode that skips any tokens that are not preprocessor directives.
    /// \details Useful for blazing through scan only headers and conditionally excluded source parts.
    ///          Should be disabled when tokenizing conditional expressions as their expressions are regular tokens.
    RE2C_TOKENIZATION_FLAGS_SKIP_REGULAR = 1u << 0u,
};

struct re2c_state_t
{
    /// \brief Tokenization file name that can be changed by line directives.
    const char *file_name;

    enum re2c_tokenization_state_t state;
    enum re2c_tokenization_flags_t flags;

    char *limit;
    const char *cursor;
    const char *marker;
    const char *token;

    /// \brief If not NULL, prevents code after it from being lost during refill, the same way as token does.
    const char *guardrail;

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

static void re2c_state_init_for_argument_string (struct re2c_state_t *state, const char *string)
{
    state->file_name = "<argument-string>";
    state->state = RE2C_TOKENIZATION_STATE_REGULAR; // Already a part of define, not a new line, actually.
    state->flags = 0u;

    const size_t length = strlen (string);
    // We don't actually change limit if there is no refill, so it is okay to cast.
    state->limit = (char *) string + length;
    state->cursor = string;
    state->marker = string;
    state->token = string;
    state->guardrail = NULL;

    state->cursor_line = 1u;
    state->cursor_column = 1u;
    state->marker_line = 1u;
    state->marker_column = 1u;

    state->saved = NULL;
    state->saved_line = 1u;
    state->saved_column = 1u;
    state->input_file_optional = NULL;
}

static void re2c_state_init_for_file (struct re2c_state_t *state, const char *path, FILE *file)
{
    state->file_name = path;
    state->state = RE2C_TOKENIZATION_STATE_NEW_LINE;
    state->flags = 0u;

    state->limit = state->input_buffer + CUSHION_INPUT_BUFFER_SIZE - 1u;
    state->cursor = state->input_buffer + CUSHION_INPUT_BUFFER_SIZE - 1u;
    state->marker = state->input_buffer + CUSHION_INPUT_BUFFER_SIZE - 1u;
    state->token = state->input_buffer + CUSHION_INPUT_BUFFER_SIZE - 1u;
    *state->limit = '\0';
    state->guardrail = NULL;

    state->cursor_line = 1u;
    state->cursor_column = 1u;
    state->marker_line = 1u;
    state->marker_column = 1u;

    state->saved = NULL;
    state->saved_line = 1u;
    state->saved_column = 1u;
    state->input_file_optional = file;
}

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
    context_signal_error (instance);
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

    if (state->guardrail && state->guardrail < preserve_from)
    {
        preserve_from = state->guardrail;
    }

    const size_t shift = preserve_from - state->input_buffer;
    const size_t used = state->limit - state->token;

    if (shift < 1u)
    {
        context_execution_error (
            instance, state, "Encountered lexeme overflow, %s.",
            state->guardrail == preserve_from ? "guardrail is a culprit" : "guardrail was not used");
        return RESULT_FAILED;
    }

    // Shift buffer contents (discard everything up to the current token).
    memmove (state->input_buffer, state->token, used);
    state->limit -= shift;
    state->cursor -= shift;
    state->marker -= shift;
    state->token -= shift;

    if (state->guardrail)
    {
        state->guardrail -= shift;
    }

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
    TOKEN_TYPE_PREPROCESSOR_INCLUDE,
    TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM,
    TOKEN_TYPE_PREPROCESSOR_HEADER_USER,
    TOKEN_TYPE_PREPROCESSOR_DEFINE,
    TOKEN_TYPE_PREPROCESSOR_UNDEF,
    TOKEN_TYPE_PREPROCESSOR_LINE,
    TOKEN_TYPE_PREPROCESSOR_PRAGMA,

    TOKEN_TYPE_IDENTIFIER,
    TOKEN_TYPE_PUNCTUATOR,

    // Only integer numbers can participate in preprocessor conditional expressions.
    // Therefore, we calculate integer values, but pass non-integer ones just as strings.

    TOKEN_TYPE_NUMBER_INTEGER,
    TOKEN_TYPE_NUMBER_FLOATING,

    TOKEN_TYPE_CHARACTER_LITERAL,
    TOKEN_TYPE_STRING_LITERAL,

    TOKEN_TYPE_NEW_LINE,

    /// \brief Whitespaces as a glue.
    /// \brief Glue is saved as token so lexer can handle it in output generation logic without relying on tokenizer
    ///        to handle its output.
    TOKEN_TYPE_GLUE,

    /// \brief Actual comments.
    /// \details We express comments as tokens because it makes it possible to handle them on lexer level along with
    ///          all other output generation. Also, it makes it possible to keep them inside macros if requested.
    TOKEN_TYPE_COMMENT,

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

    IDENTIFIER_KIND_CUSHION_PRESERVE,

    IDENTIFIER_KIND_CUSHION_DEFER,
    IDENTIFIER_KIND_CUSHION_WRAPPED,
    IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR,
    IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_PUSH,
    IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_REFERENCE,

    IDENTIFIER_KIND_DEFINED,
    IDENTIFIER_KIND_HAS_INCLUDE,
    IDENTIFIER_KIND_HAS_EMBED,
    IDENTIFIER_KIND_HAS_C_ATTRIBUTE,
    IDENTIFIER_KIND_MACRO_PRAGMA,

    IDENTIFIER_KIND_IF,
    IDENTIFIER_KIND_FOR,
    IDENTIFIER_KIND_WHILE,
    IDENTIFIER_KIND_DO,

    IDENTIFIER_KIND_RETURN,
    IDENTIFIER_KIND_BREAK,
    IDENTIFIER_KIND_CONTINUE,

    // TODO: Not sure what to do with goto for defer feature.
    //       It is rather difficult to properly generate defer when goto is used, especially when label is not yet
    //       declared. But it might be an interesting though experiment: thinking how it could be implemented.
    IDENTIFIER_KIND_GOTO,
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
    PUNCTUATOR_KIND_LOGICAL_NOT_EQUAL,        // !=

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
        enum identifier_kind_t identifier_kind;
        enum punctuator_kind_t punctuator_kind;
        unsigned long long unsigned_number_value;
        struct encoded_token_subsequence_t symbolic_literal;
    };
};

struct token_list_item_t
{
    struct token_list_item_t *next;
    struct token_t token;
};

/// \brief Copies all token contents into separate allocation so it is not dependant on input buffer and can be used
///        anywhere, for example inside macro replacement list.
static inline struct token_list_item_t *save_token_to_memory (struct context_t *instance,
                                                              const struct token_t *token,
                                                              enum allocation_class_t allocation_class)
{
    struct token_list_item_t *target = stack_group_allocator_allocate (
        &instance->allocator, sizeof (struct token_list_item_t), _Alignof (struct token_list_item_t), allocation_class);

    target->next = NULL;
    target->token.type = token->type;
    target->token.begin = stack_group_allocator_allocate (&instance->allocator, token->end - token->begin + 1u,
                                                          _Alignof (char), allocation_class);
    target->token.end = target->token.begin + (token->end - token->begin);
    memcpy ((char *) target->token.begin, token->begin, token->end - token->begin);
    *((char *) target->token.end) = '\0';

    // Now properly recalculate subsequences to make sure that they point to copied out text.
    switch (token->type)
    {
    case TOKEN_TYPE_PREPROCESSOR_IF:
    case TOKEN_TYPE_PREPROCESSOR_IFDEF:
    case TOKEN_TYPE_PREPROCESSOR_IFNDEF:
    case TOKEN_TYPE_PREPROCESSOR_ELIF:
    case TOKEN_TYPE_PREPROCESSOR_ELIFDEF:
    case TOKEN_TYPE_PREPROCESSOR_ELIFNDEF:
    case TOKEN_TYPE_PREPROCESSOR_ELSE:
    case TOKEN_TYPE_PREPROCESSOR_ENDIF:
    case TOKEN_TYPE_PREPROCESSOR_INCLUDE:
    case TOKEN_TYPE_PREPROCESSOR_DEFINE:
    case TOKEN_TYPE_PREPROCESSOR_UNDEF:
    case TOKEN_TYPE_PREPROCESSOR_LINE:
    case TOKEN_TYPE_PREPROCESSOR_PRAGMA:
    case TOKEN_TYPE_NUMBER_FLOATING:
    case TOKEN_TYPE_NEW_LINE:
    case TOKEN_TYPE_GLUE:
    case TOKEN_TYPE_COMMENT:
    case TOKEN_TYPE_END_OF_FILE:
    case TOKEN_TYPE_OTHER:
        break;

    case TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
    case TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
        target->token.header_path.begin = target->token.header_path.begin + (token->header_path.begin - token->begin);
        target->token.header_path.end = target->token.header_path.begin + (token->header_path.end - token->end);
        break;

    case TOKEN_TYPE_IDENTIFIER:
        target->token.identifier_kind = token->identifier_kind;
        break;

    case TOKEN_TYPE_PUNCTUATOR:
        target->token.punctuator_kind = token->punctuator_kind;
        break;

    case TOKEN_TYPE_NUMBER_INTEGER:
        target->token.unsigned_number_value = token->unsigned_number_value;
        break;

    case TOKEN_TYPE_CHARACTER_LITERAL:
    case TOKEN_TYPE_STRING_LITERAL:
        target->token.symbolic_literal.encoding = token->symbolic_literal.encoding;
        target->token.symbolic_literal.begin =
            target->token.symbolic_literal.begin + (token->symbolic_literal.begin - token->begin);
        target->token.symbolic_literal.end =
            target->token.symbolic_literal.begin + (token->symbolic_literal.end - token->end);
        break;
    }

    return target;
}

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

static void tokenization_next_token (struct context_t *instance, struct re2c_state_t *state, struct token_t *output)
{
    const char *marker_sub_begin;
    const char *marker_sub_end;

    /*!re2c
     new_line = [\x0d]? [\x0a];
     whitespace = [\x09\x0b\x0c\x0d\x20];
     backslash = [\x5c];
     identifier = id_start id_continue*;
     multi_line_comment = "/" "*" (. | new_line)* "*" "/";
     */

start_next_token:
    state->token = state->cursor;
    output->begin = state->token;

#define PREPROCESSOR_EMIT_TOKEN(TOKEN)                                                                                 \
    re2c_clear_saved_cursor (state);                                                                                   \
    output->type = TOKEN;                                                                                              \
    output->end = state->cursor;                                                                                       \
    return

#define PREPROCESSOR_EMIT_TOKEN_IDENTIFIER(KIND)                                                                       \
    output->identifier_kind = KIND;                                                                                    \
    PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_IDENTIFIER)

#define PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR(KIND)                                                                       \
    output->punctuator_kind = KIND;                                                                                    \
    PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PUNCTUATOR)

#define PREPROCESSOR_EMIT_TOKEN_CHARACTER_LITERAL(ENCODING)                                                            \
    output->symbolic_literal.encoding = ENCODING;                                                                      \
    output->symbolic_literal.begin = marker_sub_begin;                                                                 \
    output->symbolic_literal.end = marker_sub_end;                                                                     \
    PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_CHARACTER_LITERAL)

#define PREPROCESSOR_EMIT_TOKEN_STRING_LITERAL(ENCODING)                                                               \
    output->symbolic_literal.encoding = ENCODING;                                                                      \
    output->symbolic_literal.begin = marker_sub_begin;                                                                 \
    output->symbolic_literal.end = marker_sub_end;                                                                     \
    PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_STRING_LITERAL)

    switch (state->state)
    {
    case RE2C_TOKENIZATION_STATE_REGULAR:
    {
    regular_routine:
        if (state->flags & RE2C_TOKENIZATION_FLAGS_SKIP_REGULAR)
        {
            // Separate routine for breezing through anything that is not a preprocessor directive.
        skip_regular_routine:
            state->token = state->cursor;
            output->begin = state->token;

            /*!re2c
             new_line { state->state = RE2C_TOKENIZATION_STATE_NEW_LINE; goto start_next_token; }
             * { goto skip_regular_routine; }
             $ { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_END_OF_FILE); }
             */
        }

        /*!re2c
         whitespace+ { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_GLUE); }
         "\\" new_line { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_GLUE); }

         "//" [^\n]* { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_COMMENT); }
         multi_line_comment { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_COMMENT); }

         "/" "*" (. | new_line)*
         {
             context_execution_error (instance, state, "Encountered multiline comment that was never closed.");
             return;
         }

         new_line
         {
             state->state = RE2C_TOKENIZATION_STATE_NEW_LINE;
             PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_NEW_LINE);
         }

         "__VA_ARGS__" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_VA_ARGS); }
         "__VA_OPT__" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_VA_OPT); }

         "__CUSHION_PRESERVE__" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_CUSHION_PRESERVE); }

         "CUSHION_DEFER" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_CUSHION_DEFER); }
         "__CUSHION_WRAPPED__" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_CUSHION_WRAPPED); }
         "CUSHION_STATEMENT_ACCUMULATOR"
         { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR); }
         "CUSHION_STATEMENT_ACCUMULATOR_PUSH"
         { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_PUSH); }
         "CUSHION_STATEMENT_ACCUMULATOR_REFERENCE"
         { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_REFERENCE); }

         "defined" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_DEFINED); }
         "__has_include" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_HAS_INCLUDE); }
         "__has_embed" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_HAS_EMBED); }
         "__has_c_attribute" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_HAS_C_ATTRIBUTE); }
         "_Pragma" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_MACRO_PRAGMA); }

         "if" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_IF); }
         "for" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_FOR); }
         "while" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_WHILE); }
         "do" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_DO); }

         "return" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_RETURN); }
         "break" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_BREAK); }
         "continue" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_CONTINUE); }
         "goto" { PREPROCESSOR_EMIT_TOKEN_IDENTIFIER (IDENTIFIER_KIND_GOTO); }

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
         "!=" { PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR (PUNCTUATOR_KIND_LOGICAL_NOT_EQUAL); }

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
                 return;
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
                 return;
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
             return;
         }

         // Hexadecimal integer number. Minus in front is not a part of literal per specification.
         "0" [xX] @marker_sub_begin [0-9a-fA-F']+ @marker_sub_end integer_suffix?
         {
             if (tokenize_hex_value (marker_sub_begin, marker_sub_end, &output->unsigned_number_value) != RESULT_OK)
             {
                 context_execution_error (instance, state, "Failed to parse number due to overflow.");
                 return;
             }

             PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_NUMBER_INTEGER);
         }

         // Binary integer number. Minus in front is not a part of literal per specification.
         "0" [bB] @marker_sub_begin [01']+ @marker_sub_end integer_suffix?
         {
             if (tokenize_binary_value (marker_sub_begin, marker_sub_end, &output->unsigned_number_value) != RESULT_OK)
             {
                 context_execution_error (instance, state, "Failed to parse number due to overflow.");
                 return;
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

        break;
    }

    case RE2C_TOKENIZATION_STATE_NEW_LINE:
    {
        // Reset state to regular as next tokens would use regular anyway.
        state->state = RE2C_TOKENIZATION_STATE_REGULAR;
        goto new_line_check_for_preprocessor_begin;

    new_line_preprocessor_found:
        re2c_save_cursor (state);

    new_line_preprocessor_determine_type:
        /*!re2c
         !use:check_unsupported_in_code;

         // Whitespaces and comments prepending preprocessor command are just skipped..
         whitespace+ { goto new_line_preprocessor_determine_type; }
         multi_line_comment { goto new_line_preprocessor_determine_type; }

         "if" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_IF); }
         "ifdef" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_IFDEF); }
         "ifndef" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_IFNDEF); }
         "elif" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_ELIF); }
         "elifdef" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_ELIFDEF); }
         "elifndef" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_ELIFNDEF); }
         "else" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_ELSE); }
         "endif" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_ENDIF); }

         "include"
         {
             state->state = RE2C_TOKENIZATION_STATE_REGULAR;
             PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_INCLUDE);
         }

         "define" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_DEFINE); }
         "undef" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_UNDEF); }
         "line" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_LINE); }
         "pragma" { PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_PRAGMA); }

         // Fallthrough.
         identifier { }
         * { }
         $ { }
         */

        // This is a not preprocessor things that we care about. Just lex as hash and continue.
        if (state->flags & RE2C_TOKENIZATION_FLAGS_SKIP_REGULAR)
        {
            // If we're skipping regulars, no need to output punctuator.
            goto skip_regular_routine;
        }

        re2c_restore_saved_cursor (state);
        output->type = TOKEN_TYPE_PUNCTUATOR;
        output->end = state->cursor;
        output->punctuator_kind = PUNCTUATOR_KIND_HASH;
        return;

    new_line_check_for_preprocessor_begin:
        re2c_save_cursor (state);

        /*!re2c
         "#" { goto new_line_preprocessor_found; }
         * { }
         $ { }
         */

        re2c_restore_saved_cursor (state);
        // Nothing specific for new line found.
        goto regular_routine;
    }

    case RE2C_TOKENIZATION_STATE_INCLUDE:
    {
        if (state->flags & RE2C_TOKENIZATION_FLAGS_SKIP_REGULAR)
        {
            goto skip_regular_routine;
        }

        /*!re2c
         // Whitespaces and comments prepending include path are just skipped.
         whitespace+ { goto new_line_preprocessor_determine_type; }
         multi_line_comment { goto new_line_preprocessor_determine_type; }

         "<" @marker_sub_begin [^\n>]+ @marker_sub_end ">"
         {
             output->header_path.begin = marker_sub_begin;
             output->header_path.end = marker_sub_end;
             PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM);
         }

         "\"" @marker_sub_begin [^\n"]+ @marker_sub_end "\""
         {
             output->header_path.begin = marker_sub_begin;
             output->header_path.end = marker_sub_end;
             PREPROCESSOR_EMIT_TOKEN (TOKEN_TYPE_PREPROCESSOR_HEADER_USER);
         }

         "#" { goto new_line_preprocessor_found; }
         * { }
         $ { }
         */

        // Nothing specific for the include statement found.
        goto regular_routine;
    }
    }

#undef PREPROCESSOR_EMIT_TOKEN_CHARACTER_LITERAL
#undef PREPROCESSOR_EMIT_TOKEN_STRING_LITERAL
#undef PREPROCESSOR_EMIT_TOKEN_PUNCTUATOR
#undef PREPROCESSOR_EMIT_TOKEN_IDENTIFIER
#undef PREPROCESSOR_EMIT_TOKEN

    context_execution_error (instance, state, "Unexpected way to exit tokenizer, internal error.");
}

// Lexer implementation.

enum lex_replacement_list_result_t
{
    LEX_REPLACEMENT_LIST_RESULT_REGULAR = 0u,
    LEX_REPLACEMENT_LIST_RESULT_PRESERVED,
};

static enum lex_replacement_list_result_t lex_replacement_list (struct context_t *instance,
                                                                struct re2c_state_t *tokenization_state,
                                                                struct token_list_item_t **token_list_output)
{
    *token_list_output = NULL;
    struct token_list_item_t *first = NULL;
    struct token_list_item_t *last = NULL;
    struct token_t current_token;
    unsigned int lexing = 1u;

    while (lexing)
    {
        tokenization_next_token (instance, tokenization_state, &current_token);
        if (context_is_error_signaled (instance))
        {
            context_signal_error (instance);
            return LEX_REPLACEMENT_LIST_RESULT_REGULAR;
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
        struct token_list_item_t *new_token_item =                                                                     \
            save_token_to_memory (instance, &current_token, ALLOCATION_CLASS_PERSISTENT);                              \
        APPEND_TOKEN_TO_LIST (new_token_item);                                                                         \
    }

        switch (current_token.type)
        {
        case TOKEN_TYPE_PREPROCESSOR_IF:
        case TOKEN_TYPE_PREPROCESSOR_IFDEF:
        case TOKEN_TYPE_PREPROCESSOR_IFNDEF:
        case TOKEN_TYPE_PREPROCESSOR_ELIF:
        case TOKEN_TYPE_PREPROCESSOR_ELIFDEF:
        case TOKEN_TYPE_PREPROCESSOR_ELIFNDEF:
        case TOKEN_TYPE_PREPROCESSOR_ELSE:
        case TOKEN_TYPE_PREPROCESSOR_ENDIF:
        case TOKEN_TYPE_PREPROCESSOR_INCLUDE:
        case TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
        case TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
        case TOKEN_TYPE_PREPROCESSOR_DEFINE:
        case TOKEN_TYPE_PREPROCESSOR_UNDEF:
        case TOKEN_TYPE_PREPROCESSOR_LINE:
        case TOKEN_TYPE_PREPROCESSOR_PRAGMA:
            context_execution_error (instance, tokenization_state,
                                     "Encountered preprocessor directive while lexing replacement list. Shouldn't be "
                                     "possible at all, can be an internal error.");
            return LEX_REPLACEMENT_LIST_RESULT_REGULAR;

        case TOKEN_TYPE_IDENTIFIER:
            if (current_token.identifier_kind == IDENTIFIER_KIND_CUSHION_PRESERVE)
            {
                if (first)
                {
                    context_execution_error (
                        instance, tokenization_state,
                        "Encountered __CUSHION_PRESERVE__ while lexing replacement list and it is not first "
                        "replacement-list-significant token. When using __CUSHION_PRESERVE__ to avoid unwrapping "
                        "macro, it must always be the first thing in the replacement list.");
                    return LEX_REPLACEMENT_LIST_RESULT_REGULAR;
                }

                return LEX_REPLACEMENT_LIST_RESULT_PRESERVED;
            }

            SAVE_AND_APPEND_TOKEN_TO_LIST
            break;

        case TOKEN_TYPE_PUNCTUATOR:
        case TOKEN_TYPE_NUMBER_INTEGER:
        case TOKEN_TYPE_NUMBER_FLOATING:
        case TOKEN_TYPE_CHARACTER_LITERAL:
        case TOKEN_TYPE_STRING_LITERAL:
        case TOKEN_TYPE_OTHER:
            SAVE_AND_APPEND_TOKEN_TO_LIST
            break;

        case TOKEN_TYPE_NEW_LINE:
        case TOKEN_TYPE_END_OF_FILE:
            lexing = 0u;
            break;

        case TOKEN_TYPE_GLUE:
        case TOKEN_TYPE_COMMENT:
            // Glue and comments are ignored in replacement lists as replacements newer preserve formatting.
            break;
        }

#undef SAVE_AND_APPEND_TOKEN_TO_LIST
#undef APPEND_TOKEN_TO_LIST
    }

    *token_list_output = first;
    return LEX_REPLACEMENT_LIST_RESULT_REGULAR;
}

enum lex_file_flags_t
{
    LEX_FILE_FLAG_NONE = 0u,
    LEX_FILE_FLAG_SCAN_ONLY = 1u << 0u,
};

enum lexer_token_stack_item_flags_t
{
    LEXER_TOKEN_STACK_ITEM_FLAG_NONE = 0u,
    LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT,
};

struct lexer_token_stack_item_t
{
    struct lexer_token_stack_item_t *previous;
    struct token_list_item_t *tokens_current;
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
    CONDITIONAL_INCLUSION_FLAGS_HAD_PLAIN_ELSE = 1u << 0u,
};

struct lexer_conditional_inclusion_node_t
{
    struct lexer_conditional_inclusion_node_t *previous;
    enum lexer_conditional_inclusion_state_t state;
    enum lexer_conditional_inclusion_flags_t flags;
    unsigned int line;
};

struct lexer_path_buffer_t
{
    size_t size;
    char data[CUSHION_PATH_BUFFER_SIZE];
};

struct lexer_file_state_t
{
    unsigned int lexing;
    enum lex_file_flags_t flags;

    struct context_t *instance;
    struct lexer_token_stack_item_t *token_stack_top;
    struct lexer_conditional_inclusion_node_t *conditional_inclusion_node;

    /// \brief File name in lexer always points to the actual file.
    const char *file_name;

    struct re2c_state_t tokenization;

    struct lexer_path_buffer_t path_buffer;
};

static inline unsigned int lexer_file_state_should_continue (struct lexer_file_state_t *state)
{
    return state->lexing && !context_is_error_signaled (state->instance);
}

static inline void lexer_file_state_push_tokens (struct lexer_file_state_t *state,
                                                 struct token_list_item_t *tokens,
                                                 enum lexer_token_stack_item_flags_t flags)
{
    if (!tokens)
    {
        // Can be a macro with empty replacement list, just skip it.
        return;
    }

    struct lexer_token_stack_item_t *item =
        stack_group_allocator_allocate (&state->instance->allocator, sizeof (struct lexer_token_stack_item_t),
                                        _Alignof (struct lexer_token_stack_item_t), ALLOCATION_CLASS_TRANSIENT);

    item->previous = state->token_stack_top;
    item->tokens_current = tokens;
    item->flags = flags;
    state->token_stack_top = item;
}

static inline enum lexer_token_stack_item_flags_t lexer_file_state_pop_token (struct lexer_file_state_t *state,
                                                                              struct token_t *output)
{
    if (state->token_stack_top)
    {
        enum lexer_token_stack_item_flags_t flags = state->token_stack_top->flags;
        *output = state->token_stack_top->tokens_current->token;
        state->token_stack_top->tokens_current = state->token_stack_top->tokens_current->next;

        if (!state->token_stack_top->tokens_current)
        {
            // Stack list has ended, that means that macro replacement is no more, so we can pop out that stack item.
            state->token_stack_top = state->token_stack_top->previous;
        }

        if (output->type == TOKEN_TYPE_END_OF_FILE)
        {
            // Process lexing stop on end of file internally for ease of use.
            state->lexing = 0u;
        }

        return flags;
    }

    // No more ready-to-use tokens from replacement lists or other sources, request next token from tokenizer.
    tokenization_next_token (state->instance, &state->tokenization, output);
    return LEXER_TOKEN_STACK_ITEM_FLAG_NONE;
}

static inline void lexer_file_state_path_init (struct lexer_file_state_t *state, const char *data)
{
    const size_t in_size = strlen (data);
    if (in_size + 1u > CUSHION_PATH_BUFFER_SIZE)
    {
        context_execution_error (state->instance, &state->tokenization,
                                 "Failed to initialize path container, given path is too long: %s", data);
        return;
    }

    memcpy (state->path_buffer.data, data, in_size);
    state->path_buffer.data[in_size] = '\0';
    state->path_buffer.size = in_size;
}

static inline void lexer_file_state_path_append_sequence (struct lexer_file_state_t *state,
                                                          const char *sequence_begin,
                                                          const char *sequence_end)
{
    if (state->path_buffer.size > 0u && state->path_buffer.data[state->path_buffer.size - 1u] != '/' &&
        state->path_buffer.data[state->path_buffer.size - 1u] != '\\')
    {
        // Append directory separator.
        if (state->path_buffer.size >= CUSHION_PATH_BUFFER_SIZE)
        {
            context_execution_error (
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
        context_execution_error (
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
    struct token_list_item_t *tokens_first;
};

static unsigned int lex_calculate_stringized_internal_size (struct token_list_item_t *token)
{
    unsigned int size = 0u;
    while (token)
    {
        switch (token->token.type)
        {
        case TOKEN_TYPE_PREPROCESSOR_IF:
        case TOKEN_TYPE_PREPROCESSOR_IFDEF:
        case TOKEN_TYPE_PREPROCESSOR_IFNDEF:
        case TOKEN_TYPE_PREPROCESSOR_ELIF:
        case TOKEN_TYPE_PREPROCESSOR_ELIFDEF:
        case TOKEN_TYPE_PREPROCESSOR_ELIFNDEF:
        case TOKEN_TYPE_PREPROCESSOR_ELSE:
        case TOKEN_TYPE_PREPROCESSOR_ENDIF:
        case TOKEN_TYPE_PREPROCESSOR_INCLUDE:
        case TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
        case TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
        case TOKEN_TYPE_PREPROCESSOR_DEFINE:
        case TOKEN_TYPE_PREPROCESSOR_UNDEF:
        case TOKEN_TYPE_PREPROCESSOR_LINE:
        case TOKEN_TYPE_PREPROCESSOR_PRAGMA:
        case TOKEN_TYPE_NEW_LINE:
        case TOKEN_TYPE_GLUE:
        case TOKEN_TYPE_COMMENT:
        case TOKEN_TYPE_END_OF_FILE:
            // Must never be a part of stringizing sequence.
            assert (0);
            break;

        case TOKEN_TYPE_IDENTIFIER:
        case TOKEN_TYPE_PUNCTUATOR:
        case TOKEN_TYPE_NUMBER_INTEGER:
        case TOKEN_TYPE_NUMBER_FLOATING:
        case TOKEN_TYPE_OTHER:
            size += token->token.end - token->token.begin;
            break;

        case TOKEN_TYPE_CHARACTER_LITERAL:
        case TOKEN_TYPE_STRING_LITERAL:
        {
            size += token->token.end - token->token.begin;
            if (token->token.type == TOKEN_TYPE_STRING_LITERAL)
            {
                size += 2u; // Two more character for escaping double quotes.
                break;
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

static char *lex_write_stringized_internal_tokens (struct token_list_item_t *token, char *output)
{
    while (token)
    {
        switch (token->token.type)
        {
        case TOKEN_TYPE_PREPROCESSOR_IF:
        case TOKEN_TYPE_PREPROCESSOR_IFDEF:
        case TOKEN_TYPE_PREPROCESSOR_IFNDEF:
        case TOKEN_TYPE_PREPROCESSOR_ELIF:
        case TOKEN_TYPE_PREPROCESSOR_ELIFDEF:
        case TOKEN_TYPE_PREPROCESSOR_ELIFNDEF:
        case TOKEN_TYPE_PREPROCESSOR_ELSE:
        case TOKEN_TYPE_PREPROCESSOR_ENDIF:
        case TOKEN_TYPE_PREPROCESSOR_INCLUDE:
        case TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
        case TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
        case TOKEN_TYPE_PREPROCESSOR_DEFINE:
        case TOKEN_TYPE_PREPROCESSOR_UNDEF:
        case TOKEN_TYPE_PREPROCESSOR_LINE:
        case TOKEN_TYPE_PREPROCESSOR_PRAGMA:
        case TOKEN_TYPE_NEW_LINE:
        case TOKEN_TYPE_GLUE:
        case TOKEN_TYPE_COMMENT:
        case TOKEN_TYPE_END_OF_FILE:
            // Must never be a part of stringizing sequence.
            assert (0);
            break;

        case TOKEN_TYPE_IDENTIFIER:
        case TOKEN_TYPE_PUNCTUATOR:
        case TOKEN_TYPE_NUMBER_INTEGER:
        case TOKEN_TYPE_NUMBER_FLOATING:
        case TOKEN_TYPE_OTHER:
            memcpy (output, token->token.begin, token->token.end - token->token.begin);
            output += token->token.end - token->token.begin;
            break;

        case TOKEN_TYPE_CHARACTER_LITERAL:
        case TOKEN_TYPE_STRING_LITERAL:
        {
            if (token->token.begin + 1u != token->token.symbolic_literal.begin)
            {
                // Copy encoding prefix.
                memcpy (output, token->token.begin, token->token.symbolic_literal.begin - token->token.begin - 1u);
                output += token->token.symbolic_literal.begin - token->token.begin - 1u;
            }

            if (token->token.type == TOKEN_TYPE_CHARACTER_LITERAL)
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

            if (token->token.type == TOKEN_TYPE_CHARACTER_LITERAL)
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

static enum identifier_kind_t lex_relculate_identifier_kind (const char *begin, const char *end)
{
    // Good candidate for future optimization, but should not be that impactful right now.
    const size_t length = end - begin;

#define CHECK_KIND(LITERAL, VALUE)                                                                                     \
    if (length == sizeof (LITERAL) - 1u && strncmp (begin, "identifier", length) == 0)                                 \
    {                                                                                                                  \
        return VALUE;                                                                                                  \
    }

    CHECK_KIND ("__VA_ARGS__", IDENTIFIER_KIND_VA_ARGS)
    CHECK_KIND ("__VA_OPT__", IDENTIFIER_KIND_VA_OPT)

    CHECK_KIND ("__CUSHION_PRESERVE__", IDENTIFIER_KIND_CUSHION_PRESERVE)

    CHECK_KIND ("CUSHION_DEFER", IDENTIFIER_KIND_CUSHION_DEFER)
    CHECK_KIND ("__CUSHION_WRAPPED__", IDENTIFIER_KIND_CUSHION_WRAPPED)
    CHECK_KIND ("CUSHION_STATEMENT_ACCUMULATOR", IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR)
    CHECK_KIND ("CUSHION_STATEMENT_ACCUMULATOR_PUSH", IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_PUSH)
    CHECK_KIND ("CUSHION_STATEMENT_ACCUMULATOR_REFERENCE", IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_REFERENCE)

    CHECK_KIND ("defined", IDENTIFIER_KIND_DEFINED)
    CHECK_KIND ("__has_include", IDENTIFIER_KIND_HAS_INCLUDE)
    CHECK_KIND ("__has_embed", IDENTIFIER_KIND_HAS_EMBED)
    CHECK_KIND ("__has_c_attribute", IDENTIFIER_KIND_HAS_C_ATTRIBUTE)
    CHECK_KIND ("__Pragma", IDENTIFIER_KIND_MACRO_PRAGMA)

    CHECK_KIND ("if", IDENTIFIER_KIND_IF)
    CHECK_KIND ("for", IDENTIFIER_KIND_FOR)
    CHECK_KIND ("while", IDENTIFIER_KIND_WHILE)
    CHECK_KIND ("do", IDENTIFIER_KIND_DO)

    CHECK_KIND ("return", IDENTIFIER_KIND_RETURN)
    CHECK_KIND ("break", IDENTIFIER_KIND_BREAK)
    CHECK_KIND ("continue", IDENTIFIER_KIND_CONTINUE)
    CHECK_KIND ("goto", IDENTIFIER_KIND_GOTO)
#undef CHECK_KIND

    return IDENTIFIER_KIND_REGULAR;
}

struct macro_replacement_token_list_t
{
    struct token_list_item_t *first;
    struct token_list_item_t *last;
};

struct macro_replacement_context_t
{
    struct macro_node_t *macro;
    struct lex_macro_argument_t *arguments;
    struct token_list_item_t *current_token;
    struct macro_replacement_token_list_t result;
    struct macro_replacement_token_list_t sub_list;
};

static inline void macro_replacement_token_list_append (struct lexer_file_state_t *state,
                                                        struct macro_replacement_token_list_t *list,
                                                        struct token_t *token)
{
    struct token_list_item_t *new_token =
        stack_group_allocator_allocate (&state->instance->allocator, sizeof (struct token_list_item_t),
                                        _Alignof (struct token_list_item_t), ALLOCATION_CLASS_TRANSIENT);

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
    struct lexer_file_state_t *state, struct macro_replacement_context_t *context)
{
    context->sub_list.first = NULL;
    context->sub_list.last = NULL;

    if (context->current_token->token.identifier_kind == IDENTIFIER_KIND_VA_ARGS ||
        context->current_token->token.identifier_kind == IDENTIFIER_KIND_VA_OPT)
    {
        if ((context->macro->flags & MACRO_FLAG_VARIADIC_PARAMETERS) == 0u)
        {
            context_execution_error (state->instance, &state->tokenization,
                                     "Caught attempt to use __VA_ARGS__/__VA_OPT__ in non-variadic macro.");
            return;
        }

        struct macro_parameter_node_t *macro_parameter = context->macro->parameters_first;
        struct lex_macro_argument_t *first_variadic_argument = context->arguments;

        while (macro_parameter)
        {
            macro_parameter = macro_parameter->next;
            // There should be no fewer arguments than parameters, otherwise call is malformed.
            assert (first_variadic_argument);
            first_variadic_argument = first_variadic_argument->next;
        }

        if (context->current_token->token.identifier_kind == IDENTIFIER_KIND_VA_ARGS)
        {
            struct lex_macro_argument_t *argument = first_variadic_argument;
            while (argument)
            {
                struct token_list_item_t *argument_token = argument->tokens_first;
                while (argument_token)
                {
                    macro_replacement_token_list_append (state, &context->sub_list, &argument_token->token);
                    argument_token = argument_token->next;
                }

                argument = argument->next;
                if (argument)
                {
                    static const char *static_token_string_comma = ",";
                    struct token_t token_comma = {
                        .type = TOKEN_TYPE_PUNCTUATOR,
                        .begin = static_token_string_comma,
                        .end = static_token_string_comma + 1u,
                        .punctuator_kind = PUNCTUATOR_KIND_COMMA,
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

            if (!context->current_token || context->current_token->token.type != TOKEN_TYPE_PUNCTUATOR ||
                context->current_token->token.punctuator_kind != PUNCTUATOR_KIND_LEFT_PARENTHESIS)
            {
                context_execution_error (state->instance, &state->tokenization,
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
                    context_execution_error (state->instance, &state->tokenization,
                                             "Got to the end of replacement list while lexing __VA_OPT__.");
                    break;
                }

                // Preprocessor, new line, glue and end of file should never appear here anyway.
                if (context->current_token->token.type == TOKEN_TYPE_PUNCTUATOR)
                {
                    if (context->current_token->token.punctuator_kind == PUNCTUATOR_KIND_LEFT_PARENTHESIS)
                    {
                        ++internal_parenthesis;
                    }
                    else if (context->current_token->token.punctuator_kind == PUNCTUATOR_KIND_RIGHT_PARENTHESIS)
                    {
                        if (internal_parenthesis == 0u)
                        {
                            // Whole __VA_OPT__ is lexed.
                            break;
                        }

                        --internal_parenthesis;
                    }
                }

                macro_replacement_token_list_append (state, &context->sub_list, &context->current_token->token);
                context->current_token = context->current_token->next;
            }
        }
    }
    else
    {
        struct lex_macro_argument_t *found_argument = context->arguments;
        struct macro_parameter_node_t *found_parameter = context->macro->parameters_first;

        if (found_parameter)
        {
            const unsigned int hash =
                hash_djb2_char_sequence (context->current_token->token.begin, context->current_token->token.end);
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

        if (found_argument)
        {
            struct token_list_item_t *argument_token = found_argument->tokens_first;
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

static struct token_list_item_t *lex_do_macro_replacement (struct lexer_file_state_t *state,
                                                           struct macro_node_t *macro,
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

    while (context.current_token && !context_is_error_signaled (state->instance))
    {
        switch (context.current_token->token.type)
        {
        case TOKEN_TYPE_PREPROCESSOR_IF:
        case TOKEN_TYPE_PREPROCESSOR_IFDEF:
        case TOKEN_TYPE_PREPROCESSOR_IFNDEF:
        case TOKEN_TYPE_PREPROCESSOR_ELIF:
        case TOKEN_TYPE_PREPROCESSOR_ELIFDEF:
        case TOKEN_TYPE_PREPROCESSOR_ELIFNDEF:
        case TOKEN_TYPE_PREPROCESSOR_ELSE:
        case TOKEN_TYPE_PREPROCESSOR_ENDIF:
        case TOKEN_TYPE_PREPROCESSOR_INCLUDE:
        case TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
        case TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
        case TOKEN_TYPE_PREPROCESSOR_DEFINE:
        case TOKEN_TYPE_PREPROCESSOR_UNDEF:
        case TOKEN_TYPE_PREPROCESSOR_LINE:
        case TOKEN_TYPE_PREPROCESSOR_PRAGMA:
        case TOKEN_TYPE_NEW_LINE:
        case TOKEN_TYPE_COMMENT:
        case TOKEN_TYPE_GLUE:
        case TOKEN_TYPE_END_OF_FILE:
            // Must never be a part of valid lexed macro.
            assert (0);
            break;

        case TOKEN_TYPE_IDENTIFIER:
        {
            macro_replacement_context_process_identifier_into_sub_list (state, &context);
            if (!context_is_error_signaled (state->instance))
            {
                macro_replacement_context_append_sub_list (&context);
            }

            break;
        }

        case TOKEN_TYPE_PUNCTUATOR:
            switch (context.current_token->token.punctuator_kind)
            {
            case PUNCTUATOR_KIND_HASH:
            {
                // Stringify next argument, skipping comments. Error if next token is not an argument.
                context.current_token = context.current_token->next;
                // Preprocessor, new line, glue, comment and end of file should never appear here anyway.

                if (!context.current_token)
                {
                    context_execution_error (state->instance, &state->tokenization,
                                             "Encountered \"#\" operator as a last token in macro replacement list.");
                    break;
                }

                if (context.current_token->token.type != TOKEN_TYPE_IDENTIFIER)
                {
                    context_execution_error (state->instance, &state->tokenization,
                                             "Non-comment token following \"#\" operator is not an identifier.");
                    break;
                }

                if (context.current_token->token.identifier_kind == IDENTIFIER_KIND_VA_ARGS)
                {
                    if ((macro->flags & MACRO_FLAG_VARIADIC_PARAMETERS) == 0u)
                    {
                        context_execution_error (
                            state->instance, &state->tokenization,
                            "Caught attempt to stringize variadic arguments in non-variadic macro.");
                        break;
                    }

                    struct macro_parameter_node_t *macro_parameter = macro->parameters_first;
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

                    struct token_t stringized_token;
                    stringized_token.type = TOKEN_TYPE_STRING_LITERAL;
                    stringized_token.begin = stack_group_allocator_allocate (
                        &state->instance->allocator, stringized_size + 1u, _Alignof (char), ALLOCATION_CLASS_TRANSIENT);

                    stringized_token.end = stringized_token.begin + stringized_size;
                    *(char *) stringized_token.end = '\0';

                    stringized_token.symbolic_literal.begin = stringized_token.begin + 1u;
                    stringized_token.symbolic_literal.end = stringized_token.end - 1u;
                    char *output = (char *) stringized_token.symbolic_literal.begin;
                    current_argument = first_variadic_argument;

                    while (current_argument)
                    {
                        output = lex_write_stringized_internal_tokens (current_argument->tokens_first, output);
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

                const unsigned int hash =
                    hash_djb2_char_sequence (context.current_token->token.begin, context.current_token->token.end);
                const size_t token_length = context.current_token->token.end - context.current_token->token.begin;
                struct macro_parameter_node_t *found_parameter = macro->parameters_first;
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
                    context_execution_error (
                        state->instance, &state->tokenization,
                        "Identifier token following \"#\" operator is neither argument name nor __VA_ARGS__.");
                    break;
                }

                const unsigned int stringized_size =
                    2u + lex_calculate_stringized_internal_size (found_argument->tokens_first);

                struct token_t stringized_token;
                stringized_token.type = TOKEN_TYPE_STRING_LITERAL;
                stringized_token.begin = stack_group_allocator_allocate (
                    &state->instance->allocator, stringized_size + 1u, _Alignof (char), ALLOCATION_CLASS_TRANSIENT);

                stringized_token.end = stringized_token.begin + stringized_size;
                *(char *) stringized_token.end = '\0';

                stringized_token.symbolic_literal.begin = stringized_token.begin + 1u;
                stringized_token.symbolic_literal.end = stringized_token.end - 1u;
                lex_write_stringized_internal_tokens (found_argument->tokens_first,
                                                      (char *) stringized_token.symbolic_literal.begin);

                *(char *) stringized_token.begin = '"';
                *(char *) stringized_token.symbolic_literal.end = '"';
                macro_replacement_token_list_append (state, &context.result, &stringized_token);
                break;
            }

            case PUNCTUATOR_KIND_DOUBLE_HASH:
            {
                // Technically, we could optimize token-append operation by firstly calculating whole append sequence
                // and then doing merge, but it makes implementation more complicated and should not affect performance
                // that much, therefore simple merge is used right now.

                if (!context.result.last)
                {
                    context_execution_error (state->instance, &state->tokenization,
                                             "Encountered \"##\" operator as a first token in macro replacement list.");
                    break;
                }

                if (context.result.last->token.type != TOKEN_TYPE_IDENTIFIER)
                {
                    context_execution_error (state->instance, &state->tokenization,
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
                        context_execution_error (
                            state->instance, &state->tokenization,
                            "Encountered \"##\" operator as a last token in macro replacement list.");
                        break;
                    }

                    if (context.current_token->token.type != TOKEN_TYPE_IDENTIFIER)
                    {
                        context_execution_error (state->instance, &state->tokenization,
                                                 "Encountered \"##\" operator before non-identifier token, which is "
                                                 "currently not supported by Cushion (but possible in standard).");
                        break;
                    }

                    macro_replacement_context_process_identifier_into_sub_list (state, &context);
                    if (!context.sub_list.first)
                    {
                        // Empty sub list, check next identifier.
                        continue;
                    }

                    if (context.sub_list.first->token.type != TOKEN_TYPE_IDENTIFIER)
                    {
                        context_execution_error (state->instance, &state->tokenization,
                                                 "Encountered \"##\" operator before non-identifier token, which is "
                                                 "currently not supported by Cushion (but possible in standard).");
                        break;
                    }

                    unsigned int base_identifier_length =
                        context.result.last->token.end - context.result.last->token.begin;
                    unsigned int append_identifier_length =
                        context.sub_list.first->token.end - context.sub_list.first->token.begin;

                    char *new_token_data = stack_group_allocator_allocate (
                        &state->instance->allocator, base_identifier_length + append_identifier_length + 1u,
                        _Alignof (char), ALLOCATION_CLASS_TRANSIENT);
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

        case TOKEN_TYPE_NUMBER_INTEGER:
        case TOKEN_TYPE_NUMBER_FLOATING:
        case TOKEN_TYPE_CHARACTER_LITERAL:
        case TOKEN_TYPE_STRING_LITERAL:
        case TOKEN_TYPE_OTHER:
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

static struct token_list_item_t *lex_replace_identifier_if_macro (
    struct lexer_file_state_t *state,
    struct token_t *identifier_token,
    enum lex_replace_identifier_if_macro_context_t context);

static inline void lex_preprocessor_fixup_line (struct lexer_file_state_t *state)
{
    if ((state->flags & LEX_FILE_FLAG_SCAN_ONLY) == 0u &&
        (!state->conditional_inclusion_node ||
         state->conditional_inclusion_node->state == CONDITIONAL_INCLUSION_STATE_INCLUDED))
    {
        // Make sure that next portion of code starts with correct line directive.
        // Useful for conditional inclusion and for file includes.
        context_output_line_marker (state->instance, state->tokenization.cursor_line, state->tokenization.file_name);
    }
}

#define LEX_WHEN_ERROR(...)                                                                                            \
    if (context_is_error_signaled (state->instance))                                                                   \
    {                                                                                                                  \
        __VA_ARGS__;                                                                                                   \
    }

static inline void lex_preprocessor_preserved_tail (
    struct lexer_file_state_t *state,
    enum token_type_t preprocessor_token_type,
    // Macro node is required to properly paste function-line macro arguments if any.
    struct macro_node_t *macro_node_if_any)
{
    if (state->flags & LEX_FILE_FLAG_SCAN_ONLY)
    {
        // When using scan only, we're not outputting anything, therefore no need for actual pass.
        return;
    }

    switch (preprocessor_token_type)
    {
    case TOKEN_TYPE_PREPROCESSOR_IF:
        context_output_null_terminated (state->instance, "#if ");
        break;

    case TOKEN_TYPE_PREPROCESSOR_IFDEF:
    case TOKEN_TYPE_PREPROCESSOR_IFNDEF:
        // This token should not support preserve logic.
        assert (0u);
        break;

    case TOKEN_TYPE_PREPROCESSOR_ELIF:
        context_output_null_terminated (state->instance, "#elif ");
        break;

    case TOKEN_TYPE_PREPROCESSOR_ELIFDEF:
        context_output_null_terminated (state->instance, "#elifdef ");
        break;

    case TOKEN_TYPE_PREPROCESSOR_ELIFNDEF:
        context_output_null_terminated (state->instance, "#elifndef ");
        break;

    case TOKEN_TYPE_PREPROCESSOR_ELSE:
        context_output_null_terminated (state->instance, "#else ");
        break;

    case TOKEN_TYPE_PREPROCESSOR_ENDIF:
        context_output_null_terminated (state->instance, "#endif ");
        break;

    case TOKEN_TYPE_PREPROCESSOR_INCLUDE:
    case TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
    case TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
    case TOKEN_TYPE_PREPROCESSOR_UNDEF:
    case TOKEN_TYPE_PREPROCESSOR_LINE:
        // This token should not support preserve logic.
        assert (0u);
        break;

    case TOKEN_TYPE_PREPROCESSOR_PRAGMA:
        context_output_null_terminated (state->instance, "#pragma ");
        break;

    case TOKEN_TYPE_PREPROCESSOR_DEFINE:
    {
        context_output_null_terminated (state->instance, "#define ");
        // We need this to properly paste everything back into code.
        assert (macro_node_if_any);
        context_output_null_terminated (state->instance, macro_node_if_any->name);

        if (macro_node_if_any->flags & MACRO_FLAG_FUNCTION)
        {
            context_output_null_terminated (state->instance, "(");
            struct macro_parameter_node_t *parameter = macro_node_if_any->parameters_first;

            while (parameter)
            {
                context_output_null_terminated (state->instance, parameter->name);
                parameter = parameter->next;

                if (parameter)
                {
                    context_output_null_terminated (state->instance, ", ");
                }
            }

            if (macro_node_if_any->flags & MACRO_FLAG_VARIADIC_PARAMETERS)
            {
                if (macro_node_if_any->parameters_first)
                {
                    context_output_null_terminated (state->instance, ", ");
                }

                context_output_null_terminated (state->instance, "...");
            }

            context_output_null_terminated (state->instance, ") ");
        }
        else
        {
            context_output_null_terminated (state->instance, " ");
        }

        break;
    }

    case TOKEN_TYPE_IDENTIFIER:
    case TOKEN_TYPE_PUNCTUATOR:
    case TOKEN_TYPE_NUMBER_INTEGER:
    case TOKEN_TYPE_NUMBER_FLOATING:
    case TOKEN_TYPE_CHARACTER_LITERAL:
    case TOKEN_TYPE_STRING_LITERAL:
    case TOKEN_TYPE_NEW_LINE:
    case TOKEN_TYPE_GLUE:
    case TOKEN_TYPE_COMMENT:
    case TOKEN_TYPE_END_OF_FILE:
    case TOKEN_TYPE_OTHER:
        // We shouldn't pass regular tokens as preprocessor tokens.
        assert (0u);
        break;
    }

    struct token_t current_token;
    while (lexer_file_state_should_continue (state))
    {
        lexer_file_state_pop_token (state, &current_token);
        LEX_WHEN_ERROR (break)

        switch (current_token.type)
        {
        case TOKEN_TYPE_PREPROCESSOR_IF:
        case TOKEN_TYPE_PREPROCESSOR_IFDEF:
        case TOKEN_TYPE_PREPROCESSOR_IFNDEF:
        case TOKEN_TYPE_PREPROCESSOR_ELIF:
        case TOKEN_TYPE_PREPROCESSOR_ELIFDEF:
        case TOKEN_TYPE_PREPROCESSOR_ELIFNDEF:
        case TOKEN_TYPE_PREPROCESSOR_ELSE:
        case TOKEN_TYPE_PREPROCESSOR_ENDIF:
        case TOKEN_TYPE_PREPROCESSOR_INCLUDE:
        case TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
        case TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
        case TOKEN_TYPE_PREPROCESSOR_DEFINE:
        case TOKEN_TYPE_PREPROCESSOR_UNDEF:
        case TOKEN_TYPE_PREPROCESSOR_LINE:
        case TOKEN_TYPE_PREPROCESSOR_PRAGMA:
            context_execution_error (state->instance, &state->tokenization,
                                     "Encountered preprocessor directive while lexing preserved preprocessor "
                                     "directive. Shouldn't be possible at all, can be an internal error.");
            return;

        case TOKEN_TYPE_IDENTIFIER:
        {
            switch (current_token.identifier_kind)
            {
            case IDENTIFIER_KIND_CUSHION_PRESERVE:
                context_execution_error (state->instance, &state->tokenization,
                                         "Encountered cushion preserve keyword in unexpected context.");
                return;

            case IDENTIFIER_KIND_CUSHION_WRAPPED:
                context_execution_error (state->instance, &state->tokenization,
                                         "Encountered cushion wrapped keyword in preserved preprocessor context.");
                return;

            default:
                break;
            }

            struct token_list_item_t *macro_tokens = lex_replace_identifier_if_macro (
                state, &current_token, LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_EVALUATION);

            if (macro_tokens)
            {
                lexer_file_state_push_tokens (state, macro_tokens, LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT);
            }
            else
            {
                // Not a macro, just regular identifier.
                context_output_sequence (state->instance, current_token.begin, current_token.end);
            }

            break;
        }

        case TOKEN_TYPE_PUNCTUATOR:
        case TOKEN_TYPE_NUMBER_INTEGER:
        case TOKEN_TYPE_NUMBER_FLOATING:
        case TOKEN_TYPE_CHARACTER_LITERAL:
        case TOKEN_TYPE_STRING_LITERAL:
        case TOKEN_TYPE_GLUE:
        case TOKEN_TYPE_OTHER:
            context_output_sequence (state->instance, current_token.begin, current_token.end);
            break;

        case TOKEN_TYPE_NEW_LINE:
            // New line, tail has ended, paste the new line and return out of here.
            context_output_sequence (state->instance, current_token.begin, current_token.end);
            // Fixup line in case of skipped multi-line comments.
            lex_preprocessor_fixup_line (state);
            return;

        case TOKEN_TYPE_COMMENT:
            // We erase comments.
            break;

        case TOKEN_TYPE_END_OF_FILE:
            context_execution_error (state->instance, &state->tokenization,
                                     "Encountered end of file while reading preserved preprocessor if.");
            state->lexing = 0u;
            break;
        }
    }
}

static struct token_list_item_t *lex_replace_identifier_if_macro (
    struct lexer_file_state_t *state,
    struct token_t *identifier_token,
    enum lex_replace_identifier_if_macro_context_t context)
{
    struct macro_node_t *macro = context_macro_search (state->instance, identifier_token->begin, identifier_token->end);
    if (!macro || (macro->flags & MACRO_FLAG_PRESERVED))
    {
        // No need to unwrap.
        return NULL;
    }

    struct token_list_item_t *result_tokens_first = NULL;
    struct token_list_item_t *result_tokens_last = NULL;

    struct lex_macro_argument_t *arguments_first = NULL;
    struct lex_macro_argument_t *arguments_last = NULL;

    if (macro->flags & MACRO_FLAG_FUNCTION)
    {
        /* Scan for the opening parenthesis. */
        struct token_t current_token;

        struct token_list_item_t *argument_tokens_first = NULL;
        struct token_list_item_t *argument_tokens_last = NULL;
        unsigned int skipping_until_significant = 1u;

        while (skipping_until_significant && !context_is_error_signaled (state->instance))
        {
            lexer_file_state_pop_token (state, &current_token);
            LEX_WHEN_ERROR (break)

            switch (current_token.type)
            {
            case TOKEN_TYPE_NEW_LINE:
                switch (context)
                {
                case LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_CODE:
                    // As macro replacement cannot have new lines inside them, then we cannot be in macro replacement
                    // and therefore can freely output new line.
                    context_output_sequence (state->instance, current_token.begin, current_token.end);
                    break;

                case LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_EVALUATION:
                    context_execution_error (state->instance, &state->tokenization,
                                             "Reached new line while expecting \"(\" after function-line macro name "
                                             "inside preprocessor directive evaluation.");
                    break;
                }

                break;

            case TOKEN_TYPE_GLUE:
            case TOKEN_TYPE_COMMENT:
                // Glue and comments between macro name and arguments are just skipped.
                break;

            default:
                // Found something significant.
                skipping_until_significant = 0u;
                break;
            }
        }

        LEX_WHEN_ERROR (return NULL)
        if (current_token.type != TOKEN_TYPE_PUNCTUATOR ||
            current_token.punctuator_kind != PUNCTUATOR_KIND_LEFT_PARENTHESIS)
        {
            context_execution_error (state->instance, &state->tokenization,
                                     "Expected \"(\" after function-line macro name.");
            return NULL;
        }

        unsigned int parenthesis_counter = 1u;
        struct macro_parameter_node_t *parameter = macro->parameters_first;
        unsigned int parameterless_function_line = !parameter && (macro->flags & MACRO_FLAG_VARIADIC_PARAMETERS) == 0u;

        while (parenthesis_counter > 0u && !context_is_error_signaled (state->instance))
        {
            lexer_file_state_pop_token (state, &current_token);
            LEX_WHEN_ERROR (break)

            switch (current_token.type)
            {
            case TOKEN_TYPE_PUNCTUATOR:
                switch (current_token.punctuator_kind)
                {
                case PUNCTUATOR_KIND_LEFT_PARENTHESIS:
                    ++parenthesis_counter;
                    goto append_argument_token;
                    break;

                case PUNCTUATOR_KIND_RIGHT_PARENTHESIS:
                    --parenthesis_counter;

                    if (parenthesis_counter == 0u && !parameterless_function_line)
                    {
                        goto finalize_argument;
                    }

                    break;

                case PUNCTUATOR_KIND_COMMA:
                finalize_argument:
                {
                    if (parameterless_function_line)
                    {
                        context_execution_error (state->instance, &state->tokenization,
                                                 "Encountered more parameters for function-line macro than expected.");
                        break;
                    }

                    struct lex_macro_argument_t *new_argument = stack_group_allocator_allocate (
                        &state->instance->allocator, sizeof (struct lex_macro_argument_t),
                        _Alignof (struct lex_macro_argument_t), ALLOCATION_CLASS_TRANSIENT);

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
                    assert (parameter || (macro->flags & MACRO_FLAG_VARIADIC_PARAMETERS));

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

            case TOKEN_TYPE_NEW_LINE:
                switch (context)
                {
                case LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_CODE:
                    // As macro replacement cannot have new lines inside them, then we cannot be in macro replacement
                    // and therefore can freely output new line while parsing arguments.
                    context_output_sequence (state->instance, current_token.begin, current_token.end);
                    break;

                case LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_EVALUATION:
                    context_execution_error (state->instance, &state->tokenization,
                                             "Reached new line while parsing arguments of function-line macro inside "
                                             "preprocessor directive evaluation.");
                    break;
                }

                break;

            case TOKEN_TYPE_GLUE:
            case TOKEN_TYPE_COMMENT:
                // Glue and comments are not usually kept in macro arguments.
                break;

            case TOKEN_TYPE_END_OF_FILE:
                context_execution_error (state->instance, &state->tokenization,
                                         "Got to the end of file while parsing arguments of function-like macro.");
                break;

            default:
            append_argument_token:
            {
                if (!parameter && (macro->flags & MACRO_FLAG_VARIADIC_PARAMETERS) == 0u)
                {
                    context_execution_error (state->instance, &state->tokenization,
                                             "Encountered more parameters for function-line macro than expected.");
                    break;
                }

                struct token_list_item_t *argument_token =
                    save_token_to_memory (state->instance, &current_token, ALLOCATION_CLASS_TRANSIENT);

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
            context_execution_error (state->instance, &state->tokenization,
                                     "Encountered less parameters for function-line macro than expected.");
            return NULL;
        }
    }

    struct token_list_item_t *replacement = lex_do_macro_replacement (state, macro, arguments_first);
    if (result_tokens_last)
    {
        result_tokens_first = replacement;
    }
    else
    {
        result_tokens_last->next = replacement;
    }

    return result_tokens_first;
}

enum lex_preprocessor_sub_expression_type_t
{
    LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_ROOT = 0u,
    LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_PARENTHESIS,
    LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_POSITIVE,
    LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_NEGATIVE,
};

static long long lex_preprocessor_evaluate (struct lexer_file_state_t *state,
                                            enum lex_preprocessor_sub_expression_type_t sub_expression_type);

static inline void lex_skip_glue_and_comments (struct lexer_file_state_t *state, struct token_t *current_token)
{
    while (lexer_file_state_should_continue (state))
    {
        lexer_file_state_pop_token (state, current_token);
        LEX_WHEN_ERROR (return)

        switch (current_token->type)
        {
        case TOKEN_TYPE_GLUE:
        case TOKEN_TYPE_COMMENT:
            break;

        default:
            return;
        }
    }
}

static long long lex_do_defined_check (struct lexer_file_state_t *state, struct token_t *current_token)
{
    switch (current_token->type)
    {
    case TOKEN_TYPE_IDENTIFIER:
        switch (current_token->identifier_kind)
        {
        case IDENTIFIER_KIND_VA_ARGS:
        case IDENTIFIER_KIND_VA_OPT:
        case IDENTIFIER_KIND_CUSHION_PRESERVE:
        case IDENTIFIER_KIND_CUSHION_DEFER:
        case IDENTIFIER_KIND_CUSHION_WRAPPED:
        case IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR:
        case IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_PUSH:
        case IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_REFERENCE:
            context_execution_error (state->instance, &state->tokenization,
                                     "Encountered unsupported reserved identifier in defined check.");
            return 0u;

        default:
            return context_macro_search (state->instance, current_token->begin, current_token->end) ? 1u : 0u;
        }

        break;

    default:
        context_execution_error (state->instance, &state->tokenization, "Expected identifier for defined check.");
        return 0u;
    }

    return 0u;
}

static long long lex_preprocessor_evaluate_defined (struct lexer_file_state_t *state)
{
    struct token_t current_token;
    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return 0u)

    if (current_token.type != TOKEN_TYPE_PUNCTUATOR ||
        current_token.punctuator_kind != PUNCTUATOR_KIND_LEFT_PARENTHESIS)
    {
        context_execution_error (state->instance, &state->tokenization,
                                 "Expected \"(\" after \"defined\" in preprocessor expression evaluation.");
        return 0u;
    }

    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return 0u)

    long long result = lex_do_defined_check (state, &current_token);
    LEX_WHEN_ERROR (return 0u)

    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return 0u)

    if (current_token.type != TOKEN_TYPE_PUNCTUATOR ||
        current_token.punctuator_kind != PUNCTUATOR_KIND_RIGHT_PARENTHESIS)
    {
        context_execution_error (
            state->instance, &state->tokenization,
            "Expected \")\" after macro name in \"defined\" in preprocessor expression evaluation.");
        return result;
    }

    return result;
}

static long long lex_preprocessor_evaluate_argument (struct lexer_file_state_t *state,
                                                     enum lex_preprocessor_sub_expression_type_t sub_expression_type)
{
    struct token_t current_token;
    while (!lexer_file_state_should_continue (state))
    {
        lexer_file_state_pop_token (state, &current_token);
        LEX_WHEN_ERROR (break)

        switch (current_token.type)
        {
        case TOKEN_TYPE_PREPROCESSOR_IF:
        case TOKEN_TYPE_PREPROCESSOR_IFDEF:
        case TOKEN_TYPE_PREPROCESSOR_IFNDEF:
        case TOKEN_TYPE_PREPROCESSOR_ELIF:
        case TOKEN_TYPE_PREPROCESSOR_ELIFDEF:
        case TOKEN_TYPE_PREPROCESSOR_ELIFNDEF:
        case TOKEN_TYPE_PREPROCESSOR_ELSE:
        case TOKEN_TYPE_PREPROCESSOR_ENDIF:
        case TOKEN_TYPE_PREPROCESSOR_INCLUDE:
        case TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
        case TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
        case TOKEN_TYPE_PREPROCESSOR_DEFINE:
        case TOKEN_TYPE_PREPROCESSOR_UNDEF:
        case TOKEN_TYPE_PREPROCESSOR_LINE:
        case TOKEN_TYPE_PREPROCESSOR_PRAGMA:
            context_execution_error (state->instance, &state->tokenization,
                                     "Encountered preprocessor directive while evaluating preprocessor conditional "
                                     "expression. Shouldn't be possible at all, can be an internal error.");
            return 0;

        case TOKEN_TYPE_IDENTIFIER:
        {
            switch (current_token.identifier_kind)
            {
            case IDENTIFIER_KIND_DEFINED:
                return lex_preprocessor_evaluate_defined (state);

            case IDENTIFIER_KIND_HAS_INCLUDE:
            case IDENTIFIER_KIND_HAS_EMBED:
            case IDENTIFIER_KIND_HAS_C_ATTRIBUTE:
            {
                context_execution_error (
                    state->instance, &state->tokenization,
                    "Encountered has_* check while evaluation preprocessor conditional expression. These checks are "
                    "not supported as Cushion is not guaranteed to have enough info to process them properly.");
                return 0;
                break;
            }

            default:
            {
                struct token_list_item_t *macro_tokens = lex_replace_identifier_if_macro (
                    state, &current_token, LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_EVALUATION);

                if (!macro_tokens)
                {
                    context_execution_error (
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

        case TOKEN_TYPE_PUNCTUATOR:
            switch (current_token.punctuator_kind)
            {
            case PUNCTUATOR_KIND_LEFT_PARENTHESIS:
                // Encountered sub expression as argument.
                return lex_preprocessor_evaluate (state, LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_PARENTHESIS);

            case PUNCTUATOR_KIND_BITWISE_INVERSE:
                // Unary "~". Tail-recurse to make code easier.
                return ~lex_preprocessor_evaluate_argument (state, sub_expression_type);

            case PUNCTUATOR_KIND_PLUS:
                // Unary "+". Tail-recurse to make code easier.
                return lex_preprocessor_evaluate_argument (state, sub_expression_type);

            case PUNCTUATOR_KIND_MINUS:
                // Unary "-". Tail-recurse to make code easier.
                return -lex_preprocessor_evaluate_argument (state, sub_expression_type);

            case PUNCTUATOR_KIND_LOGICAL_NOT:
                // Unary "!". Tail-recurse to make code easier.
                return !lex_preprocessor_evaluate_argument (state, sub_expression_type);

            default:
                context_execution_error (
                    state->instance, &state->tokenization,
                    "Encountered unexpected punctuator while evaluating preprocessor conditional expression.");
                return 0;
            }

            // Should never happen unless memory is corrupted.
            assert (0);
            return 0;

        case TOKEN_TYPE_NUMBER_INTEGER:
            if (current_token.unsigned_number_value > LLONG_MAX)
            {
                context_execution_error (
                    state->instance, &state->tokenization,
                    "Encountered integer number which is higher than %lld (LLONG_MAX) while evaluating preprocessor "
                    "conditional expression, which is not supported by Cushion right now.",
                    (long long) LLONG_MAX);
                return 0;
            }

            return (long long) current_token.unsigned_number_value;

        case TOKEN_TYPE_NUMBER_FLOATING:
            context_execution_error (state->instance, &state->tokenization,
                                     "Encountered non-integer number while evaluating preprocessor conditional "
                                     "expression, which is not supported by specification.");
            return 0;

        case TOKEN_TYPE_CHARACTER_LITERAL:
            if (current_token.symbolic_literal.encoding != TOKEN_SUBSEQUENCE_ENCODING_ORDINARY)
            {
                context_execution_error (state->instance, &state->tokenization,
                                         "Encountered non-ordinary character literal while evaluating preprocessor "
                                         "conditional expression, which is currently not supported by Cushion.");
                return 0;
            }

            if (current_token.symbolic_literal.end - current_token.symbolic_literal.begin != 1u)
            {
                context_execution_error (
                    state->instance, &state->tokenization,
                    "Encountered non-single-character character literal while evaluating preprocessor conditional "
                    "expression, which is currently not supported by Cushion.");
                return 0;
            }

            return (long long) *current_token.symbolic_literal.begin;

        case TOKEN_TYPE_STRING_LITERAL:
            context_execution_error (state->instance, &state->tokenization,
                                     "Encountered string literal while evaluating preprocessor conditional expression, "
                                     "which is not supported by specification.");
            return 0;

        case TOKEN_TYPE_NEW_LINE:
            context_execution_error (
                state->instance, &state->tokenization,
                "Encountered end of line while expecting next argument for preprocessor conditional expression.");
            return 0;

        case TOKEN_TYPE_GLUE:
        case TOKEN_TYPE_COMMENT:
            // Never interested in it inside conditional, continue lexing.
            break;

        case TOKEN_TYPE_END_OF_FILE:
            context_execution_error (
                state->instance, &state->tokenization,
                "Encountered end of file while expecting next argument for preprocessor conditional expression.");
            return 0;

        case TOKEN_TYPE_OTHER:
            context_execution_error (
                state->instance, &state->tokenization,
                "Encountered unknown token expecting next argument for preprocessor conditional expression.");
            return 0;
        }
    }

    // Shouldn't exit that way unless error has occurred.
    assert (context_is_error_signaled (state->instance));
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

static long long lex_preprocessor_evaluate (struct lexer_file_state_t *state,
                                            enum lex_preprocessor_sub_expression_type_t sub_expression_type)
{
    struct token_t current_token;
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
        case TOKEN_TYPE_PUNCTUATOR:
        {
            enum lex_evaluate_operator_t next_operator = LEX_EVALUATE_OPERATOR_MULTIPLY;
            switch (current_token.punctuator_kind)
            {
            case PUNCTUATOR_KIND_RIGHT_PARENTHESIS:
                switch (sub_expression_type)
                {
                case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_ROOT:
                case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_POSITIVE:
                    context_execution_error (state->instance, &state->tokenization,
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

            case PUNCTUATOR_KIND_BITWISE_AND:
                next_operator = LEX_EVALUATE_OPERATOR_BITWISE_AND;
                break;

            case PUNCTUATOR_KIND_BITWISE_OR:
                next_operator = LEX_EVALUATE_OPERATOR_BITWISE_OR;
                break;

            case PUNCTUATOR_KIND_BITWISE_XOR:
                next_operator = LEX_EVALUATE_OPERATOR_BITWISE_XOR;
                break;

            case PUNCTUATOR_KIND_PLUS:
                next_operator = LEX_EVALUATE_OPERATOR_ADD;
                break;

            case PUNCTUATOR_KIND_MINUS:
                next_operator = LEX_EVALUATE_OPERATOR_SUBTRACT;
                break;

            case PUNCTUATOR_KIND_MULTIPLY:
                next_operator = LEX_EVALUATE_OPERATOR_MULTIPLY;
                break;

            case PUNCTUATOR_KIND_DIVIDE:
                next_operator = LEX_EVALUATE_OPERATOR_DIVIDE;
                break;

            case PUNCTUATOR_KIND_MODULO:
                next_operator = LEX_EVALUATE_OPERATOR_MODULO;
                break;

            case PUNCTUATOR_KIND_LOGICAL_AND:
                next_operator = LEX_EVALUATE_OPERATOR_LOGICAL_AND;
                break;

            case PUNCTUATOR_KIND_LOGICAL_OR:
                next_operator = LEX_EVALUATE_OPERATOR_LOGICAL_OR;
                break;

            case PUNCTUATOR_KIND_LOGICAL_LESS:
                next_operator = LEX_EVALUATE_OPERATOR_LESS;
                break;

            case PUNCTUATOR_KIND_LOGICAL_GREATER:
                next_operator = LEX_EVALUATE_OPERATOR_GREATER;
                break;

            case PUNCTUATOR_KIND_LOGICAL_LESS_OR_EQUAL:
                next_operator = LEX_EVALUATE_OPERATOR_LESS_OR_EQUAL;
                break;

            case PUNCTUATOR_KIND_LOGICAL_GREATER_OR_EQUAL:
                next_operator = LEX_EVALUATE_OPERATOR_GREATER_OR_EQUAL;
                break;

            case PUNCTUATOR_KIND_LOGICAL_EQUAL:
                next_operator = LEX_EVALUATE_OPERATOR_EQUAL;
                break;

            case PUNCTUATOR_KIND_LOGICAL_NOT_EQUAL:
                next_operator = LEX_EVALUATE_OPERATOR_NOT_EQUAL;
                break;

            case PUNCTUATOR_KIND_LEFT_SHIFT:
                next_operator = LEX_EVALUATE_OPERATOR_BITWISE_LEFT_SHIFT;
                break;

            case PUNCTUATOR_KIND_RIGHT_SHIFT:
                next_operator = LEX_EVALUATE_OPERATOR_BITWISE_RIGHT_SHIFT;
                break;

            case PUNCTUATOR_KIND_QUESTION_MARK:
                next_operator = LEX_EVALUATE_OPERATOR_TERNARY;
                break;

            case PUNCTUATOR_KIND_COMMA:
                next_operator = LEX_EVALUATE_OPERATOR_COMMA;
                break;

            case PUNCTUATOR_KIND_COLON:
                switch (sub_expression_type)
                {
                case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_ROOT:
                case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_PARENTHESIS:
                case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_NEGATIVE:
                    context_execution_error (state->instance, &state->tokenization,
                                             "Encountered unexpected \":\" in preprocessor expression evaluation.");
                    break;

                case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_POSITIVE:
                    goto finish_evaluation;
                    break;
                }

                break;

            default:
                context_execution_error (
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
                    new_item = stack_group_allocator_allocate (
                        &state->instance->allocator, sizeof (struct lex_evaluate_stack_item_t),
                        _Alignof (struct lexer_token_stack_item_t), ALLOCATION_CLASS_TRANSIENT);
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

        case TOKEN_TYPE_NEW_LINE:
            switch (sub_expression_type)
            {
            case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_ROOT:
            case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_NEGATIVE:
                goto finish_evaluation;

            case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_PARENTHESIS:
                context_execution_error (state->instance, &state->tokenization,
                                         "Expected \")\" but got new line in preprocessor expression evaluation.");

            case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_POSITIVE:
                context_execution_error (state->instance, &state->tokenization,
                                         "Expected \":\" but got new line in preprocessor expression evaluation.");
                break;
            }

            break;

        case TOKEN_TYPE_END_OF_FILE:
            switch (sub_expression_type)
            {
            case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_ROOT:
            case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_NEGATIVE:
                goto finish_evaluation;

            case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_PARENTHESIS:
                context_execution_error (state->instance, &state->tokenization,
                                         "Expected \")\" but got end of file in preprocessor expression evaluation.");

            case LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_TERNARY_POSITIVE:
                context_execution_error (state->instance, &state->tokenization,
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

                struct token_list_item_t push_first_token_item = {
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
            context_execution_error (state->instance, &state->tokenization,
                                     "Expected operator token after argument in preprocessor expression evaluation.");
            break;
        }
    }

    context_execution_error (state->instance, &state->tokenization,
                             "Failed to properly evaluate preprocessor constant expression.");
    return 0;
}

static void lex_update_tokenization_flags (struct lexer_file_state_t *state)
{
    state->tokenization.flags = RE2C_TOKENIZATION_FLAGS_NONE;
    if ((state->flags & LEX_FILE_FLAG_SCAN_ONLY) ||
        (state->conditional_inclusion_node &&
         state->conditional_inclusion_node->state == CONDITIONAL_INCLUSION_STATE_EXCLUDED))
    {
        state->tokenization.flags |= RE2C_TOKENIZATION_FLAGS_SKIP_REGULAR;
    }
}

static inline void lex_do_not_skip_regular (struct lexer_file_state_t *state)
{
    state->tokenization.flags &= ~RE2C_TOKENIZATION_FLAGS_SKIP_REGULAR;
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

static unsigned int lex_preprocessor_if_is_transitively_excluded_conditional (struct lexer_file_state_t *state)
{
    if (state->conditional_inclusion_node &&
        state->conditional_inclusion_node->state == CONDITIONAL_INCLUSION_STATE_EXCLUDED)
    {
        // Everything inside excluded scope is automatically excluded too.
        struct lexer_conditional_inclusion_node_t *node = stack_group_allocator_allocate (
            &state->instance->allocator, sizeof (struct lexer_conditional_inclusion_node_t),
            _Alignof (struct lexer_conditional_inclusion_node_t), ALLOCATION_CLASS_TRANSIENT);

        node->previous = state->conditional_inclusion_node;
        lexer_conditional_inclusion_node_init_state (node, CONDITIONAL_INCLUSION_STATE_EXCLUDED);
        node->line = state->tokenization.cursor_line;
        state->conditional_inclusion_node = node;
        return 1u;
    }

    return 0u;
}

static void lex_preprocessor_if (struct lexer_file_state_t *state, const struct token_t *preprocessor_token)
{
    if (lex_preprocessor_if_is_transitively_excluded_conditional (state))
    {
        return;
    }

    const unsigned int start_line = state->tokenization.cursor_line;
    lex_do_not_skip_regular (state);
    struct token_t current_token;
    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type == TOKEN_TYPE_IDENTIFIER &&
        current_token.identifier_kind == IDENTIFIER_KIND_CUSHION_PRESERVE)
    {
        // Special case: preserved conditional inclusion. Paste it to the output like normal code.
        struct lexer_conditional_inclusion_node_t *node = stack_group_allocator_allocate (
            &state->instance->allocator, sizeof (struct lexer_conditional_inclusion_node_t),
            _Alignof (struct lexer_conditional_inclusion_node_t), ALLOCATION_CLASS_TRANSIENT);

        node->previous = state->conditional_inclusion_node;
        lexer_conditional_inclusion_node_init_state (node, CONDITIONAL_INCLUSION_STATE_PRESERVED);
        node->line = state->tokenization.cursor_line;
        state->conditional_inclusion_node = node;

        if ((state->flags & LEX_FILE_FLAG_SCAN_ONLY) == 0u && start_line != state->tokenization.cursor_line)
        {
            lex_preprocessor_fixup_line (state);
        }

        lex_preprocessor_preserved_tail (state, preprocessor_token->type, NULL);
        lex_update_tokenization_flags (state);
        return;
    }

    // Safe to do this trick, as it is not deallocated until evaluation is shut down.
    // And token is being processed right when evaluation is started, therefore it is also safe to pass it like that.
    struct token_list_item_t push_first_token_item = {
        .next = NULL,
        .token = current_token,
    };

    lexer_file_state_push_tokens (state, &push_first_token_item, LEXER_TOKEN_STACK_ITEM_FLAG_NONE);
    const long long evaluation_result = lex_preprocessor_evaluate (state, LEX_PREPROCESSOR_SUB_EXPRESSION_TYPE_ROOT);
    LEX_WHEN_ERROR (return)

    struct lexer_conditional_inclusion_node_t *node = stack_group_allocator_allocate (
        &state->instance->allocator, sizeof (struct lexer_conditional_inclusion_node_t),
        _Alignof (struct lexer_conditional_inclusion_node_t), ALLOCATION_CLASS_TRANSIENT);

    node->previous = state->conditional_inclusion_node;
    lexer_conditional_inclusion_node_init_state (
        node, evaluation_result ? CONDITIONAL_INCLUSION_STATE_INCLUDED : CONDITIONAL_INCLUSION_STATE_EXCLUDED);
    node->line = start_line;
    state->conditional_inclusion_node = node;

    lex_preprocessor_fixup_line (state);
    lex_update_tokenization_flags (state);
}

static void lex_preprocessor_expect_new_line (struct lexer_file_state_t *state)
{
    struct token_t current_token;
    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    switch (current_token.type)
    {
    case TOKEN_TYPE_NEW_LINE:
        break;

    default:
        context_execution_error (state->instance, &state->tokenization,
                                 "Expected new line after preprocessor expression.");
        break;
    }
}

static void lex_preprocessor_ifdef (struct lexer_file_state_t *state, unsigned int reverse)
{
    if (lex_preprocessor_if_is_transitively_excluded_conditional (state))
    {
        return;
    }

    lex_do_not_skip_regular (state);
    unsigned int start_line = state->tokenization.cursor_line;
    struct token_t current_token;

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

    struct lexer_conditional_inclusion_node_t *node = stack_group_allocator_allocate (
        &state->instance->allocator, sizeof (struct lexer_conditional_inclusion_node_t),
        _Alignof (struct lexer_conditional_inclusion_node_t), ALLOCATION_CLASS_TRANSIENT);

    node->previous = state->conditional_inclusion_node;
    lexer_conditional_inclusion_node_init_state (
        node, check_result ? CONDITIONAL_INCLUSION_STATE_INCLUDED : CONDITIONAL_INCLUSION_STATE_EXCLUDED);
    node->line = start_line;
    state->conditional_inclusion_node = node;

    lex_preprocessor_fixup_line (state);
    lex_update_tokenization_flags (state);
}

static unsigned int lex_preprocessor_else_is_transitively_excluded_conditional (struct lexer_file_state_t *state)
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

static unsigned int lex_preprocessor_else_is_transitively_preserved (struct lexer_file_state_t *state,
                                                                     const struct token_t *preprocessor_token)
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
        context_execution_error (state->instance, &state->tokenization,                                                \
                                 "Found else family preprocessor without if family preprocessor before it.");          \
        return;                                                                                                        \
    }                                                                                                                  \
                                                                                                                       \
    if (state->conditional_inclusion_node->flags & CONDITIONAL_INCLUSION_FLAGS_HAD_PLAIN_ELSE)                         \
    {                                                                                                                  \
        context_execution_error (state->instance, &state->tokenization,                                                \
                                 "Found else family preprocessor in chain after unconditional #else.");                \
        return;                                                                                                        \
    }

static void lex_preprocessor_elif (struct lexer_file_state_t *state, const struct token_t *preprocessor_token)
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

static void lex_preprocessor_elifdef (struct lexer_file_state_t *state,
                                      const struct token_t *preprocessor_token,
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
    struct token_t current_token;

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

    lex_preprocessor_fixup_line (state);
    lex_update_tokenization_flags (state);
}

static void lex_preprocessor_else (struct lexer_file_state_t *state, const struct token_t *preprocessor_token)
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

static void lex_preprocessor_endif (struct lexer_file_state_t *state, const struct token_t *preprocessor_token)
{
    if (!state->conditional_inclusion_node)
    {
        context_execution_error (state->instance, &state->tokenization,
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

static void lex_file_from_handle (struct context_t *instance,
                                  FILE *input_file,
                                  const char *path,
                                  enum lex_file_flags_t flags);

static unsigned int lex_preprocessor_try_include (struct lexer_file_state_t *state,
                                                  const struct token_t *header_token,
                                                  struct include_node_t *include_node)
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

    // By default, we inherit flags and add more flags on top if needed.
    enum lex_file_flags_t flags = state->flags;

    switch (include_node->type)
    {
    case INCLUDE_TYPE_FULL:
        break;

    case INCLUDE_TYPE_SCAN:
        flags |= LEX_FILE_FLAG_SCAN_ONLY;
        break;
    }

    lex_file_from_handle (state->instance, input_file, state->path_buffer.data, flags);
    fclose (input_file);
    return 1u;
}

static void lex_preprocessor_include (struct lexer_file_state_t *state)
{
    lex_do_not_skip_regular (state);
    const unsigned int start_line = state->tokenization.cursor_line;
    struct token_t current_token;

    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    switch (current_token.type)
    {
    case TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
    case TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
        break;

    default:
        context_execution_error (state->instance, &state->tokenization, "Expected header path after #include.");
        return;
    }

    unsigned int include_happened = 0u;
    if (current_token.type == TOKEN_TYPE_PREPROCESSOR_HEADER_USER)
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

    struct include_node_t *node = state->instance->includes_first;
    while (node && !include_happened)
    {
        if (lex_preprocessor_try_include (state, &current_token, node))
        {
            include_happened = 1u;
            break;
        }

        node = node->next;
    }

    if (!include_happened && (state->flags & LEX_FILE_FLAG_SCAN_ONLY) == 0u)
    {
        // Include not found. Preserve it in code.
        context_output_null_terminated (state->instance, "#include ");
        context_output_sequence (state->instance, current_token.begin, current_token.end);
        context_output_null_terminated (state->instance, "\n");
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

static void lex_preprocessor_define (struct lexer_file_state_t *state)
{
    const unsigned int start_line = state->tokenization.cursor_line;
    lex_do_not_skip_regular (state);
    struct token_t current_token;
    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != TOKEN_TYPE_IDENTIFIER)
    {
        context_execution_error (state->instance, &state->tokenization, "Expected identifier after #define.");
        return;
    }

    switch (current_token.identifier_kind)
    {
    case IDENTIFIER_KIND_REGULAR:
        break;

    default:
        context_execution_error (state->instance, &state->tokenization,
                                 "Reserved word is used as macro name, which is not supported by Cushion.");
        return;
    }

    struct macro_node_t *node =
        stack_group_allocator_allocate (&state->instance->allocator, sizeof (struct macro_node_t),
                                        _Alignof (struct macro_node_t), ALLOCATION_CLASS_PERSISTENT);

    const unsigned int name_length = current_token.end - current_token.begin;
    char *macro_name = stack_group_allocator_allocate (&state->instance->allocator, name_length + 1u, _Alignof (char),
                                                       ALLOCATION_CLASS_PERSISTENT);

    memcpy (macro_name, current_token.begin, name_length);
    macro_name[name_length] = '\0';
    node->name = macro_name;
    node->flags = MACRO_FLAG_NONE;
    node->replacement_list_first = NULL;
    node->parameters_first = NULL;

    lexer_file_state_pop_token (state, &current_token);
    LEX_WHEN_ERROR (return)

    switch (current_token.type)
    {
    case TOKEN_TYPE_PUNCTUATOR:
    {
        node->flags |= MACRO_FLAG_FUNCTION;
        struct macro_parameter_node_t *parameters_last = NULL;
        unsigned int lexing_arguments = 1u;

        while (lexing_arguments)
        {
            lex_skip_glue_and_comments (state, &current_token);
            LEX_WHEN_ERROR (return)

            switch (current_token.type)
            {
            case TOKEN_TYPE_IDENTIFIER:
            {
                struct macro_parameter_node_t *parameter = stack_group_allocator_allocate (
                    &state->instance->allocator, sizeof (struct macro_parameter_node_t),
                    _Alignof (struct macro_parameter_node_t), ALLOCATION_CLASS_PERSISTENT);

                const unsigned int parameter_name_length = current_token.end - current_token.begin;
                char *parameter_name =
                    stack_group_allocator_allocate (&state->instance->allocator, parameter_name_length + 1u,
                                                    _Alignof (char), ALLOCATION_CLASS_PERSISTENT);

                memcpy (parameter_name, current_token.begin, parameter_name_length);
                parameter_name[parameter_name_length] = '\0';

                parameter->name = parameter_name;
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

                if (current_token.type != TOKEN_TYPE_PUNCTUATOR ||
                    current_token.punctuator_kind != PUNCTUATOR_KIND_COMMA)
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

        if (current_token.type == TOKEN_TYPE_PUNCTUATOR && current_token.punctuator_kind == PUNCTUATOR_KIND_TRIPLE_DOT)
        {
            node->flags |= MACRO_FLAG_VARIADIC_PARAMETERS;
            lex_skip_glue_and_comments (state, &current_token);
            LEX_WHEN_ERROR (return)
        }

        if (current_token.type != TOKEN_TYPE_PUNCTUATOR &&
            current_token.punctuator_kind == PUNCTUATOR_KIND_RIGHT_PARENTHESIS)
        {
            context_execution_error (state->instance, &state->tokenization,
                                     "Expected \")\" or \",\" while reading macro parameter name list.");
            return;
        }

        break;
    }

    case TOKEN_TYPE_NEW_LINE:
    case TOKEN_TYPE_END_OF_FILE:
        // No replacement list, go directly to registration.
        goto register_macro;

    case TOKEN_TYPE_GLUE:
    case TOKEN_TYPE_COMMENT:
        break;

    default:
        context_execution_error (state->instance, &state->tokenization,
                                 "Expected whitespaces, comments, \"(\", line end or file end after macro name.");
        return;
    }

    // Lex replacement list.
    enum lex_replacement_list_result_t lex_result =
        lex_replacement_list (state->instance, &state->tokenization, &node->replacement_list_first);

    switch (lex_result)
    {
    case LEX_REPLACEMENT_LIST_RESULT_REGULAR:
        // Fixup line after reading the macro.
        lex_preprocessor_fixup_line (state);
        break;

    case LEX_REPLACEMENT_LIST_RESULT_PRESERVED:
        if (start_line != state->tokenization.cursor_line)
        {
            // Fixup line if arguments and other things used line continuation.
            lex_preprocessor_fixup_line (state);
        }

        node->flags |= MACRO_FLAG_PRESERVED;
        lex_preprocessor_preserved_tail (state, TOKEN_TYPE_PREPROCESSOR_DEFINE, node);
        break;
    }

register_macro:
    // Register generated macro.
    context_macro_add (state->instance, node);
    lex_update_tokenization_flags (state);
}

static void lex_preprocessor_undef (struct lexer_file_state_t *state)
{
    unsigned int start_line = state->tokenization.cursor_line;
    lex_do_not_skip_regular (state);
    struct token_t current_token;
    lex_skip_glue_and_comments (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != TOKEN_TYPE_IDENTIFIER)
    {
        context_execution_error (state->instance, &state->tokenization, "Expected identifier after #undef.");
        return;
    }

    struct macro_node_t *node = context_macro_search (state->instance, current_token.begin, current_token.end);
    if (!node || (node->flags & MACRO_FLAG_PRESERVED))
    {
        // Preserve #undef as macro is either unknown or explicitly preserved.
        if ((state->flags & LEX_FILE_FLAG_SCAN_ONLY) == 0u)
        {
            if (start_line != state->tokenization.cursor_line)
            {
                lex_preprocessor_fixup_line (state);
                start_line = state->tokenization.cursor_line;
            }

            context_output_null_terminated (state->instance, "#undef ");
            context_output_sequence (state->instance, current_token.begin, current_token.end);
            context_output_null_terminated (state->instance, "\n");
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

    context_macro_remove (state->instance, current_token.begin, current_token.end);
    if ((state->flags & LEX_FILE_FLAG_SCAN_ONLY) == 0u)
    {
        context_output_null_terminated (state->instance, "\n");
    }

    lex_preprocessor_expect_new_line (state);
    if (start_line + 1u != state->tokenization.cursor_line)
    {
        lex_preprocessor_fixup_line (state);
    }

    lex_update_tokenization_flags (state);
}

static inline void lex_skip_glue_comments_new_line (struct lexer_file_state_t *state, struct token_t *current_token)
{
    while (lexer_file_state_should_continue (state))
    {
        lexer_file_state_pop_token (state, current_token);
        LEX_WHEN_ERROR (return)

        switch (current_token->type)
        {
        case TOKEN_TYPE_NEW_LINE:
        case TOKEN_TYPE_GLUE:
        case TOKEN_TYPE_COMMENT:
            break;

        default:
            return;
        }
    }
}

static void lex_code_macro_pragma (struct lexer_file_state_t *state)
{
    const unsigned int start_line = state->tokenization.cursor_line;
    struct token_t current_token;
    lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != TOKEN_TYPE_PUNCTUATOR &&
        current_token.punctuator_kind != PUNCTUATOR_KIND_LEFT_PARENTHESIS)
    {
        context_execution_error (state->instance, &state->tokenization, "Expected \"(\" after _Pragma.");
        return;
    }

    lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != TOKEN_TYPE_STRING_LITERAL)
    {
        context_execution_error (state->instance, &state->tokenization,
                                 "Expected string literal as argument of _Pragma.");
        return;
    }

    if (current_token.symbolic_literal.encoding != TOKEN_SUBSEQUENCE_ENCODING_ORDINARY)
    {
        context_execution_error (state->instance, &state->tokenization,
                                 "Only ordinary encoding supported for _Pragma argument.");
        return;
    }

    // Currently, we cannot check whether we're already at new line, so we're adding new line just in case.
    // New line addition (even if it was necessary) breaks line numbering for errors,
    // therefore we need to add line directive before it too.

    context_output_null_terminated (state->instance, "\n");
    context_output_line_marker (state->instance, start_line, state->tokenization.file_name);
    context_output_null_terminated (state->instance, "#pragma ");

    const char *output_begin_cursor = current_token.symbolic_literal.begin;
    const char *cursor = current_token.symbolic_literal.begin;

    while (cursor < current_token.symbolic_literal.end)
    {
        if (*cursor == '\\')
        {
            if (cursor + 1u >= current_token.symbolic_literal.end)
            {
                context_execution_error (state->instance, &state->tokenization,
                                         "Encountered \"\\\" as the last symbol of _Pragma argument literal.");
                return;
            }

            if (output_begin_cursor < cursor)
            {
                context_output_sequence (state->instance, output_begin_cursor, cursor);
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
                context_execution_error (state->instance, &state->tokenization,
                                         "Encountered unsupported escape \"\\%c\" in _Pragma argument literal, only "
                                         "\"\\\\\" and \"\\\"\" are supported in that context.");
                return;
            }
        }

        ++cursor;
    }

    if (output_begin_cursor < cursor)
    {
        context_output_sequence (state->instance, output_begin_cursor, cursor);
    }

    context_output_null_terminated (state->instance, "\n");
    lex_skip_glue_comments_new_line (state, &current_token);
    LEX_WHEN_ERROR (return)

    if (current_token.type != TOKEN_TYPE_PUNCTUATOR &&
        current_token.punctuator_kind != PUNCTUATOR_KIND_RIGHT_PARENTHESIS)
    {
        context_execution_error (state->instance, &state->tokenization, "Expected \")\" after _Pragma argument.");
        return;
    }

    // Always fixup line as pragma output results in new line.
    lex_preprocessor_fixup_line (state);
}

static void lex_code_identifier (struct lexer_file_state_t *state, struct token_t *current_token)
{
    // When we're doing scan only pass, we should not lex identifiers like this at all.
    assert (!(state->flags & LEX_FILE_FLAG_SCAN_ONLY));

    switch (current_token->identifier_kind)
    {
    case IDENTIFIER_KIND_CUSHION_PRESERVE:
        context_execution_error (state->instance, &state->tokenization,
                                 "Encountered cushion preserve keyword in unexpected context.");
        return;

    case IDENTIFIER_KIND_CUSHION_WRAPPED:
        context_execution_error (state->instance, &state->tokenization,
                                 "Encountered cushion wrapped keyword in unexpected context.");
        return;

    case IDENTIFIER_KIND_MACRO_PRAGMA:
        lex_code_macro_pragma (state);
        return;

    default:
        break;
    }

    if (current_token->identifier_kind == IDENTIFIER_KIND_CUSHION_PRESERVE)
    {
        context_execution_error (state->instance, &state->tokenization,
                                 "Encountered cushion preserve keyword in unexpected context.");
        return;
    }

    if (current_token->identifier_kind == IDENTIFIER_KIND_CUSHION_WRAPPED)
    {
        context_execution_error (state->instance, &state->tokenization,
                                 "Encountered cushion wrapped keyword in unexpected context.");
        return;
    }

    struct token_list_item_t *macro_tokens =
        lex_replace_identifier_if_macro (state, current_token, LEX_REPLACE_IDENTIFIER_IF_MACRO_CONTEXT_CODE);

    if (macro_tokens)
    {
        lexer_file_state_push_tokens (state, macro_tokens, LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT);
    }
    else
    {
        // Not a macro, just regular identifier.
        context_output_sequence (state->instance, current_token->begin, current_token->end);
    }
}

/// \brief Helper function for deciding whether we need to add separator between left token that is created through
///        macro replacement and arbitrary right token (can be from macro replacement too).
static inline unsigned int lex_is_separator_needed_for_token_pair (enum token_type_t left, enum token_type_t right)
{
    switch (right)
    {
    case TOKEN_TYPE_NEW_LINE:
    case TOKEN_TYPE_GLUE:
    case TOKEN_TYPE_COMMENT:
    case TOKEN_TYPE_END_OF_FILE:
        // No need for additional space here.
        return 0u;

    default:
        break;
    }

    switch (left)
    {
    case TOKEN_TYPE_COMMENT:
        // No need for additional space here.
        return 0u;

    default:
        break;
    }

    // Currently we put space in any case that we're not fully sure.
    // Better safe than sorry, even at expense of preprocessed replacement formatting.
    return 1u;
}

static void lex_root_file (struct context_t *instance, const char *path, enum lex_file_flags_t flags)
{
    FILE *input_file = fopen (path, "r");
    if (!input_file)
    {
        fprintf (stderr, "Failed to open input file \"%s\".\n", path);
        context_signal_error (instance);
        return;
    }

    lex_file_from_handle (instance, input_file, path, flags);
    fclose (input_file);
}

static void lex_file_from_handle (struct context_t *instance,
                                  FILE *input_file,
                                  const char *path,
                                  enum lex_file_flags_t flags)
{
    context_output_line_marker (instance, 1u, path);
    struct stack_group_allocator_transient_marker_t allocation_marker =
        stack_group_allocator_get_transient_marker (&instance->allocator);

    struct lexer_file_state_t *state =
        stack_group_allocator_allocate (&instance->allocator, sizeof (struct lexer_file_state_t),
                                        _Alignof (struct lexer_file_state_t), ALLOCATION_CLASS_TRANSIENT);

    state->lexing = 1u;
    state->flags = flags;
    state->instance = instance;
    state->token_stack_top = NULL;
    state->conditional_inclusion_node = NULL;
    state->file_name = path;

    re2c_state_init_for_file (&state->tokenization, path, input_file);
    lex_update_tokenization_flags (state);

    struct token_t current_token;
    current_token.type = TOKEN_TYPE_NEW_LINE; // Just stub value.
    unsigned int previous_token_line = state->tokenization.cursor_line;
    enum lexer_token_stack_item_flags_t current_token_flags = LEXER_TOKEN_STACK_ITEM_FLAG_NONE;

    while (lexer_file_state_should_continue (state))
    {
        const unsigned int previous_is_macro_replacement =
            current_token_flags & LEXER_TOKEN_STACK_ITEM_FLAG_MACRO_REPLACEMENT;
        enum token_type_t previous_type = current_token.type;

        lexer_file_state_pop_token (state, &current_token);
        if (context_is_error_signaled (instance))
        {
            break;
        }

        if (!(flags & LEX_FILE_FLAG_SCAN_ONLY) && previous_is_macro_replacement &&
            lex_is_separator_needed_for_token_pair (previous_type, current_token.type))
        {
            context_output_null_terminated (instance, " ");
        }

        // TODO: Currently conditional inclusion is calculated, but not really used.
        //       Fix it after everything is set inplace.
        //       Actually, it should be kind of automatically skipped through RE2C_TOKENIZATION_FLAGS_SKIP_REGULAR.
        //       So, maybe, no need to change anything?

        // TODO: Do not forget about RE2C_TOKENIZATION_FLAGS_SKIP_REGULAR while lexing preprocessor subsequences.
        //       Use lex_do_not_skip_regular.

        switch (current_token.type)
        {
        case TOKEN_TYPE_PREPROCESSOR_IF:
            lex_preprocessor_if (state, &current_token);
            break;

        case TOKEN_TYPE_PREPROCESSOR_IFDEF:
            lex_preprocessor_ifdef (state, 0u);
            break;

        case TOKEN_TYPE_PREPROCESSOR_IFNDEF:
            lex_preprocessor_ifdef (state, 1u);
            break;

        case TOKEN_TYPE_PREPROCESSOR_ELIF:
            lex_preprocessor_elif (state, &current_token);
            break;

        case TOKEN_TYPE_PREPROCESSOR_ELIFDEF:
            lex_preprocessor_elifdef (state, &current_token, 0u);
            break;

        case TOKEN_TYPE_PREPROCESSOR_ELIFNDEF:
            lex_preprocessor_elifdef (state, &current_token, 1u);
            break;

        case TOKEN_TYPE_PREPROCESSOR_ELSE:
            lex_preprocessor_else (state, &current_token);
            break;

        case TOKEN_TYPE_PREPROCESSOR_ENDIF:
            lex_preprocessor_endif (state, &current_token);
            break;

        case TOKEN_TYPE_PREPROCESSOR_INCLUDE:
            if (!state->conditional_inclusion_node ||
                state->conditional_inclusion_node->state != CONDITIONAL_INCLUSION_STATE_EXCLUDED)
            {
                lex_preprocessor_include (state);
            }

            break;

        case TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM:
        case TOKEN_TYPE_PREPROCESSOR_HEADER_USER:
            context_execution_error (instance, &state->tokenization,
                                     "Unexpected header path token (no prior #include).");
            break;

        case TOKEN_TYPE_PREPROCESSOR_DEFINE:
            if (!state->conditional_inclusion_node ||
                state->conditional_inclusion_node->state != CONDITIONAL_INCLUSION_STATE_EXCLUDED)
            {
                lex_preprocessor_define (state);
            }

            break;

        case TOKEN_TYPE_PREPROCESSOR_UNDEF:
            if (!state->conditional_inclusion_node ||
                state->conditional_inclusion_node->state != CONDITIONAL_INCLUSION_STATE_EXCLUDED)
            {
                lex_preprocessor_undef (state);
            }

            break;

        case TOKEN_TYPE_PREPROCESSOR_LINE:
            if (!state->conditional_inclusion_node ||
                state->conditional_inclusion_node->state != CONDITIONAL_INCLUSION_STATE_EXCLUDED)
            {
                // TODO: Implement. Proper line directive processing is necessary.
            }

            break;

        case TOKEN_TYPE_PREPROCESSOR_PRAGMA:
            if (!state->conditional_inclusion_node ||
                state->conditional_inclusion_node->state != CONDITIONAL_INCLUSION_STATE_EXCLUDED)
            {
                // TODO: Implement. Check if next identifier is once and process it. Otherwise, preserve tail.
            }

            break;

        case TOKEN_TYPE_IDENTIFIER:
            lex_code_identifier (state, &current_token);
            break;

        case TOKEN_TYPE_PUNCTUATOR:
        case TOKEN_TYPE_NUMBER_INTEGER:
        case TOKEN_TYPE_NUMBER_FLOATING:
        case TOKEN_TYPE_CHARACTER_LITERAL:
        case TOKEN_TYPE_STRING_LITERAL:
        case TOKEN_TYPE_NEW_LINE:
        case TOKEN_TYPE_GLUE:
        case TOKEN_TYPE_OTHER:
            context_output_sequence (instance, current_token.begin, current_token.end);
            break;

        case TOKEN_TYPE_COMMENT:
            // If it was a multiline comment, fixup line number.
            if (previous_token_line != state->tokenization.cursor_line)
            {
                context_output_null_terminated (state->instance, "\n");
                lex_preprocessor_fixup_line (state);
            }
            else
            {
                // Just a space to make sure that tokens are not merged by mistake.
                context_output_null_terminated (state->instance, " ");
            }

            break;

        case TOKEN_TYPE_END_OF_FILE:
            if (state->conditional_inclusion_node)
            {
                context_execution_error (
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
    stack_group_allocator_reset_transient (&instance->allocator, allocation_marker);
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
    struct input_node_t *node =
        stack_group_allocator_allocate (&instance->allocator, sizeof (struct input_node_t),
                                        _Alignof (struct input_node_t), ALLOCATION_CLASS_PERSISTENT);

    node->path = context_copy_string_inside (instance, path, ALLOCATION_CLASS_PERSISTENT);
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
    instance->output_path = context_copy_string_inside (instance, path, ALLOCATION_CLASS_PERSISTENT);
}

void cushion_context_configure_cmake_depfile (cushion_context_t context, const char *path)
{
    struct context_t *instance = context.value;
    instance->cmake_depfile_path = context_copy_string_inside (instance, path, ALLOCATION_CLASS_PERSISTENT);
}

void cushion_context_configure_define (cushion_context_t context, const char *name, const char *value)
{
    struct context_t *instance = context.value;
    struct macro_node_t *new_node =
        stack_group_allocator_allocate (&instance->allocator, sizeof (struct macro_node_t),
                                        _Alignof (struct macro_node_t), ALLOCATION_CLASS_PERSISTENT);

    new_node->name = name;
    new_node->flags = MACRO_FLAG_NONE;
    new_node->value = value;
    new_node->parameters_first = NULL;

    new_node->next = instance->unresolved_macros_first;
    instance->unresolved_macros_first = new_node;
}

void cushion_context_configure_include_full (cushion_context_t context, const char *path)
{
    struct context_t *instance = context.value;
    struct include_node_t *node =
        stack_group_allocator_allocate (&instance->allocator, sizeof (struct include_node_t),
                                        _Alignof (struct include_node_t), ALLOCATION_CLASS_PERSISTENT);

    node->type = INCLUDE_TYPE_FULL;
    node->path = context_copy_string_inside (instance, path, ALLOCATION_CLASS_PERSISTENT);
    context_includes_add (instance, node);
}

void cushion_context_configure_include_scan_only (cushion_context_t context, const char *path)
{
    struct context_t *instance = context.value;
    struct include_node_t *node =
        stack_group_allocator_allocate (&instance->allocator, sizeof (struct include_node_t),
                                        _Alignof (struct include_node_t), ALLOCATION_CLASS_PERSISTENT);

    node->type = INCLUDE_TYPE_SCAN;
    node->path = context_copy_string_inside (instance, path, ALLOCATION_CLASS_PERSISTENT);
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
        fprintf (stderr, "Received feature selection in clean build (Cushion was built without extensions).\n");
        result = CUSHION_RESULT_UNSUPPORTED_FEATURES;
    }
#endif

    if (result == CUSHION_RESULT_OK)
    {
        struct macro_node_t *macro_node = instance->unresolved_macros_first;
        instance->unresolved_macros_first = NULL;

        while (macro_node)
        {
            struct macro_node_t *next = macro_node->next;
            struct re2c_state_t tokenization_state;
            re2c_state_init_for_argument_string (&tokenization_state, macro_node->value);

            enum lex_replacement_list_result_t lex_result =
                lex_replacement_list (instance, &tokenization_state, &macro_node->replacement_list_first);

            if (context_is_error_signaled (instance))
            {
                fprintf (stderr, "Failed to lex object macro \"%s\" from arguments.\n", macro_node->name);
                result = CUSHION_RESULT_FAILED_TO_LEX_CONFIGURED_DEFINES;
            }
            else if (tokenization_state.cursor != tokenization_state.limit)
            {
                fprintf (stderr,
                         "Object macro \"%s\" from arguments cannot be fully lexed: argument string contains new "
                         "line in the middle. It is a warning for most compilers (they just ignore everything "
                         "after new line usually), but Cushion thinks that it is an obvious error as arguments "
                         "must be properly formatted.\n",
                         macro_node->name);
                result = CUSHION_RESULT_FAILED_TO_LEX_CONFIGURED_DEFINES;
            }
            else
            {
                switch (lex_result)
                {
                case LEX_REPLACEMENT_LIST_RESULT_REGULAR:
                    context_macro_add (instance, macro_node);
                    break;

                case LEX_REPLACEMENT_LIST_RESULT_PRESERVED:
                    fprintf (stderr,
                             "Encountered __CUSHION_PRESERVE__ while lexing macro \"%s\" from arguments, which is not "
                             "supported.\n",
                             macro_node->name);
                    result = CUSHION_RESULT_FAILED_TO_LEX_CONFIGURED_DEFINES;
                    break;
                }
            }

            macro_node = next;
        }
    }

    if (result == CUSHION_RESULT_OK)
    {
        instance->output = fopen (instance->output_path, "w");
        if (instance->output)
        {
            struct input_node_t *input_node = instance->inputs_first;
            while (input_node)
            {
                lex_root_file (instance, input_node->path, LEX_FILE_FLAG_NONE);
                if (context_is_error_signaled (instance))
                {
                    result = CUSHION_RESULT_LEX_FAILED;
                    break;
                }

                input_node = input_node->next;
            }

            fclose (instance->output);
        }
        else
        {
            fprintf (stderr, "Failed to open output file \"%s\".\n", instance->output_path);
            result = CUSHION_RESULT_FAILED_TO_OPEN_OUTPUT;
        }
    }

    // Reset all the configuration.
    context_clean_configuration (instance);

    // Shrink and reset memory usage.
    stack_group_allocator_shrink (&instance->allocator);
    stack_group_allocator_reset_all (&instance->allocator);

    return result;
}

void cushion_context_destroy (cushion_context_t context)
{
    struct context_t *instance = context.value;
    stack_group_allocator_shutdown (&instance->allocator);
    free (instance);
}
