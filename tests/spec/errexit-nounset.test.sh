# Shell options: set -e, -u, -x, -n, -o/+o, $- (POSIX.1-2024)

#### set -e exits on simple command failure
(set -e; echo before; false; echo after)
echo "rc=$?"
## STDOUT:
before
rc=1
## END

#### set -e ignored in if/while/until conditions, !, and non-last &&/||
(
	set -e
	if false; then echo no; fi
	! true
	false && echo no
	false || echo ok
	{ false && echo no; }
	echo survived
)
echo "rc=$?"
## STDOUT:
ok
survived
rc=0
## END

#### set -e active in subshell and on last element of &&/||
(set -e; (false && true); echo never)
echo "sub=$?"
(set -e; true && false; echo never)
echo "and=$?"
## STDOUT:
sub=1
and=1
## END

#### set -e ignored inside function called in condition, active outside
(
	set -e
	f() { false; echo inside; }
	if f; then echo cond_ok; fi
)
echo "cond_rc=$?"
(
	set -e
	f() { false; echo never; }
	f
	echo never2
)
echo "call_rc=$?"
## STDOUT:
inside
cond_ok
cond_rc=0
call_rc=1
## END

#### set -e runs EXIT trap with failing status
(
	trap 'echo "trap=$?"' EXIT
	set -e
	(exit 11)
)
echo "rc=$?"
## STDOUT:
trap=11
rc=11
## END

#### set -u fails on unset variable, exempts $@/$* and defaults
(set -u; echo "$NO_SUCH_VAR"; echo never) 2>/dev/null
[ $? -ne 0 ] && echo "var=fail"
(set -u; set --; echo "at=<$@> def=${NO_SUCH_VAR:-ok}")
## STDOUT:
var=fail
at=<> def=ok
## END

#### set -u fails on unset arithmetic variable, safe on short-circuit
(set -u; echo $((0 && NO_SUCH_VAR)); echo $((X = 4)); echo $((X + 1)))
(set -u; echo $((NO_SUCH_VAR + 1))) 2>/dev/null
[ $? -ne 0 ] && echo "arith=fail"
## STDOUT:
0
4
5
arith=fail
## END

#### set -x traces commands with PS4 and LINENO
(
	PS4='+L$LINENO: '
	set -x
	a=1
	echo "hi there"
	set +x
) 2>&1
## STDOUT:
+L4: a=1
+L5: echo 'hi there'
hi there
+L6: set +x
## END

#### set -n stops executing subsequent commands
(
	echo before
	set -n
	echo after
)
echo "rc=$?"
## STDOUT:
before
rc=0
## END

#### set +o output is reusable via eval
set -e -u -C -o pipefail
saved=$(set +o)
set +e +u +C +o pipefail
eval "$saved"
case "$-" in
*e*u*|*u*e*) echo "flags=ok" ;;
esac
set -o | grep '^pipefail' | awk '{print $2}'
## STDOUT:
flags=ok
on
## END
