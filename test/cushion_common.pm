# Module that stores common logic for filtering out user-specific path data from test files.

package cushion_common;
use strict;
use warnings FATAL => 'all';

use base "Exporter";
our @EXPORT = ('fix_line_directive', 'fix_depfile_line');

# Fix line directive for using in expectation saved in version control by removing user-specific path part.
sub fix_line_directive {
    my ($test_directory, $line) = @_;
    if ($line =~ /#line ([0-9]+) \"$test_directory\/([a-z0-9_\.\/\\]+)\"/) {
        $line = "#line " . $1 . " \"" . $2 . "\"\n";
    }

    return $line;
}

# Fix depfile line for using in expectation saved in version control by removing user-specific path part.
sub fix_depfile_line {
    my ($test_directory, $line) = @_;
    
    # Format to path name.
    $test_directory =~ s/\s/\\ /g;
    
    # Replace target path.
    $line =~ s/^.*\/([A-Za-z\\\s0-9_\.]+)\s:/$1 :/;
    
    # Replace depfile dependency paths.
    $line =~ s|$test_directory/||g;
    
    return $line;
}

1;
