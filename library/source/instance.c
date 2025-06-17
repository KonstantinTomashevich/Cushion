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

#if defined(CUSHION_EXTENSIONS)
    instance->deferred_output_first = NULL;
    instance->deferred_output_last = NULL;
    instance->deferred_output_selected = NULL;

    instance->free_buffers_first = NULL;

    instance->statement_accumulators_first = NULL;
    instance->statement_accumulator_refs_first = NULL;

    instance->statement_unordered_push_first = NULL;
    instance->statement_unordered_push_last = NULL;
#endif

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

    for (unsigned int index = 0u; index < CUSHION_DEPFILE_BUCKETS; ++index)
    {
        instance->cmake_depfile_buckets[index] = NULL;
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
                                 struct cushion_error_context_t error_context)
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
            cushion_instance_execution_error (instance, error_context, "Encountered macro \"%s\" redefinition.",
                                              node->name);
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

#if defined(CUSHION_EXTENSIONS)
struct cushion_output_buffer_node_t *new_cushion_output_buffer_node (struct cushion_instance_t *instance)
{
    struct cushion_output_buffer_node_t *allocated;
    if (instance->free_buffers_first)
    {
        allocated = instance->free_buffers_first;
        instance->free_buffers_first = allocated->next;
    }
    else
    {
        allocated = cushion_allocator_allocate (&instance->allocator, sizeof (struct cushion_output_buffer_node_t),
                                                _Alignof (struct cushion_output_buffer_node_t),
                                                CUSHION_ALLOCATION_CLASS_PERSISTENT);
    }

    allocated->next = NULL;
    allocated->end = allocated->data;
    return allocated;
}
#endif

void cushion_instance_output_sequence (struct cushion_instance_t *instance, const char *begin, const char *end)
{
    if (instance->output)
    {
        size_t length = end - begin;

#if defined(CUSHION_EXTENSIONS)
        struct cushion_deferred_output_node_t *deferred_node =
            instance->deferred_output_selected ? instance->deferred_output_selected : instance->deferred_output_last;

        if (deferred_node)
        {
            if (!deferred_node->content_last)
            {
                deferred_node->content_last = new_cushion_output_buffer_node (instance);
                deferred_node->content_first = deferred_node->content_last;
            }

            while (length > 0u)
            {
                const size_t used_in_buffer = deferred_node->content_last->end - deferred_node->content_last->data;
                const size_t left_in_buffer = CUSHION_OUTPUT_BUFFER_NODE_SIZE - used_in_buffer;

                if (left_in_buffer > length)
                {
                    memcpy (deferred_node->content_last->end, begin, length);
                    deferred_node->content_last->end += length;
                    break;
                }
                else
                {
                    memcpy (deferred_node->content_last->end, begin, left_in_buffer);
                    deferred_node->content_last->end += left_in_buffer;
                    begin += left_in_buffer;
                    length -= left_in_buffer;

                    deferred_node->content_last->next = new_cushion_output_buffer_node (instance);
                    deferred_node->content_last = deferred_node->content_last->next;
                }
            }

            return;
        }
#endif

        if (fwrite (begin, 1u, length, instance->output) != length)
        {
            fprintf (stderr, "Failed to output preprocessed code.\n");
            cushion_instance_signal_error (instance);
        }
    }
}

#if defined(CUSHION_EXTENSIONS)
struct cushion_deferred_output_node_t *cushion_output_add_deferred_sink (struct cushion_instance_t *instance,
                                                                         const char *source_file,
                                                                         unsigned int source_line)
{
    const char *source_file_safe =
        cushion_instance_copy_null_terminated_inside (instance, source_file, CUSHION_ALLOCATION_CLASS_PERSISTENT);

    // Nodes are not reused as they're relatively small. Only buffers are reused as they're relatively big.

    struct cushion_deferred_output_node_t *deferred_node = cushion_allocator_allocate (
        &instance->allocator, sizeof (struct cushion_deferred_output_node_t),
        _Alignof (struct cushion_deferred_output_node_t), CUSHION_ALLOCATION_CLASS_PERSISTENT);

    deferred_node->flags = CUSHION_DEFERRED_OUTPUT_NODE_FLAG_UNFINISHED;
    deferred_node->source_file = source_file_safe;
    deferred_node->source_line = source_line;
    deferred_node->content_first = NULL;
    deferred_node->content_last = NULL;

    struct cushion_deferred_output_node_t *follow_up_node = cushion_allocator_allocate (
        &instance->allocator, sizeof (struct cushion_deferred_output_node_t),
        _Alignof (struct cushion_deferred_output_node_t), CUSHION_ALLOCATION_CLASS_PERSISTENT);

    follow_up_node->flags = CUSHION_DEFERRED_OUTPUT_NODE_FLAG_NONE;
    follow_up_node->source_file = source_file_safe;
    follow_up_node->source_line = source_line;
    follow_up_node->content_first = NULL;
    follow_up_node->content_last = NULL;

    deferred_node->next = follow_up_node;
    follow_up_node->next = NULL;

    if (instance->deferred_output_last)
    {
        instance->deferred_output_last->next = deferred_node;
    }
    else
    {
        instance->deferred_output_first = deferred_node;
    }

    instance->deferred_output_last = follow_up_node;
    return deferred_node;
}

void cushion_output_select_sink (struct cushion_instance_t *instance, struct cushion_deferred_output_node_t *sink)
{
    assert (!sink || sink->flags & CUSHION_DEFERRED_OUTPUT_NODE_FLAG_UNFINISHED);
    instance->deferred_output_selected = sink;
}

static void flush_sink (struct cushion_instance_t *instance, struct cushion_deferred_output_node_t *sink)
{
    // We do not add line directives before and after sinks as we expect their content to handle it properly.
    struct cushion_output_buffer_node_t *buffer = sink->content_first;

    while (buffer)
    {
        if (buffer->data != buffer->end)
        {
            const size_t length = buffer->end - buffer->data;
            if (fwrite (buffer->data, 1u, length, instance->output) != length)
            {
                fprintf (stderr, "Failed to output preprocessed code.\n");
                cushion_instance_signal_error (instance);
            }
        }

        buffer = buffer->next;
    }

    // Return buffers to free list.
    if (sink->content_last)
    {
        sink->content_last->next = instance->free_buffers_first;
        instance->free_buffers_first = sink->content_first;
    }
}

void cushion_output_finish_sink (struct cushion_instance_t *instance, struct cushion_deferred_output_node_t *sink)
{
    assert (sink->flags & CUSHION_DEFERRED_OUTPUT_NODE_FLAG_UNFINISHED);
    sink->flags &= ~CUSHION_DEFERRED_OUTPUT_NODE_FLAG_UNFINISHED;

    if (sink == instance->deferred_output_first)
    {
        // It was a blocking sink, try flush everything now.
        struct cushion_deferred_output_node_t *current = instance->deferred_output_first;

        while (current)
        {
            if (current->flags & CUSHION_DEFERRED_OUTPUT_NODE_FLAG_UNFINISHED)
            {
                // Flushed everything that can be flushed now.
                break;
            }

            flush_sink (instance, current);
            current = current->next;
        }

        instance->deferred_output_first = current;
        if (!current)
        {
            instance->deferred_output_last = NULL;
        }
    }
}

void cushion_output_finalize (struct cushion_instance_t *instance)
{
    if (instance->deferred_output_first)
    {
        cushion_instance_signal_error (instance);
        fprintf (stderr,
                 "Failed to properly write output: some sinks are still unfinished. See errors above for reasons (and "
                 "if there is no reasons, then it is an internal error).\n");
        struct cushion_deferred_output_node_t *current = instance->deferred_output_first;

        while (current)
        {
            if (current->flags & CUSHION_DEFERRED_OUTPUT_NODE_FLAG_UNFINISHED)
            {
                fprintf (stderr, "    Sink created at \"%s\" line %u is not finished.\n", current->source_file,
                         (unsigned int) current->source_line);

                // Add information to the output file too.
                cushion_instance_output_formatted (
                    instance, "\n#line %u \"%s\"\n/* Sink that was created here is not finished properly. */\n",
                    (unsigned int) current->source_line, current->source_file);

                // Restore line number for following sinks.
                if (current->next)
                {
                    cushion_instance_output_formatted (instance, "#line %u \"%s\"\n",
                                                       (unsigned int) current->next->source_line,
                                                       current->next->source_file);
                }

                // No need for returning buffers to free list, as we're finalizing everything either way.
            }
            else
            {
                flush_sink (instance, current);
            }

            current = current->next;
        }
    }
}
#endif

static void output_depfile_path_name (struct cushion_instance_t *instance, const char *absolute_path_name)
{
    char conversion_buffer[CUSHION_PATH_MAX];
    const char *cursor = absolute_path_name;
    char *output = conversion_buffer;

#define CHECK_OUTPUT_BOUNDS                                                                                            \
    if (output >= conversion_buffer + CUSHION_PATH_MAX)                                                                \
    {                                                                                                                  \
        fprintf (stderr, "Failed to add path name \"%s\" to depfile due to path length overflow,\n",                   \
                 absolute_path_name);                                                                                  \
        cushion_instance_signal_error (instance);                                                                      \
        return;                                                                                                        \
    }

    while (*cursor)
    {
        if (*cursor == ' ')
        {
            CHECK_OUTPUT_BOUNDS
            *output = '\\';
            ++output;

            CHECK_OUTPUT_BOUNDS
            *output = ' ';
            ++output;
        }
        else if (*cursor == '\\')
        {
            // Replace \ with / for depfile format. Replace \\ with single /.
            if (*(cursor + 1u) == '\\')
            {
                ++cursor;
            }

            CHECK_OUTPUT_BOUNDS
            *output = '/';
            ++output;
        }
        else
        {
            CHECK_OUTPUT_BOUNDS
            *output = *cursor;
            ++output;
        }

        ++cursor;
    }

    CHECK_OUTPUT_BOUNDS
    *output = '\0';

    if (fprintf (instance->cmake_depfile_output, "%s ", conversion_buffer) == 0)
    {
        fprintf (stderr, "Failed to output depfile path name.\n");
        cushion_instance_signal_error (instance);
    }
}

void cushion_instance_output_depfile_target (struct cushion_instance_t *instance)
{
    if (instance->cmake_depfile_output)
    {
        // Convert output path to absolute as it might be relative, but depfile must use absolute paths.
        char absolute_buffer[CUSHION_PATH_MAX];

        if (cushion_convert_path_to_absolute (instance->output_path, absolute_buffer) != CUSHION_INTERNAL_RESULT_OK)
        {
            fprintf (stderr, "Failed to convert output path to absolute for depfile.\n");
            cushion_instance_signal_error (instance);
            return;
        }

        output_depfile_path_name (instance, absolute_buffer);
        if (fprintf (instance->cmake_depfile_output, ": ") == 0)
        {
            fprintf (stderr, "Failed to output depfile target separator.\n");
            cushion_instance_signal_error (instance);
        }
    }
}

void cushion_instance_output_depfile_entry (struct cushion_instance_t *instance, const char *absolute_path)
{
    if (instance->cmake_depfile_output)
    {
        const unsigned int path_hash = cushion_hash_djb2_null_terminated (absolute_path);
        struct cushion_depfile_dependency_node_t *search_node =
            instance->cmake_depfile_buckets[path_hash % CUSHION_DEPFILE_BUCKETS];

        while (search_node)
        {
            if (search_node->path_hash == path_hash && strcmp (search_node->path, absolute_path) == 0)
            {
                break;
            }

            search_node = search_node->next;
        }

        if (search_node)
        {
            // Already added to depfile.
            return;
        }

        struct cushion_depfile_dependency_node_t *new_node = cushion_allocator_allocate (
            &instance->allocator, sizeof (struct cushion_depfile_dependency_node_t),
            _Alignof (struct cushion_depfile_dependency_node_t), CUSHION_ALLOCATION_CLASS_PERSISTENT);

        new_node->path_hash = path_hash;
        new_node->path =
            cushion_instance_copy_null_terminated_inside (instance, absolute_path, CUSHION_ALLOCATION_CLASS_PERSISTENT);

        new_node->next = instance->cmake_depfile_buckets[path_hash % CUSHION_PRAGMA_ONCE_BUCKETS];
        instance->cmake_depfile_buckets[path_hash % CUSHION_PRAGMA_ONCE_BUCKETS] = new_node;
        output_depfile_path_name (instance, absolute_path);
    }
}

void cushion_instance_execution_error_internal (struct cushion_instance_t *instance,
                                                struct cushion_error_context_t context,
                                                const char *format,
                                                va_list variadic_arguments)
{
    if (context.column != UINT_MAX)
    {
        fprintf (stderr, "[%s:%u:%u] ", context.file, (unsigned int) context.line, (unsigned int) context.column);
    }
    else
    {
        fprintf (stderr, "[%s:%u] ", context.file, (unsigned int) context.line);
    }

    vfprintf (stderr, format, variadic_arguments);
    fprintf (stderr, "\n");
    cushion_instance_signal_error (instance);
}
