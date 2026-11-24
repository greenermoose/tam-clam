# Special builtins, traps, and background jobs (POSIX.1-2024 XCU 2.14)

#### set -- and shift
set -- a b c d
shift 2
echo "$#:$1:$2"
## stdout: 2:c:d

#### eval executes constructed command string
cmd='echo hello'
eval "$cmd world"
## stdout: hello world

#### dot (.) sources file in current shell and supports return
cat >helper.sh <<'EOF'
sourced_var=from_dot
return 4
sourced_var=unreachable
EOF
. ./helper.sh
echo "rc=$? var=$sourced_var"
## stdout: rc=4 var=from_dot

#### export makes variable visible to child processes
export EX_VAR=hello_child
sh -c 'echo "$EX_VAR"'
## stdout: hello_child

#### readonly prevents reassignment and unset
(readonly RO=1; RO=2; echo never) 2>/dev/null
[ $? -ne 0 ] && echo "ro_assign=fail"
(readonly RO=1; unset RO; echo never) 2>/dev/null
[ $? -ne 0 ] && echo "ro_unset=fail"
## STDOUT:
ro_assign=fail
ro_unset=fail
## END

#### Special builtin prefix assignments persist; regular builtins do not
X=before
X=persisted :
echo "after_colon=$X"
X=temp true
echo "after_true=$X"
## STDOUT:
after_colon=persisted
after_true=persisted
## END

#### command suppresses special builtin fatal exit
command shift 99 2>/dev/null
echo "survived=$?"
## stdout: survived=1

#### trap EXIT runs at shell exit and is reset in subshells
(
	trap 'echo outer_exit' EXIT
	(echo in_sub)
	echo after_sub
)
## STDOUT:
in_sub
after_sub
outer_exit
## END

#### Background command &, $!, and wait
(exit 13) &
pid=$!
wait "$pid"
echo "waited=$?"
## stdout: waited=13
