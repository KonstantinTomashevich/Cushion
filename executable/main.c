#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cushion.h>

enum argument_mode_t
{
    ARGUMENT_MODE_NONE = 0u,
    ARGUMENT_MODE_FEATURES,
    ARGUMENT_MODE_OPTIONS,
    ARGUMENT_MODE_INPUT,
    ARGUMENT_MODE_OUTPUT,
    ARGUMENT_MODE_CMAKE_DEPFILE,
    ARGUMENT_MODE_DEFINE,
    ARGUMENT_MODE_INCLUDE_FULL,
    ARGUMENT_MODE_INCLUDE_SCAN,
};

static const char help_message[] =
    "Cushion " CUSHION_VERSION_STRING
    "\n"
    "C preprocessor for feeding the code to code parsers and generators.\n"
    "\n"
    "To specify what arguments mean, argument switches are used:\n"
    "\n"
    "    --features         Any argument after this one is feature request. Supported:\n"
    "                           defer                    Support for defer blocks extension.\n"
    "                           wrapper-macro            Support for wrapper macros extension.\n"
    "                           statement-accumulator    Support for statement accumulator extension.\n"
    "                           snippet                  Support for snippet macros extension.\n"
    "\n"
    "    --options          Any argument after this one is option. Supported:\n"
    "                           forbid-macro-redefinition    Ignore macro redefinitions and print error\n"
    "                                                        when one is encountered.\n"
    "\n"
    "    --input            Any argument after this one is an input file for preprocessing.\n"
    "                       Multiple input files are treated like one file that includes all the inputs.\n"
    "\n"
    "    --output           Any argument after this one is an output file. Only one output file is supported.\n"
    "\n"
    "    --cmake-depfile    Any argument after this one is a cmake depfile output file.\n"
    "                       Only one cmake depfile output file is supported.\n"
    "\n"
    "    --define           Any argument after this one is a command line definition.\n"
    "                       Macro can be passed as just macro name, then value \"1\" is automatically used for it.\n"
    "                       Alternatively, macro can be passed in MACRO_NAME=MACRO_VALUE format as argument.\n"
    "\n"
    "    --include-full     Any argument after this one is an include path files under which are fully included.\n"
    "                       Includes in \"#include \"<path>\"\" format will be fully included if their path is\n"
    "                       absolute or relative to file even when this path is not passed through include full\n"
    "                       arguments.\n"
    "\n"
    "    --include-scan     Any argument after this one is a scan-only include path.\n"
    "\n"
    "For proper execution, at least one input and output must be specified. Other arguments are optional.\n";

int main (int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && (strcmp (argv[1u], "--help") == 0 || strcmp (argv[1u], "-help") == 0 ||
                                    strcmp (argv[1u], "/?") == 0)))
    {
        fprintf (stdout, "%s\n", help_message);
        return 0;
    }

    cushion_context_t context = cushion_context_create ();
    enum argument_mode_t argument_mode = ARGUMENT_MODE_NONE;
    uint8_t has_output = 0u;
    uint8_t has_cmake_depfile = 0u;

    for (unsigned int index = 1u; index < (unsigned int) argc; ++index)
    {
        char *argument = argv[index];
        if (strcmp (argument, "--features") == 0)
        {
            argument_mode = ARGUMENT_MODE_FEATURES;
            continue;
        }
        else if (strcmp (argument, "--options") == 0)
        {
            argument_mode = ARGUMENT_MODE_OPTIONS;
            continue;
        }
        else if (strcmp (argument, "--input") == 0)
        {
            argument_mode = ARGUMENT_MODE_INPUT;
            continue;
        }
        else if (strcmp (argument, "--output") == 0)
        {
            argument_mode = ARGUMENT_MODE_OUTPUT;
            continue;
        }
        else if (strcmp (argument, "--cmake-depfile") == 0)
        {
            argument_mode = ARGUMENT_MODE_CMAKE_DEPFILE;
            continue;
        }
        else if (strcmp (argument, "--define") == 0)
        {
            argument_mode = ARGUMENT_MODE_DEFINE;
            continue;
        }
        else if (strcmp (argument, "--include-full") == 0)
        {
            argument_mode = ARGUMENT_MODE_INCLUDE_FULL;
            continue;
        }
        else if (strcmp (argument, "--include-scan") == 0)
        {
            argument_mode = ARGUMENT_MODE_INCLUDE_SCAN;
            continue;
        }

        switch (argument_mode)
        {
        case ARGUMENT_MODE_NONE:
            fprintf (stderr, "Encountered argument without any argument switch before that.\n");
            cushion_context_destroy (context);
            return -1;

        case ARGUMENT_MODE_FEATURES:
            if (strcmp (argument, "defer") == 0)
            {
                cushion_context_configure_feature (context, CUSHION_FEATURE_DEFER, 1u);
            }
            else if (strcmp (argument, "wrapper-macro") == 0)
            {
                cushion_context_configure_feature (context, CUSHION_FEATURE_WRAPPER_MACRO, 1u);
            }
            else if (strcmp (argument, "statement-accumulator") == 0)
            {
                cushion_context_configure_feature (context, CUSHION_FEATURE_STATEMENT_ACCUMULATOR, 1u);
            }
            else if (strcmp (argument, "snippet") == 0)
            {
                cushion_context_configure_feature (context, CUSHION_FEATURE_SNIPPET, 1u);
            }
            else
            {
                fprintf (stderr, "Encountered unknown feature \"%s\".\n", argument);
                cushion_context_destroy (context);
                return -1;
            }

            break;

        case ARGUMENT_MODE_OPTIONS:
            if (strcmp (argument, "forbid-macro-redefinition") == 0)
            {
                cushion_context_configure_option (context, CUSHION_OPTION_FORBID_MACRO_REDEFINITION, 1u);
            }
            else
            {
                fprintf (stderr, "Encountered unknown option \"%s\".\n", argument);
                cushion_context_destroy (context);
                return -1;
            }

            break;

        case ARGUMENT_MODE_INPUT:
            cushion_context_configure_input (context, argument);
            break;

        case ARGUMENT_MODE_OUTPUT:
            if (has_output)
            {
                fprintf (stderr, "Encountered output more that once.\n");
                cushion_context_destroy (context);
                return -1;
            }
            else
            {
                cushion_context_configure_output (context, argument);
                has_output = 1u;
            }

            break;

        case ARGUMENT_MODE_CMAKE_DEPFILE:
            if (has_cmake_depfile)
            {
                fprintf (stderr, "Encountered cmake depfile more that once.\n");
                cushion_context_destroy (context);
                return -1;
            }
            else
            {
                cushion_context_configure_cmake_depfile (context, argument);
                has_cmake_depfile = 1u;
            }

            break;

        case ARGUMENT_MODE_DEFINE:
        {
            char *assign_place = argument;
            while (*assign_place != '=' && *assign_place)
            {
                ++assign_place;
            }

            if (*assign_place)
            {
                *assign_place = '\0';
                cushion_context_configure_define (context, argument, assign_place + 1u);
                *assign_place = '=';
            }
            else
            {
                cushion_context_configure_define (context, argument, "1");
            }

            break;
        }

        case ARGUMENT_MODE_INCLUDE_FULL:
            cushion_context_configure_include_full (context, argument);
            break;

        case ARGUMENT_MODE_INCLUDE_SCAN:
            cushion_context_configure_include_scan_only (context, argument);
            break;
        }
    }

    enum cushion_result_t result = cushion_context_execute (context);
    cushion_context_destroy (context);
    return result;
}
