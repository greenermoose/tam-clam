# exec and times special builtins.

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# exec with redirections only persists across subsequent commands.
(
	exec >"$tmp/out.txt"
	echo line1
	echo line2
)
cat "$tmp/out.txt"

# exec duplicating and closing file descriptors.
(
	exec 3>&1
	echo via-fd3 >&3
	exec 3>&-
)

# exec replacing a subshell process.
(
	exec sh -c 'echo replaced'
	echo never-reached
)

# exec with -- and command.
(
	exec -- sh -c 'echo dash-dash'
)

# times prints two lines of "<m>m<s>.<f>s <m>m<s>.<f>s".
t_out=$(times)
nlines=$(printf '%s\n' "$t_out" | wc -l)
echo "times-lines:$nlines"
case "$t_out" in
	*[0-9]m[0-9]*.[0-9]*s\ [0-9]m[0-9]*.[0-9]*s*) echo "times-fmt:ok" ;;
	*) echo "times-fmt:bad:$t_out" ;;
esac
