#include "internal.h"

void cushion_allocator_init (struct cushion_allocator_t *instance)
{
    instance->first_page = malloc (sizeof (struct cushion_allocator_page_t));
    instance->first_page->next = NULL;
    instance->first_page->top_transient = instance->first_page->data;
    instance->first_page->bottom_persistent = instance->first_page->data + CUSHION_ALLOCATOR_PAGE_SIZE;
    instance->current_page = instance->first_page;
}

static void *allocator_page_allocate (struct cushion_allocator_page_t *page,
                                      uintptr_t size,
                                      uintptr_t alignment,
                                      enum cushion_allocation_class_t class)
{
    switch (class)
    {
    case CUSHION_ALLOCATION_CLASS_TRANSIENT:
    {
        uint8_t *address = (uint8_t *) cushion_apply_alignment ((uintptr_t) page->top_transient, alignment);
        uint8_t *new_top = address + size;

        if (new_top <= (uint8_t *) page->bottom_persistent)
        {
            page->top_transient = new_top;
            return address;
        }

        break;
    }

    case CUSHION_ALLOCATION_CLASS_PERSISTENT:
    {
        uint8_t *address =
            (uint8_t *) cushion_apply_alignment_reversed (((uintptr_t) page->bottom_persistent) - size, alignment);

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

void *cushion_allocator_allocate (struct cushion_allocator_t *allocator,
                                  uintptr_t size,
                                  uintptr_t alignment,
                                  enum cushion_allocation_class_t class)
{
    void *result = allocator_page_allocate (allocator->current_page, size, alignment, class);
    if (!result)
    {
        if (!allocator->current_page->next)
        {
            struct cushion_allocator_page_t *new_page = malloc (sizeof (struct cushion_allocator_page_t));
            new_page->next = NULL;
            new_page->top_transient = new_page->data;
            new_page->bottom_persistent = new_page->data + CUSHION_ALLOCATOR_PAGE_SIZE;
            allocator->current_page->next = new_page;
        }
        else
        {
            allocator->current_page = allocator->current_page->next;
        }

        result = allocator_page_allocate (allocator->current_page, size, alignment, class);
    }

    if (!result)
    {
        fprintf (stderr, "Internal error: failed to allocate %lu bytes with %lu alignment.", (unsigned long) size,
                 (unsigned long) alignment);
        abort ();
    }

    return result;
}

struct cushion_allocator_transient_marker_t cushion_allocator_get_transient_marker (
    struct cushion_allocator_t *allocator)
{
    return (struct cushion_allocator_transient_marker_t) {
        .page = allocator->current_page,
        .top_transient = allocator->current_page->top_transient,
    };
}

void cushion_allocator_reset_transient (struct cushion_allocator_t *allocator,
                                        struct cushion_allocator_transient_marker_t transient_marker)
{
    allocator->current_page = transient_marker.page;
    allocator->current_page->top_transient = transient_marker.top_transient;
    struct cushion_allocator_page_t *page = allocator->current_page->next;

    while (page)
    {
        page->top_transient = page->data;
        page = page->next;
    }
}

void cushion_allocator_reset_all (struct cushion_allocator_t *allocator)
{
    struct cushion_allocator_page_t *page = allocator->first_page;
    while (page)
    {
        page->top_transient = page->data;
        page->bottom_persistent = page->data + CUSHION_ALLOCATOR_PAGE_SIZE;
        page = page->next;
    }

    allocator->current_page = allocator->first_page;
}

void cushion_allocator_shrink (struct cushion_allocator_t *allocator)
{
    struct cushion_allocator_page_t *page = allocator->current_page->next;
    struct cushion_allocator_page_t *previous = allocator->current_page;

    while (page)
    {
        struct cushion_allocator_page_t *next = page->next;
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

void cushion_allocator_shutdown (struct cushion_allocator_t *allocator)
{
    struct cushion_allocator_page_t *page = allocator->first_page;
    while (page)
    {
        struct cushion_allocator_page_t *next = page->next;
        free (page);
        page = next;
    }
}

void cushion_instance_clean_configuration (struct cushion_instance_t *instance)
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

    for (unsigned int index = 0u; index < CUSHION_PRAGMA_ONCE_BUCKETS; ++index)
    {
        instance->pragma_once_buckets[index] = NULL;
    }

    instance->unresolved_macros_first = NULL;
}

void cushion_instance_includes_add (struct cushion_instance_t *instance, struct cushion_include_node_t *node)
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

static inline struct cushion_macro_node_t *macro_search_in_list (struct cushion_macro_node_t *list,
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

struct cushion_macro_node_t *cushion_instance_macro_search (struct cushion_instance_t *instance,
                                                            const char *name_begin,
                                                            const char *name_end)
{
    unsigned int name_hash = cushion_hash_djb2_char_sequence (name_begin, name_end);
    struct cushion_macro_node_t *list = instance->macro_buckets[name_hash % CUSHION_MACRO_BUCKETS];
    return macro_search_in_list (list, name_hash, name_begin, name_end);
}

void cushion_instance_macro_add (struct cushion_instance_t *instance,
                                 struct cushion_macro_node_t *node,
                                 struct cushion_tokenization_state_t *tokenization_state_for_logging)
{
    node->name_hash = cushion_hash_djb2_null_terminated (node->name);
    // To have consistent behavior, calculate parameter hashes here too.
    struct cushion_macro_parameter_node_t *parameter = node->parameters_first;

    while (parameter)
    {
        parameter->name_hash = cushion_hash_djb2_null_terminated (parameter->name);
        parameter = parameter->next;
    }

    struct cushion_macro_node_t *bucket_list = instance->macro_buckets[node->name_hash % CUSHION_MACRO_BUCKETS];
    struct cushion_macro_node_t *already_here =
        macro_search_in_list (bucket_list, node->name_hash, node->name, node->name + strlen (node->name));

    if (already_here)
    {
        if (cushion_instance_has_option (instance, CUSHION_OPTION_FORBID_MACRO_REDEFINITION) &&
            (instance->state_flags & CUSHION_INSTANCE_STATE_FLAG_EXECUTION))
        {
            cushion_instance_execution_error (instance, tokenization_state_for_logging,
                                              "Encountered macro \"%s\" redefinition.", node->name);
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

void cushion_instance_macro_remove (struct cushion_instance_t *instance, const char *name_begin, const char *name_end)
{
    unsigned int name_hash = cushion_hash_djb2_char_sequence (name_begin, name_end);
    // Logic is almost the same as for search, but we need to keep previous pointer here for proper removal.
    struct cushion_macro_node_t *list = instance->macro_buckets[name_hash % CUSHION_MACRO_BUCKETS];
    struct cushion_macro_node_t *previous = NULL;

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

void cushion_instance_output_sequence (struct cushion_instance_t *instance, const char *begin, const char *end)
{
    if (instance->output)
    {
        // TODO: Proper deferred output needed for the future when statement accumulators are added.

        size_t length = end - begin;
        if (fwrite (begin, 1u, length, instance->output) != length)
        {
            fprintf (stderr, "Failed to output preprocessed code.\n");
            cushion_instance_signal_error (instance);
        }
    }
}

void cushion_instance_output_null_terminated (struct cushion_instance_t *instance, const char *string)
{
    return cushion_instance_output_sequence (instance, string, string + strlen (string));
}

void cushion_instance_output_line_marker (struct cushion_instance_t *instance, unsigned int line, const char *path)
{
    if (instance->output)
    {
        // TODO: Proper deferred output needed for the future when statement accumulators are added.

        if (fprintf (instance->output, "#line %u \"%s\"\n", line, path) == 0)
        {
            fprintf (stderr, "Failed to output preprocessed code.\n");
            cushion_instance_signal_error (instance);
        }
    }
}

void cushion_instance_execution_error (struct cushion_instance_t *instance,
                                       struct cushion_tokenization_state_t *state,
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
    cushion_instance_signal_error (instance);
}
