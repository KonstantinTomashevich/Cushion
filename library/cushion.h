#pragma once

#if defined(__cplusplus)
#    define CUSHION_HEADER_BEGIN                                                                                       \
        extern "C"                                                                                                     \
        {
#    define CUSHION_HEADER_END }
#else
#    define CUSHION_HEADER_BEGIN
#    define CUSHION_HEADER_END
#endif

#define CUSHION_HANDLE(NAME)                                                                                           \
    struct cushion_handle_##NAME                                                                                       \
    {                                                                                                                  \
        void *value;                                                                                                   \
    };                                                                                                                 \
    typedef struct cushion_handle_##NAME NAME

CUSHION_HEADER_BEGIN

CUSHION_HANDLE (cushion_context_t);

enum cushion_feature_t
{
    CUSHION_FEATURE_DEFER = 0u,
    CUSHION_FEATURE_WRAPPER_MACRO,
    CUSHION_FEATURE_STATEMENT_ACCUMULATOR,
};

/// \details There is no option for keeping comments for several reasons:
///          - There is no kind-of-standard behavior for doing so, some preprocessors still eat some comments in
///            some cases even with all the options.
///          - It is difficult to find a case for using them for passing meta information in code, because custom
///            pragmas are much more useful for that case.
///          That means that there is no standardized way to handle them and there is no use case for cushion when it
///          is useful to keep comments, therefore doing it right now is futile and unnecessary complicates code.
enum cushion_option_t
{
    CUSHION_OPTION_FORBID_MACRO_REDEFINITION = 0u,
};

enum cushion_result_t
{
    CUSHION_RESULT_OK = 0u,
    CUSHION_RESULT_PARTIAL_CONFIGURATION,
    CUSHION_RESULT_UNSUPPORTED_FEATURES,
    CUSHION_RESULT_FAILED_TO_LEX_CONFIGURED_DEFINES,
    CUSHION_RESULT_FAILED_TO_OPEN_OUTPUT,
    CUSHION_RESULT_LEX_FAILED,
};

cushion_context_t cushion_context_create (void);

void cushion_context_configure_feature (cushion_context_t context,
                                        enum cushion_feature_t feature,
                                        unsigned int enabled);

void cushion_context_configure_option (cushion_context_t context, enum cushion_option_t option, unsigned int enabled);

void cushion_context_configure_input (cushion_context_t context, const char *path);

/// \warning Overrides previous output value if any!
void cushion_context_configure_output (cushion_context_t context, const char *path);

/// \warning Overrides previous cmake depfile value if any!
void cushion_context_configure_cmake_depfile (cushion_context_t context, const char *path);

void cushion_context_configure_define (cushion_context_t context, const char *name, const char *value);

void cushion_context_configure_include_full (cushion_context_t context, const char *path);

void cushion_context_configure_include_scan_only (cushion_context_t context, const char *path);

enum cushion_result_t cushion_context_execute (cushion_context_t context);

void cushion_context_destroy (cushion_context_t context);

CUSHION_HEADER_END
