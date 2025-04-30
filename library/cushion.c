#include <assert.h>
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

    char *output_path;
    char *cmake_depfile_path;

    char *execution_file;
    unsigned int execution_line;
    unsigned int execution_column;
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
};

static inline void context_clean_configuration (struct context_t *instance)
{
    instance->state_flags = 0u;
    instance->features = 0u;
    instance->options = 0u;
    instance->includes_first = NULL;
    instance->inputs_first = NULL;
    instance->inputs_last = NULL;
    instance->output_path = NULL;
    instance->cmake_depfile_path = NULL;

    instance->execution_file = NULL;
    instance->execution_line = 0u;
    instance->execution_column = 0u;

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

#define CUSHION_EXECUTION_ERROR(FORMAT, ...)                                                                           \
    fprintf (stderr, "[%s:%u:%u] " FORMAT, instance->execution_file, instance->execution_line,                         \
             instance->execution_column, __VA_ARGS__);                                                                 \
    instance->state_flags |= CONTEXT_STATE_FLAG_ERRED

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
            CUSHION_EXECUTION_ERROR ("Encountered macro \"%s\" redefinition.", node->name);
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
        fprintf (stderr, "Missing inputs in configuration.");
        result = CUSHION_RESULT_PARTIAL_CONFIGURATION;
    }

    if (!instance->output_path)
    {
        fprintf (stderr, "Missing output path in configuration.");
        result = CUSHION_RESULT_PARTIAL_CONFIGURATION;
    }

    if (result == CUSHION_RESULT_OK)
    {
        // TODO: Implement. Do things.
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
