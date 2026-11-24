# set -u (nounset) POSIX.1-2024 rules

# Unset variable fails and stops non-interactive subshell
(
	set -u
	echo "$UNSET_VAR_013"
	echo never
) 2>/dev/null
[ $? -ne 0 ] && echo "unset_var=fail" || echo "unset_var=ok"

# Empty-but-set variable is allowed
(
	set -u
	empty=""
	echo "empty=<$empty>"
)

# Default / alternate / assign forms do not trigger nounset
(
	set -u
	echo "def=${UNSET_VAR_013-fallback}"
	echo "cdef=${UNSET_VAR_013:-cfallback}"
	echo "alt=<${UNSET_VAR_013+yes}>"
	echo "calt=<${UNSET_VAR_013:+yes}>"
	echo "asg=${NEW_VAR_013=assigned}"
	echo "after=$NEW_VAR_013"
)

# Special parameters @, *, #, ?, -, $, 0 are exempt
(
	set -u
	set --
	echo "at=<$@> star=<$*> hash=$# q=$?"
)

# Unset positional parameter fails
(
	set -u
	set -- onlyone
	echo "$1"
	echo "$2"
	echo never
) 2>/dev/null
[ $? -ne 0 ] && echo "unset_pos=fail" || echo "unset_pos=ok"

# Length and trim on unset variable fail
(
	set -u
	echo "${#UNSET_VAR_013}"
	echo never
) 2>/dev/null
[ $? -ne 0 ] && echo "unset_len=fail" || echo "unset_len=ok"

(
	set -u
	echo "${UNSET_VAR_013#a}"
	echo never
) 2>/dev/null
[ $? -ne 0 ] && echo "unset_trim=fail" || echo "unset_trim=ok"

# Arithmetic expansion: unset variable fails, short-circuited branch does not
(
	set -u
	echo "sc=$((0 && UNSET_VAR_013))"
	echo "assign=$((ARITH_NEW_013 = 7))"
	echo "read=$((ARITH_NEW_013 + 1))"
)

(
	set -u
	echo "$((UNSET_VAR_013 + 1))"
	echo never
) 2>/dev/null
[ $? -ne 0 ] && echo "unset_arith=fail" || echo "unset_arith=ok"

# set +u turns nounset off again
(
	set -u
	set +u
	echo "off=<$UNSET_VAR_013>"
)
