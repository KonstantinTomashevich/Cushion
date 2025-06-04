#include "internal.h"

cushion_context_t cushion_context_create (void)
{
    struct cushion_instance_t *instance = malloc (sizeof (struct cushion_instance_t));
    cushion_allocator_init (&instance->allocator);
    cushion_instance_clean_configuration (instance);

    cushion_context_t result = {.value = instance};
    return result;
}

void cushion_context_configure_feature (cushion_context_t context, enum cushion_feature_t feature, unsigned int enabled)
{
    struct cushion_instance_t *instance = context.value;
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
    struct cushion_instance_t *instance = context.value;
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
    struct cushion_instance_t *instance = context.value;
    struct cushion_input_node_t *node =
        cushion_allocator_allocate (&instance->allocator, sizeof (struct cushion_input_node_t),
                                    _Alignof (struct cushion_input_node_t), CUSHION_ALLOCATION_CLASS_PERSISTENT);

    node->path = cushion_instance_copy_null_terminated_inside (instance, path, CUSHION_ALLOCATION_CLASS_PERSISTENT);
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
    struct cushion_instance_t *instance = context.value;
    instance->output_path =
        cushion_instance_copy_null_terminated_inside (instance, path, CUSHION_ALLOCATION_CLASS_PERSISTENT);
}

void cushion_context_configure_cmake_depfile (cushion_context_t context, const char *path)
{
    struct cushion_instance_t *instance = context.value;
    instance->cmake_depfile_path =
        cushion_instance_copy_null_terminated_inside (instance, path, CUSHION_ALLOCATION_CLASS_PERSISTENT);
}

void cushion_context_configure_define (cushion_context_t context, const char *name, const char *value)
{
    struct cushion_instance_t *instance = context.value;
    struct cushion_macro_node_t *new_node =
        cushion_allocator_allocate (&instance->allocator, sizeof (struct cushion_macro_node_t),
                                    _Alignof (struct cushion_macro_node_t), CUSHION_ALLOCATION_CLASS_PERSISTENT);

    new_node->name = cushion_instance_copy_null_terminated_inside (instance, name, CUSHION_ALLOCATION_CLASS_PERSISTENT);
    new_node->flags = CUSHION_MACRO_FLAG_NONE;
    new_node->value =
        cushion_instance_copy_null_terminated_inside (instance, value, CUSHION_ALLOCATION_CLASS_PERSISTENT);
    new_node->parameters_first = NULL;

    new_node->next = instance->unresolved_macros_first;
    instance->unresolved_macros_first = new_node;
}

void cushion_context_configure_include_full (cushion_context_t context, const char *path)
{
    struct cushion_instance_t *instance = context.value;
    struct cushion_include_node_t *node =
        cushion_allocator_allocate (&instance->allocator, sizeof (struct cushion_include_node_t),
                                    _Alignof (struct cushion_include_node_t), CUSHION_ALLOCATION_CLASS_PERSISTENT);

    node->type = INCLUDE_TYPE_FULL;
    node->path = cushion_instance_copy_null_terminated_inside (instance, path, CUSHION_ALLOCATION_CLASS_PERSISTENT);
    cushion_instance_includes_add (instance, node);
}

void cushion_context_configure_include_scan_only (cushion_context_t context, const char *path)
{
    struct cushion_instance_t *instance = context.value;
    struct cushion_include_node_t *node =
        cushion_allocator_allocate (&instance->allocator, sizeof (struct cushion_include_node_t),
                                    _Alignof (struct cushion_include_node_t), CUSHION_ALLOCATION_CLASS_PERSISTENT);

    node->type = INCLUDE_TYPE_SCAN;
    node->path = cushion_instance_copy_null_terminated_inside (instance, path, CUSHION_ALLOCATION_CLASS_PERSISTENT);
    cushion_instance_includes_add (instance, node);
}

enum cushion_result_t cushion_context_execute (cushion_context_t context)
{
    struct cushion_instance_t *instance = context.value;
    enum cushion_result_t result = CUSHION_RESULT_OK;
    instance->state_flags = CUSHION_INSTANCE_STATE_FLAG_EXECUTION;

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
        struct cushion_macro_node_t *macro_node = instance->unresolved_macros_first;
        instance->unresolved_macros_first = NULL;

        while (macro_node)
        {
            struct cushion_macro_node_t *next = macro_node->next;
            struct cushion_tokenization_state_t tokenization_state;

            struct cushion_allocator_transient_marker_t transient_marker =
                cushion_allocator_get_transient_marker (&instance->allocator);

            cushion_tokenization_state_init_for_argument_string (
                &tokenization_state, macro_node->value, &instance->allocator, CUSHION_ALLOCATION_CLASS_TRANSIENT);

            enum cushion_lex_replacement_list_result_t lex_result = cushion_lex_replacement_list_from_tokenization (
                instance, &tokenization_state, &macro_node->replacement_list_first, &macro_node->flags);

            if (cushion_instance_is_error_signaled (instance))
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
#if defined(CUSHION_EXTENSIONS)
            else if (macro_node->flags & CUSHION_MACRO_FLAG_WRAPPED)
            {
                fprintf (stderr,
                         "Object macro \"%s\" from arguments cannot use __CUSHION_WRAPPED__, this feature is only "
                         "supported for macro defined in code.\n",
                         macro_node->name);
                result = CUSHION_RESULT_FAILED_TO_LEX_CONFIGURED_DEFINES;
            }
#endif
            else
            {
                switch (lex_result)
                {
                case CUSHION_LEX_REPLACEMENT_LIST_RESULT_REGULAR:
                    cushion_instance_macro_add (instance, macro_node, NULL);
                    break;

                case CUSHION_LEX_REPLACEMENT_LIST_RESULT_PRESERVED:
                    fprintf (stderr,
                             "Encountered __CUSHION_PRESERVE__ while lexing macro \"%s\" from arguments, which is not "
                             "supported.\n",
                             macro_node->name);
                    result = CUSHION_RESULT_FAILED_TO_LEX_CONFIGURED_DEFINES;
                    break;
                }
            }

            cushion_allocator_reset_transient (&instance->allocator, transient_marker);
            macro_node = next;
        }
    }

    if (result == CUSHION_RESULT_OK)
    {
        instance->output = fopen (instance->output_path, "w");
        if (instance->cmake_depfile_path)
        {
            instance->cmake_depfile_output = fopen (instance->cmake_depfile_path, "w");
            if (instance->cmake_depfile_output)
            {
                cushion_instance_output_depfile_target (instance);
            }
            else
            {
                fprintf (stderr, "Failed to open depfile output file \"%s\".\n", instance->cmake_depfile_path);
                result = CUSHION_RESULT_FAILED_TO_OPEN_OUTPUT;
            }
        }

        if (instance->output)
        {
            struct cushion_input_node_t *input_node = instance->inputs_first;
            while (input_node)
            {
                cushion_lex_root_file (instance, input_node->path, CUSHION_LEX_FILE_FLAG_NONE);
                if (cushion_instance_is_error_signaled (instance))
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

        if (instance->cmake_depfile_output)
        {
            fclose (instance->cmake_depfile_output);
        }
    }

    // Reset all the configuration.
    cushion_instance_clean_configuration (instance);

    // Shrink and reset memory usage.
    cushion_allocator_shrink (&instance->allocator);
    cushion_allocator_reset_all (&instance->allocator);

    return result;
}

void cushion_context_destroy (cushion_context_t context)
{
    struct cushion_instance_t *instance = context.value;
    cushion_allocator_shutdown (&instance->allocator);
    free (instance);
}
