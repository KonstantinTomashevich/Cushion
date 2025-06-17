find_program (PERL_EXECUTABLE perl REQUIRED)
file (MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_results")

function (register_test TEST_NAME)
    add_test (
            NAME "${TEST_NAME}"
            COMMAND
            "${PERL_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/test_launcher"
            "$<TARGET_FILE:cushion>"
            "${TEST_NAME}"
            ${ARGN}
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_results"
            COMMAND_EXPAND_LISTS)
endfunction ()

register_test ("combined_features" "--features" "defer" "wrapper-macro" "statement-accumulator")
register_test ("comments_trivial")
register_test ("conditional_inclusion_defined")
register_test ("conditional_inclusion_evaluate_integer")
register_test ("conditional_inclusion_evaluate_macro")
register_test ("conditional_inclusion_preserve")
register_test ("conditional_inclusion_trivial")
register_test ("custom_line_directive")
register_test ("defer_goto" "--features" "defer")
register_test ("defer_loop" "--features" "defer")
register_test ("defer_scope" "--features" "defer")
register_test ("defer_switch" "--features" "defer")
register_test ("include_local")
register_test ("include_pragma_once")
register_test ("include_recursive")
register_test ("include_scan_only")
register_test ("include_trivial")
register_test ("macro_command_line" "--define" "IN_1" "IN_2" "IN_3" "MACRO_WITH_VALUE=1 + 2 + 3")
register_test ("macro_concatenate")
register_test ("macro_preserve")
register_test ("macro_stringize")
register_test ("macro_trivial")
register_test ("macro_undef")
register_test ("macro_variadic")
register_test ("macro_wrapper" "--features" "wrapper-macro")
register_test ("multiple_input" "--input" 
        "${CMAKE_CURRENT_SOURCE_DIR}/source/multiple_input_append_1.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/multiple_input_append_2.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/multiple_input_append_3.c")
register_test ("pragma_trivial")
register_test ("statement_accumulator_combine_files" "--features" "statement-accumulator")
register_test ("statement_accumulator_macro" "--features" "wrapper-macro" "statement-accumulator")
register_test ("statement_accumulator_optional" "--features" "statement-accumulator")
register_test ("statement_accumulator_trivial" "--features" "statement-accumulator")
register_test ("statement_accumulator_unique" "--features" "statement-accumulator")
register_test ("statement_accumulator_unordered" "--features" "statement-accumulator")

