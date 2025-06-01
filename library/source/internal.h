#pragma once

#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cushion.h>

/// \brief This file is an internal include that is shared between compilation objects.
/// \details Instance, tokenization and lexing share a lot of commonly visible data structures, therefore it is
///          much more convenient to keep all of them in one file and only write implementation-specific things
///          in compilation objects.

// We need to get absolute path for proper line directives and proper pragma once.
#if defined(_WIN32) || defined(_WIN64)
#    define CUSHION_PATH_MAX 4096
#    include <windows.h>
#    define CUSHION_GET_ABSOLUTE_PATH_WINDOWS
#elif defined(__unix__) || defined(__unix) || (defined(__APPLE__) && defined(__MACH__))
#    define CUSHION_PATH_MAX PATH_MAX
#    define CUSHION_GET_ABSOLUTE_PATH_UNIX
#else
#    error "Cushion has no implementation for getting absolute path for #pragma once on this OS."
#endif

CUSHION_HEADER_BEGIN

// Common generic utility functions.

static inline uintptr_t cushion_apply_alignment (uintptr_t address_or_size, uintptr_t alignment)
{
    const uintptr_t modulo = address_or_size % alignment;
    if (modulo != 0u)
    {
        address_or_size += alignment - modulo;
    }

    return address_or_size;
}

static inline uintptr_t cushion_apply_alignment_reversed (uintptr_t address_or_size, uintptr_t alignment)
{
    return address_or_size - address_or_size % alignment;
}

static inline unsigned int cushion_hash_djb2_char_sequence (const char *begin, const char *end)
{
    unsigned int hash_value = 5381u;
    while (begin != end)
    {
        hash_value = (hash_value << 5u) + hash_value + (unsigned char) *begin;
        ++begin;
    }

    return hash_value;
}

static inline unsigned int cushion_hash_djb2_null_terminated (const char *string)
{
    return cushion_hash_djb2_char_sequence (string, string + strlen (string));
}

enum cushion_internal_result_t
{
    CUSHION_INTERNAL_RESULT_OK = 0u,
    CUSHION_INTERNAL_RESULT_FAILED = 1u,
};

// Memory management section: common utility for memory management.

/// \brief We use double stack allocator for everything.
struct cushion_allocator_t
{
    struct cushion_allocator_page_t *first_page;
    struct cushion_allocator_page_t *current_page;
};

struct cushion_allocator_page_t
{
    struct cushion_allocator_page_t *next;
    void *top_transient;
    void *bottom_persistent;
    uint8_t data[CUSHION_ALLOCATOR_PAGE_SIZE];
};

enum cushion_allocation_class_t
{
    /// \brief Allocated in some scope and will be deallocated when exiting the scope.
    CUSHION_ALLOCATION_CLASS_TRANSIENT = 0u,

    /// \brief Persistent allocator for the whole execution.
    CUSHION_ALLOCATION_CLASS_PERSISTENT,
};

struct cushion_allocator_transient_marker_t
{
    struct cushion_allocator_page_t *page;
    void *top_transient;
};

void cushion_allocator_init (struct cushion_allocator_t *instance);

void *cushion_allocator_allocate (struct cushion_allocator_t *allocator,
                                  uintptr_t size,
                                  uintptr_t alignment,
                                  enum cushion_allocation_class_t class);

struct cushion_allocator_transient_marker_t cushion_allocator_get_transient_marker (
    struct cushion_allocator_t *allocator);

void cushion_allocator_reset_transient (struct cushion_allocator_t *allocator,
                                        struct cushion_allocator_transient_marker_t transient_marker);

void cushion_allocator_reset_all (struct cushion_allocator_t *allocator);

void cushion_allocator_shrink (struct cushion_allocator_t *allocator);

void cushion_allocator_shutdown (struct cushion_allocator_t *allocator);

// Context instance generic part.

enum cushion_instance_state_flag_t
{
    CUSHION_INSTANCE_STATE_FLAG_EXECUTION = 1u << 0u,
    CUSHION_INSTANCE_STATE_FLAG_ERRED = 1u << 1u,
};

struct cushion_instance_t
{
    enum cushion_instance_state_flag_t state_flags;
    unsigned int features;
    unsigned int options;

    struct cushion_include_node_t *includes_first;
    struct cushion_include_node_t *includes_last;

    struct cushion_macro_node_t *macro_buckets[CUSHION_MACRO_BUCKETS];
    struct cushion_pragma_once_file_node_t *pragma_once_buckets[CUSHION_PRAGMA_ONCE_BUCKETS];

    struct cushion_allocator_t allocator;

    struct cushion_input_node_t *inputs_first;
    struct cushion_input_node_t *inputs_last;

    FILE *output;
    FILE *cmake_depfile_output;

    char *output_path;
    char *cmake_depfile_path;

    struct cushion_macro_node_t *unresolved_macros_first;
};

enum cushion_include_type_t
{
    INCLUDE_TYPE_FULL = 0u,
    INCLUDE_TYPE_SCAN,
};

struct cushion_include_node_t
{
    struct cushion_include_node_t *next;
    enum cushion_include_type_t type;
    char *path;
};

struct cushion_input_node_t
{
    struct cushion_input_node_t *next;
    char *path;
};

enum cushion_macro_flags_t
{
    CUSHION_MACRO_FLAG_NONE = 0u,

    /// \brief It is a function-like macro. Even macro without arguments can be function-like.
    CUSHION_MACRO_FLAG_FUNCTION = 1u << 0u,

    CUSHION_MACRO_FLAG_VARIADIC_PARAMETERS = 1u << 1u,
    CUSHION_MACRO_FLAG_PRESERVED = 1u << 2u,
};

struct cushion_macro_parameter_node_t
{
    struct cushion_macro_parameter_node_t *next;
    unsigned int name_hash;
    const char *name;
};

struct cushion_macro_node_t
{
    struct cushion_macro_node_t *next;
    unsigned int name_hash;
    const char *name;
    enum cushion_macro_flags_t flags;

    union
    {
        /// \brief String value when we're gathering macro values from configuration.
        const char *value;

        /// \brief Actual replacement list tokens. Produced when execution has started.
        struct cushion_token_list_item_t *replacement_list_first;
    };

    struct cushion_macro_parameter_node_t *parameters_first;
};

struct cushion_pragma_once_file_node_t
{
    struct cushion_pragma_once_file_node_t *next;
    unsigned int path_hash;
    const char *path;
};

void cushion_instance_clean_configuration (struct cushion_instance_t *instance);

static inline char *cushion_instance_copy_char_sequence_inside (struct cushion_instance_t *instance,
                                                                const char *begin,
                                                                const char *end,
                                                                enum cushion_allocation_class_t allocation_class)
{
    const size_t length = end - begin;
    char *copied = cushion_allocator_allocate (&instance->allocator, length + 1u, _Alignof (char), allocation_class);
    memcpy (copied, begin, length);
    copied[length] = '\0';
    return copied;
}

static inline char *cushion_instance_copy_null_terminated_inside (struct cushion_instance_t *instance,
                                                                  const char *string,
                                                                  enum cushion_allocation_class_t allocation_class)
{
    return cushion_instance_copy_char_sequence_inside (instance, string, string + strlen (string), allocation_class);
}

void cushion_instance_includes_add (struct cushion_instance_t *instance, struct cushion_include_node_t *node);

struct cushion_macro_node_t *cushion_instance_macro_search (struct cushion_instance_t *instance,
                                                            const char *name_begin,
                                                            const char *name_end);

struct cushion_tokenization_state_t;

void cushion_instance_execution_error (struct cushion_instance_t *instance,
                                       struct cushion_tokenization_state_t *state,
                                       const char *format,
                                       ...);

static inline void cushion_instance_signal_error (struct cushion_instance_t *instance)
{
    instance->state_flags |= CUSHION_INSTANCE_STATE_FLAG_ERRED;
}

static inline unsigned int cushion_instance_is_error_signaled (struct cushion_instance_t *instance)
{
    return instance->state_flags & CUSHION_INSTANCE_STATE_FLAG_ERRED;
}

static inline unsigned int cushion_instance_has_option (struct cushion_instance_t *instance,
                                                        enum cushion_option_t option)
{
    return instance->options & (1u << option);
}

void cushion_instance_macro_add (struct cushion_instance_t *instance,
                                 struct cushion_macro_node_t *node,
                                 struct cushion_tokenization_state_t *tokenization_state_for_logging);

void cushion_instance_macro_remove (struct cushion_instance_t *instance, const char *name_begin, const char *name_end);

void cushion_instance_output_sequence (struct cushion_instance_t *instance, const char *begin, const char *end);

void cushion_instance_output_null_terminated (struct cushion_instance_t *instance, const char *string);

void cushion_instance_output_line_marker (struct cushion_instance_t *instance, unsigned int line, const char *path);

// Tokenization section: structs and functions to properly setup for tokenization.

enum cushion_tokenization_mode_t
{
    CUSHION_TOKENIZATION_MODE_REGULAR = 0u,

    /// \brief By standard, preprocessor tokens always start right after new line and cannot be encountered otherwise.
    CUSHION_TOKENIZATION_MODE_NEW_LINE,

    /// \brief Include needs separate mode as header path tokens can only be found there.
    CUSHION_TOKENIZATION_MODE_INCLUDE,

    /// \brief Line needs separate mode as by standard even numbers that start from 0 must be parsed as decimals here.
    CUSHION_TOKENIZATION_MODE_LINE,
};

enum cushion_tokenization_flags_t
{
    CUSHION_TOKENIZATION_FLAGS_NONE = 0u,

    /// \brief Simplified mode that skips any tokens that are not preprocessor directives.
    /// \details Useful for blazing through scan only headers and conditionally excluded source parts.
    ///          Should be disabled when tokenizing conditional expressions as their expressions are regular tokens.
    CUSHION_TOKENIZATION_FLAGS_SKIP_REGULAR = 1u << 0u,
};

struct cushion_tokenization_state_t
{
    /// \brief Tokenization file name that can be changed by line directives.
    const char *file_name;

    enum cushion_tokenization_mode_t state;
    enum cushion_tokenization_flags_t flags;

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

    /// \details Separate allocation is needed, unfortunately,
    ///          because size of re2c tags would only be known after re2c generator pass.
    struct re2c_tags_t *tags;

    FILE *input_file_optional;
    char input_buffer[CUSHION_INPUT_BUFFER_SIZE];
};

void cushion_tokenization_state_init_for_argument_string (struct cushion_tokenization_state_t *state,
                                                          const char *string,
                                                          struct cushion_allocator_t *allocator,
                                                          enum cushion_allocation_class_t allocation_class);

void cushion_tokenization_state_init_for_file (struct cushion_tokenization_state_t *state,
                                               const char *path,
                                               FILE *file,
                                               struct cushion_allocator_t *allocator,
                                               enum cushion_allocation_class_t allocation_class);

enum cushion_token_type_t
{
    CUSHION_TOKEN_TYPE_PREPROCESSOR_IF = 0u,
    CUSHION_TOKEN_TYPE_PREPROCESSOR_IFDEF,
    CUSHION_TOKEN_TYPE_PREPROCESSOR_IFNDEF,
    CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIF,
    CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFDEF,
    CUSHION_TOKEN_TYPE_PREPROCESSOR_ELIFNDEF,
    CUSHION_TOKEN_TYPE_PREPROCESSOR_ELSE,
    CUSHION_TOKEN_TYPE_PREPROCESSOR_ENDIF,
    CUSHION_TOKEN_TYPE_PREPROCESSOR_INCLUDE,
    CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_SYSTEM,
    CUSHION_TOKEN_TYPE_PREPROCESSOR_HEADER_USER,
    CUSHION_TOKEN_TYPE_PREPROCESSOR_DEFINE,
    CUSHION_TOKEN_TYPE_PREPROCESSOR_UNDEF,
    CUSHION_TOKEN_TYPE_PREPROCESSOR_LINE,
    CUSHION_TOKEN_TYPE_PREPROCESSOR_PRAGMA,

    CUSHION_TOKEN_TYPE_IDENTIFIER,
    CUSHION_TOKEN_TYPE_PUNCTUATOR,

    // Only integer numbers can participate in preprocessor conditional expressions.
    // Therefore, we calculate integer values, but pass non-integer ones just as strings.

    CUSHION_TOKEN_TYPE_NUMBER_INTEGER,
    CUSHION_TOKEN_TYPE_NUMBER_FLOATING,

    CUSHION_TOKEN_TYPE_CHARACTER_LITERAL,
    CUSHION_TOKEN_TYPE_STRING_LITERAL,

    CUSHION_TOKEN_TYPE_NEW_LINE,

    /// \brief Whitespaces as a glue.
    /// \brief Glue is saved as token so lexer can handle it in output generation logic without relying on tokenizer
    ///        to handle its output.
    CUSHION_TOKEN_TYPE_GLUE,

    /// \brief Actual comments.
    /// \details We express comments as tokens because it makes it possible to handle them on lexer level along with
    ///          all other output generation. Also, it makes it possible to keep them inside macros if requested.
    CUSHION_TOKEN_TYPE_COMMENT,

    /// \brief Special token for processing end of file that might break grammar in lexer.
    CUSHION_TOKEN_TYPE_END_OF_FILE,

    /// \brief Everything that is not one of the above, goes into here.
    /// \details As in standard draft, "each non-white-space character that cannot be one of the above" form tokens.
    CUSHION_TOKEN_TYPE_OTHER,
};

/// \brief Some preprocessing identifiers like __VA_ARGS__ or Cushion control identifiers need additional care.
/// \details When extensions are enabled, some keywords like return also need additional care.
enum cushion_identifier_kind_t
{
    CUSHION_IDENTIFIER_KIND_REGULAR = 0u,

    CUSHION_IDENTIFIER_KIND_VA_ARGS,
    CUSHION_IDENTIFIER_KIND_VA_OPT,

    CUSHION_IDENTIFIER_KIND_FILE,
    CUSHION_IDENTIFIER_KIND_LINE,

    CUSHION_IDENTIFIER_KIND_CUSHION_PRESERVE,

    CUSHION_IDENTIFIER_KIND_CUSHION_DEFER,
    CUSHION_IDENTIFIER_KIND_CUSHION_WRAPPED,
    CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR,
    CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_PUSH,
    CUSHION_IDENTIFIER_KIND_CUSHION_STATEMENT_ACCUMULATOR_REFERENCE,

    CUSHION_IDENTIFIER_KIND_DEFINED,
    CUSHION_IDENTIFIER_KIND_HAS_INCLUDE,
    CUSHION_IDENTIFIER_KIND_HAS_EMBED,
    CUSHION_IDENTIFIER_KIND_HAS_C_ATTRIBUTE,
    CUSHION_IDENTIFIER_KIND_MACRO_PRAGMA,

    CUSHION_IDENTIFIER_KIND_IF,
    CUSHION_IDENTIFIER_KIND_FOR,
    CUSHION_IDENTIFIER_KIND_WHILE,
    CUSHION_IDENTIFIER_KIND_DO,

    CUSHION_IDENTIFIER_KIND_RETURN,
    CUSHION_IDENTIFIER_KIND_BREAK,
    CUSHION_IDENTIFIER_KIND_CONTINUE,

    // TODO: Not sure what to do with goto for defer feature.
    //       It is rather difficult to properly generate defer when goto is used, especially when label is not yet
    //       declared. But it might be an interesting though experiment: thinking how it could be implemented.
    CUSHION_IDENTIFIER_KIND_GOTO,
};

enum cushion_punctuator_kind_t
{
    CUSHION_PUNCTUATOR_KIND_LEFT_SQUARE_BRACKET = 0u, // [
    CUSHION_PUNCTUATOR_KIND_RIGHT_SQUARE_BRACKET,     // ]

    CUSHION_PUNCTUATOR_KIND_LEFT_PARENTHESIS,  // (
    CUSHION_PUNCTUATOR_KIND_RIGHT_PARENTHESIS, // )

    CUSHION_PUNCTUATOR_KIND_LEFT_CURLY_BRACE,  // {
    CUSHION_PUNCTUATOR_KIND_RIGHT_CURLY_BRACE, // }

    CUSHION_PUNCTUATOR_KIND_MEMBER_ACCESS,  // .
    CUSHION_PUNCTUATOR_KIND_POINTER_ACCESS, // ->

    CUSHION_PUNCTUATOR_KIND_INCREMENT, // ++
    CUSHION_PUNCTUATOR_KIND_DECREMENT, // --

    CUSHION_PUNCTUATOR_KIND_BITWISE_AND,     // &
    CUSHION_PUNCTUATOR_KIND_BITWISE_OR,      // |
    CUSHION_PUNCTUATOR_KIND_BITWISE_XOR,     // ^
    CUSHION_PUNCTUATOR_KIND_BITWISE_INVERSE, // ~

    CUSHION_PUNCTUATOR_KIND_PLUS,     // +
    CUSHION_PUNCTUATOR_KIND_MINUS,    // -
    CUSHION_PUNCTUATOR_KIND_MULTIPLY, // *
    CUSHION_PUNCTUATOR_KIND_DIVIDE,   // /
    CUSHION_PUNCTUATOR_KIND_MODULO,   // %

    CUSHION_PUNCTUATOR_KIND_LOGICAL_NOT,              // !
    CUSHION_PUNCTUATOR_KIND_LOGICAL_AND,              // &&
    CUSHION_PUNCTUATOR_KIND_LOGICAL_OR,               // ||
    CUSHION_PUNCTUATOR_KIND_LOGICAL_LESS,             // <
    CUSHION_PUNCTUATOR_KIND_LOGICAL_GREATER,          // >
    CUSHION_PUNCTUATOR_KIND_LOGICAL_LESS_OR_EQUAL,    // <=
    CUSHION_PUNCTUATOR_KIND_LOGICAL_GREATER_OR_EQUAL, // >=
    CUSHION_PUNCTUATOR_KIND_LOGICAL_EQUAL,            // ==
    CUSHION_PUNCTUATOR_KIND_LOGICAL_NOT_EQUAL,        // !=

    CUSHION_PUNCTUATOR_KIND_LEFT_SHIFT,  // <<
    CUSHION_PUNCTUATOR_KIND_RIGHT_SHIFT, // >>

    CUSHION_PUNCTUATOR_KIND_QUESTION_MARK, // ?
    CUSHION_PUNCTUATOR_KIND_COLON,         // :
    CUSHION_PUNCTUATOR_KIND_DOUBLE_COLON,  // ::
    CUSHION_PUNCTUATOR_KIND_SEMICOLON,     // ;
    CUSHION_PUNCTUATOR_KIND_COMMA,         // ,
    CUSHION_PUNCTUATOR_KIND_TRIPLE_DOT,    // ...
    CUSHION_PUNCTUATOR_KIND_HASH,          // #
    CUSHION_PUNCTUATOR_KIND_DOUBLE_HASH,   // ##

    CUSHION_PUNCTUATOR_KIND_ASSIGN,             // =
    CUSHION_PUNCTUATOR_KIND_PLUS_ASSIGN,        // +=
    CUSHION_PUNCTUATOR_KIND_MINUS_ASSIGN,       // -=
    CUSHION_PUNCTUATOR_KIND_MULTIPLY_ASSIGN,    // *=
    CUSHION_PUNCTUATOR_KIND_DIVIDE_ASSIGN,      // /=
    CUSHION_PUNCTUATOR_KIND_LEFT_SHIFT_ASSIGN,  // <<=
    CUSHION_PUNCTUATOR_KIND_RIGHT_SHIFT_ASSIGN, // >>=
    CUSHION_PUNCTUATOR_KIND_BITWISE_AND_ASSIGN, // &=
    CUSHION_PUNCTUATOR_KIND_BITWISE_OR_ASSIGN,  // |=
    CUSHION_PUNCTUATOR_KIND_BITWISE_XOR_ASSIGN, // ^=
};

struct cushion_token_subsequence_t
{
    const char *begin;
    const char *end;
};

enum cushion_token_subsequence_encoding_t
{
    CUSHION_TOKEN_SUBSEQUENCE_ENCODING_ORDINARY = 0u,
    CUSHION_TOKEN_SUBSEQUENCE_ENCODING_UTF8,
    CUSHION_TOKEN_SUBSEQUENCE_ENCODING_UTF16,
    CUSHION_TOKEN_SUBSEQUENCE_ENCODING_UTF32,
    CUSHION_TOKEN_SUBSEQUENCE_ENCODING_WIDE,
};

struct cushion_encoded_token_subsequence_t
{
    enum cushion_token_subsequence_encoding_t encoding;
    const char *begin;
    const char *end;
};

struct cushion_token_t
{
    enum cushion_token_type_t type;
    const char *begin;
    const char *end;

    union
    {
        struct cushion_token_subsequence_t header_path;
        enum cushion_identifier_kind_t identifier_kind;
        enum cushion_punctuator_kind_t punctuator_kind;
        unsigned long long unsigned_number_value;
        struct cushion_encoded_token_subsequence_t symbolic_literal;
    };
};

struct cushion_token_list_item_t
{
    struct cushion_token_list_item_t *next;
    struct cushion_token_t token;
};

struct cushion_token_list_item_t *cushion_save_token_to_memory (struct cushion_instance_t *instance,
                                                                const struct cushion_token_t *token,
                                                                enum cushion_allocation_class_t allocation_class);

void cushion_tokenization_next_token (struct cushion_instance_t *instance,
                                      struct cushion_tokenization_state_t *state,
                                      struct cushion_token_t *output);

// Lexing section: structs and functions to properly setup for lexing.

enum cushion_lex_replacement_list_result_t
{
    CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR = 0u,
    CUSHION_LEX_REPLACEMENT_LIST_RESULT_PRESERVED,
};

enum cushion_lex_replacement_list_result_t cushion_lex_replacement_list (
    struct cushion_instance_t *instance,
    struct cushion_tokenization_state_t *tokenization_state,
    struct cushion_token_list_item_t **token_list_output);

enum cushion_lex_file_flags_t
{
    CUSHION_LEX_FILE_FLAG_NONE = 0u,
    CUSHION_LEX_FILE_FLAG_SCAN_ONLY = 1u << 0u,
    CUSHION_LEX_FILE_FLAG_PROCESSED_PRAGMA_ONCE = 1u << 1u,
};

struct cushion_lexer_path_buffer_t
{
    size_t size;
    char data[CUSHION_PATH_BUFFER_SIZE];
};

struct cushion_lexer_file_state_t
{
    unsigned int lexing;
    enum cushion_lex_file_flags_t flags;

    struct cushion_instance_t *instance;
    struct lexer_token_stack_item_t *token_stack_top;
    struct lexer_conditional_inclusion_node_t *conditional_inclusion_node;

    /// \brief File name in lexer always points to the actual file using absolute path.
    char file_name[CUSHION_PATH_MAX];

    struct cushion_tokenization_state_t tokenization;

    struct cushion_lexer_path_buffer_t path_buffer;
};

void cushion_lex_root_file (struct cushion_instance_t *instance, const char *path, enum cushion_lex_file_flags_t flags);

void cushion_lex_file_from_handle (struct cushion_instance_t *instance,
                                   FILE *input_file,
                                   const char *path,
                                   enum cushion_lex_file_flags_t flags);

CUSHION_HEADER_END
