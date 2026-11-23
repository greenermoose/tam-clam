#!/bin/sh
# Script tests for tam-clam.
#
#   sh tests/run-scripts.sh build/tam-clam
#
# Each tests/scripts/NAME.sh runs with its folder as the working
# directory and standard input from /dev/null. Its standard output must
# match NAME.out, and its exit status NAME.status (0 when there is no
# .status file). Each script also runs under a reference shell, which
# must give the same output and status: dash when it is installed, else
# bash --posix. Set TC_REF to choose another, or TC_REF=none to skip.
#
# tests/refuse/ holds valid shell that tam-clam does not support yet. It
# must be refused with status 2 and no wrong output, so those scripts are
# not compared with a reference shell.
#
# Then come checks of the ways tam-clam is started.
set -u
here=$(cd "$(dirname "$0")" && pwd)
shell=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
pass=0
fail=0

if [ -n "${TC_REF+set}" ]; then
	ref=$TC_REF
elif command -v dash >/dev/null 2>&1; then
	ref=dash
elif command -v bash >/dev/null 2>&1; then
	ref='bash --posix'
else
	ref=none
fi

# check NAME SHELL DIR SCRIPT: run SCRIPT under SHELL and compare.
check() {
	name=$1 sh=$2 dir=$3 script=$4
	base=${script%.sh}
	want_status=0
	[ -f "$dir/$base.status" ] && want_status=$(cat "$dir/$base.status")
	(cd "$dir" && $sh "$script" </dev/null >"$tmp/out" 2>"$tmp/err")
	status=$?
	if ! cmp -s "$tmp/out" "$dir/$base.out"; then
		echo "FAIL $name: output differs"
		diff "$dir/$base.out" "$tmp/out" | sed 's/^/    /'
		fail=$((fail + 1))
	elif [ "$status" != "$want_status" ]; then
		echo "FAIL $name: status $status, want $want_status"
		sed 's/^/    /' "$tmp/err"
		fail=$((fail + 1))
	else
		pass=$((pass + 1))
	fi
}

for f in "$here"/scripts/*.sh; do
	s=$(basename "$f")
	check "scripts/$s" "$shell" "$here/scripts" "$s"
	[ "$ref" = none ] || check "scripts/$s under $ref" "$ref" "$here/scripts" "$s"
done
for f in "$here"/refuse/*.sh; do
	s=$(basename "$f")
	check "refuse/$s" "$shell" "$here/refuse" "$s"
done

# expect NAME WANT_STATUS WANT_OUTPUT COMMAND...: run one start-up check.
expect() {
	name=$1 want_status=$2 want_out=$3
	shift 3
	out=$("$@" 2>"$tmp/err")
	status=$?
	if [ "$status" != "$want_status" ] || [ "$out" != "$want_out" ]; then
		echo "FAIL start: $name: status $status (want $want_status)," \
			"output '$out' (want '$want_out')"
		sed 's/^/    /' "$tmp/err"
		fail=$((fail + 1))
	else
		pass=$((pass + 1))
	fi
}

printf 'echo one\necho two\n' >"$tmp/stdin"
expect '-c runs its string' 0 'a b' "$shell" -c 'echo a b'
expect '-c keeps the last status' 1 '' "$shell" -c 'false'
expect '-c needs a string' 2 '' "$shell" -c
expect 'commands from standard input' 0 'one
two' sh -c '"$1" <"$2"' - "$shell" "$tmp/stdin"
expect '-s reads standard input' 0 'one
two' sh -c '"$1" -s <"$2"' - "$shell" "$tmp/stdin"
# Standard input is read a byte at a time, so a command can read the
# line after it.
expect 'commands share standard input' 0 'hello
after' sh -c 'printf "dd bs=1 count=6 status=none\nhello\necho after\n" | "$1"' - "$shell"
expect 'a missing script' 127 '' "$shell" "$tmp/no-such-script"
expect 'a directory as a script' 126 '' "$shell" "$tmp"
expect 'an unknown option' 2 '' "$shell" -Z
expect '--version' 0 "tam-clam $(cat "$here/../VERSION")" "$shell" --version
expect 'a syntax error stops the script' 2 'before' \
	"$shell" -c 'echo before
echo a ; ; echo b
echo after'
"$shell" -c nosuch myname 2>"$tmp/err"
if grep -q '^myname: line 1: nosuch: command not found$' "$tmp/err"; then
	pass=$((pass + 1))
else
	echo "FAIL start: -c NAME sets the name in messages"
	sed 's/^/    /' "$tmp/err"
	fail=$((fail + 1))
fi

# Positional parameters across -c, script, and -s.
expect '-c positional parameters' 0 'myname:2:alpha:beta' \
	"$shell" -c 'echo "$0:$#:$1:$2"' myname alpha beta
expect '-c "$@" preserves words' 0 '<a b><c>' \
	"$shell" -c 'printf "<%s>" "$@"' myname 'a b' c
expect '-c "$@" with 0 args produces 0 words' 0 '0' \
	"$shell" -c 'sh -c "echo \$#" _ "$@"' myname
expect '-c "$*" joins with IFS[0]' 0 'a:b:c' \
	"$shell" -c 'IFS=:; echo "$*"' myname a b c
expect '-c ${10} positional parameter' 0 'ten' \
	"$shell" -c 'echo "${10}"' myname 1 2 3 4 5 6 7 8 9 ten
printf 'echo "$#:$1:$2"\n' >"$tmp/pos.sh"
expect 'script positional parameters' 0 '2:foo:bar' \
	"$shell" "$tmp/pos.sh" foo bar
expect '-s positional parameters' 0 '2:x:y' \
	sh -c '"$1" -s x y <"$2"' - "$shell" "$tmp/pos.sh"

# 0.0.4 / 0.1.0: function recursion limit (FUNCNEST and default 1000), parser nesting limit,
# checked 64-bit arithmetic overflow, division by zero, and $(...) nesting limit.
expect 'FUNCNEST=10 stops runaway recursion' 1 '' \
	"$shell" -c 'FUNCNEST=10; f() { f; }; f'
expect 'default recursion limit stops runaway recursion' 1 '' \
	"$shell" -c 'f() { f; }; f'
expect 'parser nesting limit stops deep nesting' 2 '' \
	"$shell" -c "$(awk 'BEGIN { for (i=0;i<150;i++) printf "{ "; printf ":; "; for (i=0;i<150;i++) printf "}; "; }')"
expect 'cmdsub nesting limit stops deep $(...)' 2 '' \
	"$shell" -c "$(awk 'BEGIN { for (i=0;i<150;i++) printf "$( "; printf "echo x"; for (i=0;i<150;i++) printf " )"; }')"
expect 'arithmetic 64-bit overflow is an error' 1 '' \
	"$shell" -c 'echo $((9223372036854775807 + 1))'
expect 'arithmetic division by zero is an error' 1 '' \
	"$shell" -c 'echo $((1 / 0))'

echo "run-scripts: $pass passed, $fail failed (reference shell: $ref)"
[ "$fail" -eq 0 ]
