## Indexed and associative arrays (0.3.0, bash syntax)

#### Indexed array literal and element access
a=(one two three)
printf '%s|%s|%s|%s\n' "$a" "${a[0]}" "${a[1]}" "${a[2]}"
## N-I dash
## N-I ash
## stdout: one|one|two|three

#### Multiline indexed array literal with comments
a=(
    first # comment
    "second item"
    third
)
printf '%s\n' "${a[@]}"
## N-I dash
## N-I ash
## STDOUT:
first
second item
third
## END

#### Empty array and length
a=()
printf 'len=%s words=%s\n' "${#a[@]}" "$#"
set -- "${a[@]}"
printf 'after=%s\n' "$#"
## N-I dash
## N-I ash
## STDOUT:
len=0 words=0
after=0
## END

#### Quoted @ vs * and IFS joining
a=("a b" "" "c d")
set -- "${a[@]}"
printf 'at=%s [%s][%s][%s]\n' "$#" "$1" "$2" "$3"
IFS=:
printf 'star=[%s]\n' "${a[*]}"
## N-I dash
## N-I ash
## STDOUT:
at=3 [a b][][c d]
star=[a b::c d]
## END

#### Sparse indexed arrays, keys, and lengths
a=([2]=two [10]=ten)
printf 'len=%s elem2=%s elem2len=%s\n' "${#a[@]}" "${a[2]}" "${#a[2]}"
printf 'keys=%s\n' "${!a[*]}"
printf 'vals=%s\n' "${a[*]}"
## N-I dash
## N-I ash
## STDOUT:
len=2 elem2=two elem2len=3
keys=2 10
vals=two ten
## END

#### Negative indices on indexed arrays
a=(alpha beta gamma)
printf '%s %s %s\n' "${a[-1]}" "${a[-2]}" "${a[-3]}"
a[-1]=omega
printf '%s\n' "${a[*]}"
## N-I dash
## N-I ash
## STDOUT:
gamma beta alpha
alpha beta omega
## END

#### Negative index on sparse array
a=([5]=five)
printf 'm1=%s m2=%s\n' "${a[-1]}" "${a[-2]}"
## N-I dash
## N-I ash
## stdout: m1=five m2=

#### Out-of-bounds negative index fails
a=(x y)
echo "${a[-5]}"
## N-I dash
## N-I ash
## OK bash status: 0
## status: nonzero
## stderr-contains: bad array subscript

#### Append to array, element, scalar, and scalar-to-array
a=(one two)
a+=(three "four five")
printf 'arr=%s len=%s\n' "${a[3]}" "${#a[@]}"
a[1]+=TWO
printf 'elem=%s\n' "${a[1]}"
s=hello
s+=world
printf 'scalar=%s\n' "$s"
t=first
t+=(second third)
printf 's2a=%s|%s|%s\n' "${t[0]}" "${t[1]}" "${t[2]}"
## N-I dash
## N-I ash
## STDOUT:
arr=four five len=4
elem=twoTWO
scalar=helloworld
s2a=first|second|third
## END

#### Subscripted assignment and arithmetic subscripts
i=2
a[i+1]=three
a[0]=zero
printf '%s %s len=%s\n' "${a[0]}" "${a[3]}" "${#a[@]}"
## N-I dash
## N-I ash
## stdout: zero three len=2

#### Associative array declare -A, lookup, append, keys, length
declare -A m=([apple]=red [banana]=yellow)
m[cherry]=dark
m[apple]+="-sweet"
m+=([date]=brown)
printf 'apple=%s cherry=%s date=%s len=%s\n' "${m[apple]}" "${m[cherry]}" "${m[date]}" "${#m[@]}"
printf 'keys=%s\n' "$(printf '%s\n' "${!m[@]}" | sort | tr '\n' ',')"
## N-I dash
## N-I ash
## STDOUT:
apple=red-sweet cherry=dark date=brown len=4
keys=apple,banana,cherry,date,
## END

#### Associative array keys with spaces and variables
declare -A m
k="two words"
m["$k"]=found
m["a b"]=ok
printf '%s|%s\n' "${m[$k]}" "${m["a b"]}"
## N-I dash
## N-I ash
## stdout: found|ok

#### Unset array element and whole array
a=(zero one two three)
unset 'a[1]'
printf 'keys=%s vals=%s len=%s\n' "${!a[*]}" "${a[*]}" "${#a[@]}"
unset 'a[@]'
printf 'after=%s\n' "${#a[@]}"
declare -A m=([x]=1 [y]=2)
unset 'm[x]'
printf 'assoc=%s len=%s\n' "${!m[*]}" "${#m[@]}"
## N-I dash
## N-I ash
## STDOUT:
keys=0 2 3 vals=zero two three len=3
after=0
assoc=y len=1
## END

#### Parameter operators on array elements and element-wise trim
a=(hello.txt world.txt "")
printf 'def=%s\n' "${a[2]:-fallback}"
printf 'asgn=%s val=%s\n' "${a[5]:=assigned}" "${a[5]}"
printf 'trim=%s\n' "${a[0]%.txt}"
b=(foo.c bar.c baz.c)
printf 'all=%s\n' "${b[*]%.c}"
## N-I dash
## N-I ash
## STDOUT:
def=fallback
asgn=assigned val=assigned
trim=hello
all=foo bar baz
## END

#### Arrays in arithmetic expansion
a=(10 20 30)
printf '%s\n' "$(( a[0] + a[1] * 2 ))"
printf '%s\n' "$(( a[1] += 5, a[1] ))"
printf '%s %s\n' "$(( ++a[0] ))" "$(( a[2]++ ))"
printf '%s %s %s\n' "${a[0]}" "${a[1]}" "${a[2]}"
## N-I dash
## N-I ash
## STDOUT:
50
25
11 30
11 25 31
## END

#### Arrays inside functions (AST deep clone)
f() {
    a=(x y)
    a+=(z)
    declare -A m=([k]="$1")
    printf '%s|%s|%s\n' "${a[*]}" "${#a[@]}" "${m[k]}"
}
f hello
f world
## N-I dash
## N-I ash
## STDOUT:
x y z|3|hello
x y z|3|world
## END

#### readonly array prevents modification
readonly a=(one two)
a[0]=changed
## N-I dash
## N-I ash
## status: nonzero
## stderr-contains: readonly

#### Cannot convert between indexed and associative array
declare -A m=([a]=1)
declare -a m
## N-I dash
## N-I ash
## status: nonzero

#### set -u with empty declared array vs unset variable
set -u
a=()
printf 'empty=%s\n' "${#a[@]}"
set -- "${a[@]}"
printf 'expanded=%s\n' "$#"
## N-I dash
## N-I ash
## STDOUT:
empty=0
expanded=0
## END

#### read -a splits input into indexed array
printf 'one two   three\n' | {
    read -a words
    printf 'len=%s [%s][%s][%s]\n' "${#words[@]}" "${words[0]}" "${words[1]}" "${words[2]}"
}
## N-I dash
## N-I ash
## stdout: len=3 [one][two][three]

#### Word splitting and globbing inside compound array literal
touch f1.dat f2.dat
x="p q"
a=($x *.dat "keep space")
printf '%s\n' "${a[@]}"
## N-I dash
## N-I ash
## STDOUT:
p
q
f1.dat
f2.dat
keep space
## END

#### declare -p round-trip for indexed and associative arrays
a=([0]="hello world" [5]="it's ok")
decl=$(declare -p a)
unset a
eval "$decl"
printf '%s|%s|%s\n' "${#a[@]}" "${a[0]}" "${a[5]}"
declare -A m=([k1]="v 1" [k2]="v'2")
declm=$(declare -p m)
unset m
eval "$declm"
printf '%s|%s|%s\n' "${#m[@]}" "${m[k1]}" "${m[k2]}"
## N-I dash
## N-I ash
## STDOUT:
2|hello world|it's ok
2|v 1|v'2
## END
