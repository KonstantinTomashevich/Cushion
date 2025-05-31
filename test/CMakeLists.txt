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

register_test ("comments_trivial")
register_test ("conditional_inclusion_defined")
register_test ("conditional_inclusion_evaluate_integer")
register_test ("conditional_inclusion_evaluate_macro")
register_test ("conditional_inclusion_trivial")
register_test ("macro_concatenate")
register_test ("macro_preserve")
register_test ("macro_stringize")
register_test ("macro_trivial")
register_test ("macro_variadic")

