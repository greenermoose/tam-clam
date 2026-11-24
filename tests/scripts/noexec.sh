# set -n (noexec), set -o / set +o, and $-

# set -o / set +o round-trip
(
	set -e -u -C -f -o pipefail
	case "$-" in
	*e*) echo "has_e=yes" ;;
	esac
	case "$-" in
	*u*) echo "has_u=yes" ;;
	esac
	case "$-" in
	*C*) echo "has_C=yes" ;;
	esac
	case "$-" in
	*f*) echo "has_f=yes" ;;
	esac
	saved=$(set +o)
	set +e +u +C +f +o pipefail
	eval "$saved"
	set -o | grep '^pipefail' | awk '{print $1 ":" $2}'
	set -o | grep '^errexit' | awk '{print $1 ":" $2}'
)

# set -n stops executing subsequent commands in the subshell
(
	echo before_n
	set -n
	echo after_n_same_block
)
echo "after_n_rc=$?"
