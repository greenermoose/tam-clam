# export, readonly, unset, set -a, set -f.

# export with and without assignment.
export EX1=hello
EX2=world
export EX2
sh -c 'echo "$EX1:$EX2"'

# export -p produces reusable output.
out=$(export -p | grep '^export EX1=')
case "$out" in
	*EX1=*hello*) echo "export-p:ok" ;;
	*) echo "export-p:bad:$out" ;;
esac

# readonly with assignment and -p.
readonly RO1=fixed
echo "ro:$RO1"
rout=$(readonly -p | grep '^readonly RO1=')
case "$rout" in
	*RO1=*fixed*) echo "readonly-p:ok" ;;
	*) echo "readonly-p:bad:$rout" ;;
esac

# unset variable (default and -v).
U1=one U2=two
unset U1
unset -v U2
echo "unset:${U1-gone}:${U2-gone}"

# unset function (-f).
myf() { echo hi; }
myf
unset -f myf
# Redefining or checking absence via subshell.
(myf 2>/dev/null)
echo "unset-f:$?"

# set -a / +a (allexport).
set -a
case "$-" in
	*a*) echo "flag-a:on" ;;
	*) echo "flag-a:off" ;;
esac
AUTO_EX=yes
set +a
NO_EX=no
sh -c 'echo "auto:${AUTO_EX-unset}:${NO_EX-unset}"'

# set -f / +f (noglob).
set -f
case "$-" in
	*f*) echo "flag-f:on" ;;
	*) echo "flag-f:off" ;;
esac
echo *.sh
set +f
case "$-" in
	*f*) echo "flag-f:still-on" ;;
	*) echo "flag-f:off" ;;
esac
