/*!re2c
 id_start = [a-zA-Z_];
 id_continue = [a-zA-Z_0-9]*;
 */

// Non-ASCII characters are not supported in code as we do not expect them in identifiers.
/*!rules:re2c:check_unsupported_in_code
 [^\x00-\x7F]
 {
     cushion_instance_tokenization_error (
         instance, state,
         "Encountered non-ASCII character outside of comments and string literals."
         "This version of Cushion is built without unicode support for anything outside of comments and literals.");
     return;
 }
 */
