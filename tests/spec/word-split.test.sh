# IFS field splitting and $@ / $* (POSIX.1-2024 XCU 2.6.5)

#### Default IFS trims leading/trailing whitespace and collapses runs
x="   one   two	three  "
set -- $x
echo "$#:$1|$2|$3"
## stdout: 3:one|two|three

#### Non-whitespace IFS delimiter preserves empty fields
IFS=:
x=":a::b:"
set -- $x
printf '%d:' "$#"
printf '<%s>' "$@"
echo
## stdout: 4:<><a><><b>

#### Mixed IFS whitespace and non-whitespace delimiter
IFS=" :"
x="  : a : : b :  "
set -- $x
printf '%d:' "$#"
printf '<%s>' "$@"
echo
## stdout: 4:<><a><><b>

#### Empty IFS disables field splitting
IFS=""
x="a b c"
set -- $x
echo "$#:$1"
## stdout: 1:a b c

#### Unset IFS uses default space-tab-newline
unset IFS
x="  a	b
c  "
set -- $x
echo "$#:$1|$2|$3"
## stdout: 3:a|b|c

#### Quoted "$@" preserves separate words and produces 0 words when empty
set -- "a b" "" "c"
printf '%d:' "$#"
printf '<%s>' "$@"
echo
set --
printf 'empty=%d\n' "$(sh -c 'echo $#' _ "$@")"
## STDOUT:
3:<a b><><c>
empty=0
## END

#### Quoted "$*" joins with first character of IFS (or nothing when IFS empty)
set -- a b c
IFS=:
echo "colon:<$*>"
IFS=""
echo "empty:<$*>"
unset IFS
echo "unset:<$*>"
## STDOUT:
colon:<a:b:c>
empty:<abc>
unset:<a b c>
## END

#### Field splitting through unquoted ${x:-word}
unset x
set -- ${x:-one two}
echo "$#:$1|$2"
## stdout: 2:one|two
