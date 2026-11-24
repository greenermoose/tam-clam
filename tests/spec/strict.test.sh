## Oils-named strict modes and `set -o strict` (0.3.3)

#### strict_errexit rejects function call in if condition
set -e -o strict_errexit
f() { false; echo unreachable; }
if f; then echo yes; fi
echo after
## status: 1
## N-I bash
## N-I dash
## N-I ash
## N-I mksh
## stdout-json: ""

#### strict_errexit rejects compound command in && LHS
set -e -o strict_errexit
{ true; } && echo ok
## status: 1
## N-I bash
## N-I dash
## N-I ash
## N-I mksh
## stdout-json: ""

#### strict_errexit allows simple commands, pipelines, !, and [[ ]]
set -e -o strict_errexit
x=hello
if [[ $x == h* ]] && ! false; then
    echo allowed
fi
## N-I bash
## N-I dash
## N-I ash
## N-I mksh
## stdout: allowed

#### command_sub_errexit fails command on failing $(...)
set -o command_sub_errexit
echo $(exit 4)
echo status=$?
f() { local v=$(exit 9); }
f
echo fstatus=$?
## N-I bash
## N-I dash
## N-I ash
## N-I mksh
## STDOUT:
status=4
fstatus=9
## END

#### command_sub_errexit with set -e aborts script
set -e -o command_sub_errexit
echo $(false)
echo should-not-run
## status: 1
## N-I bash
## N-I dash
## N-I ash
## N-I mksh
## stdout-json: ""

#### simple_word_eval prevents splitting and globbing of unquoted expansions
touch a.txt b.txt
set -o simple_word_eval
s="one two"
empty=""
pat="*.txt"
set -- $s $empty $pat *.txt
printf 'n=%d [%s][%s][%s][%s][%s]\n' "$#" "$1" "$2" "$3" "$4" "$5"
## N-I bash
## N-I dash
## N-I ash
## N-I mksh
## stdout: n=5 [one two][][*.txt][a.txt][b.txt]

#### strict_argv rejects empty command names but allows assignments
set -o strict_argv
x=ok
echo "$x"
empty=""
$empty
echo status=$?
## N-I bash
## N-I dash
## N-I ash
## N-I mksh
## STDOUT:
ok
status=1
## END

#### set -o strict enables all strict options together
set -o strict
[[ -o strict && -o errexit && -o nounset && -o pipefail && -o strict_errexit && -o command_sub_errexit && -o simple_word_eval && -o strict_argv ]] && echo all-on
set +o strict
[[ ! -o strict && ! -o errexit ]] && echo all-off
## N-I bash
## N-I dash
## N-I ash
## N-I mksh
## STDOUT:
all-on
all-off
## END
