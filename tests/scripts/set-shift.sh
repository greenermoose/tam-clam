# set positional parameters and shift.

set -- alpha beta gamma delta
echo "$#:$1:$2:$3:$4"

shift
echo "$#:$1:$2:$3"

shift 2
echo "$#:$1"

shift 0
echo "$#:$1"

shift 1
echo "$#:$*"

# set without -- when first arg does not start with - or +.
set one two
echo "$#:$1:$2"

# set -- with no args clears positional parameters.
set --
echo "cleared:$#"

# set -- with args starting with '-'.
set -- -x -y
echo "$#:$1:$2"

# shift inside a function does not affect outer positional parameters.
set -- outer1 outer2
fn() {
	shift
	echo "fn:$#:$1"
}
fn a b c
echo "outer:$#:$1:$2"
