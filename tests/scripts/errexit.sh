# set -e (errexit) POSIX.1-2024 rules and quirks

# Basic failure exits immediately
(
	set -e
	echo before
	false
	echo after
)
echo "basic=$?"

# set +e disables errexit
(
	set -e
	set +e
	false
	echo recovered
)
echo "plus_e=$?"

# Ignored in if / elif conditions
(
	set -e
	if false; then
		echo bad1
	elif false; then
		echo bad2
	else
		echo else_branch
	fi
	echo after_if
)
echo "if=$?"

# Ignored in while / until conditions
(
	set -e
	i=0
	while [ "$i" -lt 2 ]; do
		i=$((i + 1))
	done
	until [ "$i" -eq 0 ]; do
		i=$((i - 1))
	done
	echo "loop_done=$i"
)
echo "loop=$?"

# Ignored on non-last elements of && / ||, active on last element
(
	set -e
	false && echo skipped
	echo after_and_shortcircuit
	false || echo fallback
	echo after_or_fallback
)
echo "andor_ok=$?"

(
	set -e
	true && false
	echo never
)
echo "andor_last_fail=$?"

(
	set -e
	false || false
	echo never
)
echo "or_last_fail=$?"

# Ignored on negated pipeline !
(
	set -e
	! true
	echo after_bang_true
	! false
	echo after_bang_false
)
echo "bang=$?"

# Compound command { ... } does not re-trigger errexit on ignored failure
(
	set -e
	{ false && echo no; }
	echo after_group
)
echo "group=$?"

# Subshell ( ... ) DOES trigger errexit in parent
(
	set -e
	( false && echo no )
	echo never
)
echo "subshell_fail=$?"

# Function called in ignored context inherits ignore
(
	set -e
	f() {
		false
		echo inside_f
	}
	if f; then
		echo f_ok
	fi
	echo after_f_if
)
echo "func_ignored=$?"

# Function called normally exits on internal failure
(
	set -e
	f() {
		false
		echo never_in_f
	}
	f
	echo never_after_f
)
echo "func_fail=$?"

# Function returning non-zero via return triggers errexit
(
	set -e
	g() { return 7; }
	g
	echo never
)
echo "func_return=$?"

# Pipeline: without pipefail only last stage counts
(
	set -e
	false | true
	echo pipe_ok
)
echo "pipe_ok=$?"

(
	set -e
	true | false
	echo never
)
echo "pipe_fail=$?"

# Assignment with failing command substitution triggers errexit
(
	set -e
	x=$(exit 5)
	echo never
)
echo "assign_cmdsub=$?"

# EXIT trap still runs with failing status on errexit
(
	trap 'echo "trap_status=$?"' EXIT
	set -e
	(exit 9)
	echo never
)
echo "trap_rc=$?"
