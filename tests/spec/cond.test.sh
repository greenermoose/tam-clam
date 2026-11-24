## [[ ... ]] conditional compound command (0.3.1)

#### No word splitting or globbing inside [[ ]]
x="a b c"
touch foo.txt
[[ $x == "a b c" ]] && echo split-safe
[[ foo* == foo* ]] && echo glob-safe
## N-I dash
## N-I ash
## STDOUT:
split-safe
glob-safe
## END

#### Pattern matching with == and != (quoted vs unquoted)
s="tam-clam-0.3.1"
[[ $s == tam-* ]] && echo m1
[[ $s == "tam-*" ]] || echo m2
[[ $s != *.txt ]] && echo m3
[[ $s == tam-clam-[0-9].[0-9].[0-9] ]] && echo m4
## N-I dash
## N-I ash
## STDOUT:
m1
m2
m3
m4
## END

#### Logical &&, ||, !, grouping, and short-circuit
x=0
[[ 1 -eq 1 || $((x=1)) -eq 1 ]]
echo "after-or=$x"
[[ 1 -eq 2 && $((x=2)) -eq 2 ]]
echo "after-and=$x"
[[ ! (1 -eq 2 || 3 -lt 2) && (a == a) ]] && echo grouped
## N-I dash
## N-I ash
## STDOUT:
after-or=0
after-and=0
grouped
## END

#### Lexicographical < and > inside [[ ]]
[[ apple < banana ]] && echo lt
[[ zebra > mango ]] && echo gt
[[ alpha > beta ]] || echo not-gt
## N-I dash
## N-I ash
## STDOUT:
lt
gt
not-gt
## END

#### Arithmetic evaluation in -eq/-lt/-gt
a=10
b=20
[[ a+5 -eq 15 ]] && echo eq
[[ b-a -ge 10 ]] && echo ge
[[ a*2 -lt b+1 ]] && echo lt
## N-I dash
## N-I ash
## STDOUT:
eq
ge
lt
## END

#### Unary -v (scalar and array) and -o (shell options)
a=1
arr=(x y)
unset missing
[[ -v a ]] && echo var-set
[[ ! -v missing ]] && echo var-unset
[[ -v arr[1] ]] && echo elem-set
[[ ! -v arr[5] ]] && echo elem-unset
set -f
[[ -o noglob ]] && echo opt-on
[[ ! -o errexit ]] && echo opt-off
## N-I dash
## N-I ash
## STDOUT:
var-set
var-unset
elem-set
elem-unset
opt-on
opt-off
## END

#### Regex matching =~ with BASH_REMATCH captures
s="release 2026-11-24 v0.3.1"
if [[ $s =~ ([0-9]{4})-([0-9]{2})-([0-9]{2}) ]]; then
    printf '%s|%s|%s|%s\n' "${BASH_REMATCH[0]}" "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" "${BASH_REMATCH[3]}"
fi
## N-I dash
## N-I ash
## stdout: 2026-11-24|2026|11|24

#### Regex matching =~ with alternation, quoting, and mismatch reset
[[ "foo.bar" =~ "foo.bar" ]] && echo lit-ok
[[ "fooxbar" =~ "foo.bar" ]] || echo lit-no
[[ "cat" =~ ^(cat|dog)$ ]] && echo alt-ok:${BASH_REMATCH[1]}
[[ "bird" =~ ^(cat|dog)$ ]] || echo mismatch:${#BASH_REMATCH[@]}
## N-I dash
## N-I ash
## STDOUT:
lit-ok
lit-no
alt-ok:cat
mismatch:0
## END

#### Regex syntax error returns status 2
[[ abc =~ [unclosed ]]
echo status=$?
## N-I dash
## N-I ash
## stdout: status=2

#### [[ ]] inside a shell function
check_ver() {
    [[ $1 =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] && echo "${BASH_REMATCH[1]}.${BASH_REMATCH[2]}"
}
check_ver 0.3.1
## N-I dash
## N-I ash
## stdout: 0.3
