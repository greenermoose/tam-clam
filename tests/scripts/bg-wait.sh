# Background commands (&), $!, and wait.

# Single background job with explicit exit status.
(exit 42) &
p1=$!
case "$p1" in
	[0-9]*) echo "pid1:ok" ;;
	*) echo "pid1:bad:$p1" ;;
esac
wait "$p1"
echo "st1:$?"

# Waiting on the same PID again returns 127.
wait "$p1"
echo "again:$?"

# Waiting on an unknown PID returns 127.
wait 999999
echo "unknown:$?"

# Multiple background jobs; wait with multiple PIDs returns last status.
(exit 3) &
a=$!
(exit 7) &
b=$!
wait "$a" "$b"
echo "multi:$?"

# wait with no arguments waits for all and returns 0.
(exit 5) &
(exit 9) &
wait
echo "all:$?"

# Asynchronous command list (a & b & wait).
(exit 11) & p_x=$!; (exit 13) & p_y=$!
wait "$p_x"
echo "px:$?"
wait "$p_y"
echo "py:$?"

# Background command gets /dev/null as stdin when not redirected.
(cat) &
wait $!
echo "stdin:$?"
