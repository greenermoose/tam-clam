## local and full declare (0.3.1)

#### local shadows outer variable and restores on return
x=outer
f() {
    local x=inner
    echo "in:$x"
}
f
echo "out:$x"
## N-I dash
## N-I ash
## STDOUT:
in:inner
out:outer
## END

#### Nested dynamic scoping with local
x=global
g() { echo "g:$x"; x=from-g; }
f() {
    local x=from-f
    g
    echo "f:$x"
}
f
echo "top:$x"
## N-I dash
## N-I ash
## STDOUT:
g:from-f
f:from-g
top:global
## END

#### local without value starts unset and hides outer
x=outer
f() {
    local x
    echo "in:${x-UNSET}"
    x=set-in-f
    echo "in2:$x"
}
f
echo "out:$x"
## N-I dash
## N-I ash
## STDOUT:
in:UNSET
in2:set-in-f
out:outer
## END

#### declare inside function is local by default; declare -g is global
a=1
b=2
f() {
    declare a=10
    declare -g b=20
    declare -g c=30
    echo "in:$a:$b:$c"
}
f
echo "out:$a:$b:$c"
## N-I dash
## N-I ash
## STDOUT:
in:10:20:30
out:1:20:30
## END

#### local with compound array assignment
arr=(outer1 outer2)
f() {
    local arr=(inner1 inner2 inner3)
    echo "in:${#arr[@]}:${arr[*]}"
}
f
echo "out:${#arr[@]}:${arr[*]}"
## N-I dash
## N-I ash
## STDOUT:
in:3:inner1 inner2 inner3
out:2:outer1 outer2
## END

#### local outside function fails
local x=1
## status: 1
## N-I dash
## N-I ash

#### declare -i integer attribute and += arithmetic
declare -i n=2+3
echo "$n"
n=n*4
echo "$n"
n+=5
echo "$n"
declare +i n
n+=1
echo "$n"
## N-I dash
## N-I ash
## STDOUT:
5
20
25
251
## END

#### declare -l and -u case transformation
declare -l lo=HeLLo
declare -u up=HeLLo
echo "$lo|$up"
lo=WoRLd
up=WoRLd
echo "$lo|$up"
declare +l lo
lo=KeEp
echo "$lo"
## N-I dash
## N-I ash
## STDOUT:
hello|HELLO
world|WORLD
KeEp
## END

#### declare -F lists function names
zeta() { :; }
alpha() { :; }
declare -F alpha
declare -F nope
echo status=$?
## N-I dash
## N-I ash
## STDOUT:
alpha
status=1
## END
