# trap for signals and EXIT.

# Catching USR1 at safe point, preserving $? across trap action.
got=""
trap 'got="caught:$?"' USR1
(exit 19)
kill -USR1 $$
# Status of kill is 0; trap sees 0 and restores 0.
echo "after-kill:$?:$got"

# Ignoring a signal with empty action.
trap '' USR1
kill -USR1 $$
echo "ignored:$?"

# Resetting a trap with '-'.
trap - USR1
echo "reset:ok"

# trap -p / $(trap) subshell display preservation.
trap 'echo sub_saved' USR2
saved=$(trap)
case "$saved" in
	*sub_saved*USR2*) echo "subshell-display:ok" ;;
	*) echo "subshell-display:bad:$saved" ;;
esac
trap - USR2

# Subshell with its own EXIT trap runs it without affecting parent.
(
	trap 'echo sub-exit:$?' EXIT
	exit 17
)
echo "sub-status:$?"

# Subshell EXIT trap overriding exit status with explicit exit.
(
	trap 'echo override; exit 23' EXIT
	exit 5
)
echo "override-status:$?"

# Parent EXIT trap runs when script finishes.
trap 'echo parent-exit:$?' EXIT
true
