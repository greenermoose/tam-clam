# Regular POSIX builtins (POSIX.1-2024)

#### cd -L, cd -P, cd -, and pwd
mkdir -p real/sub
ln -s real/sub link
start=$(pwd)
(
	cd -L link
	echo "L=$(basename "$(pwd -L)")"
	echo "P=$(basename "$(pwd -P)")"
	cd - >/dev/null
	[ "$(pwd)" = "$start" ] && echo "back=ok"
)
## STDOUT:
L=link
P=sub
back=ok
## END

#### printf formatting, reuse, %b, and char constants
printf '[%s:%03d]\n' a 7 b 42
printf '%b\n' 'x\ty'
printf '%d\n' "'A"
## STDOUT:
[a:007]
[b:042]
x	y
65
## END

#### test and [ 0..4 arg rules and numeric/string/file primaries
touch f
[ -f f -a ! -d f ] && echo file_ok
[ 10 -gt 2 -a "abc" != "def" ] && echo cmp_ok
[ "" ] || echo empty_false
[ "!" ] && echo single_bang_true
## STDOUT:
file_ok
cmp_ok
empty_false
single_bang_true
## END

#### read with -r, IFS splitting, and custom -d delimiter
printf 'a,b,c d\n' | { IFS=, read -r x y z; echo "$x|$y|$z"; }
printf 'p:q:' | { read -d : u; read -d : v; echo "$u|$v"; }
## STDOUT:
a|b|c d
p|q
## END

#### getopts with clusters, arguments, and silent mode
parse() {
	OPTIND=1
	out=""
	while getopts ":ab:" opt; do
		out="$out$opt:${OPTARG:-};"
	done
	shift $((OPTIND - 1))
	echo "$out|$*"
}
parse -ab val rest
## stdout: a:;b:val;|rest

#### alias expansion, trailing blank chaining, and quote bypass
alias greet='echo hi '
alias who='world'
greet who
'greet' 2>/dev/null || echo bypassed
unalias greet who
## STDOUT:
hi world
bypassed
## END

#### command -v and type
f() { :; }
echo "fn=$(command -v f)"
echo "bi=$(command -v true)"
type if | grep -q 'keyword\|reserved' && echo kw_ok
## STDOUT:
fn=f
bi=true
kw_ok
## END

#### umask octal and symbolic -S
(
	umask 0027
	echo "oct=$(umask)"
	echo "sym=$(umask -S)"
)
## STDOUT:
oct=0027
sym=u=rwx,g=rx,o=
## END

#### kill -l signal name lookup
echo "$(kill -l 15) $(kill -l 143)"
## stdout: TERM TERM
