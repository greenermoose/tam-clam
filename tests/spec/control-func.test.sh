# Compound commands and functions (POSIX.1-2024 XCU 2.9.4, 2.9.5)

#### if / elif / else
for n in 1 2 3; do
	if [ "$n" -eq 1 ]; then
		echo one
	elif [ "$n" -eq 2 ]; then
		echo two
	else
		echo other
	fi
done
## STDOUT:
one
two
other
## END

#### while and until loops with break and continue
i=0
out=""
while [ "$i" -lt 6 ]; do
	i=$((i + 1))
	[ "$i" -eq 2 ] && continue
	[ "$i" -eq 5 ] && break
	out="$out$i"
done
echo "while=$out"

j=0
until [ "$j" -eq 3 ]; do
	j=$((j + 1))
done
echo "until=$j"
## STDOUT:
while=134
until=3
## END

#### Nested break 2 exits both loops
res=""
for a in 1 2; do
	for b in x y; do
		res="$res$a$b,"
		[ "$b" = x ] && break 2
	done
done
echo "$res"
## stdout: 1x,

#### for without 'in' iterates over positional parameters
set -- alpha "beta gamma" delta
for x; do
	echo "<$x>"
done
## STDOUT:
<alpha>
<beta gamma>
<delta>
## END

#### Grouping { } shares state; subshell ( ) isolates state
v=outer
{ v=grouped; }
echo "after_group=$v"
( v=subshell; echo "in_sub=$v" )
echo "after_sub=$v"
## STDOUT:
after_group=grouped
in_sub=subshell
after_sub=grouped
## END

#### Functions have local positional parameters and return status
set -- outer1 outer2
f() {
	echo "fn:$#:$1:$2"
	return 7
}
f inner1
echo "rc=$? outer:$#:$1:$2"
## STDOUT:
fn:1:inner1:
rc=7 outer:2:outer1:outer2
## END

#### Function recursion bounded by FUNCNEST
(
	FUNCNEST=5
	r() { r; }
	r
) 2>/dev/null
## status: nonzero
