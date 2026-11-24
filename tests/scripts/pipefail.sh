# set -o pipefail (POSIX.1-2024)

# Default: pipefail is off
false | true
echo "default=$?"

# Enabled: rightmost non-zero exit status wins
set -o pipefail
false | true
echo "fail_true=$?"

(exit 3) | (exit 42) | true
echo "rightmost=$?"

true | true | true
echo "all_ok=$?"

# Negation ! inverts the pipefail status
! (false | true)
echo "bang_fail=$?"

! (true | true)
echo "bang_ok=$?"

# Combined with set -e
(
	set -e
	(exit 6) | true
	echo never
)
echo "pipefail_e=$?"

# Turning pipefail back off
set +o pipefail
false | true
echo "off=$?"
