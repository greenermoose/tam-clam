greet() {
	echo "n=$# 1=$1 2=$2"
}
greet alpha beta

outer_args() {
	echo "before:$#:$1:$2"
	greet inner1
	echo "after:$#:$1:$2"
}
outer_args A B

check_zero() {
	if test "$0" = "$1"; then
		echo "zero-ok"
	fi
}
check_zero "$0"

v=outer
show_v() {
	echo "in-func:$v"
}
v=temp show_v
echo "after-temp:$v"

ret_val() {
	return 42
	echo never
}
ret_val
echo "ret42:$?"

ret_last() {
	(exit 9)
	return
	echo never
}
ret_last
echo "ret-last:$?"

true() {
	echo "custom-true"
}
true

redef() {
	echo "v1-start"
	redef() {
		echo "v2"
	}
	echo "v1-end"
}
redef
redef

countdown() {
	if test -z "$1"; then
		echo "done"
		return 0
	fi
	echo "step:$1"
	countdown "${1#x}"
}
countdown xxx

with_redir() {
	cat
} <<EOF
from-func-heredoc
EOF
with_redir

emit() {
	echo hello
	echo world
}
emit | tr a-z A-Z
