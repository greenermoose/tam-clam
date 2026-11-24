# Pathname expansion (globbing) and case matching (POSIX.1-2024 XCU 2.13)

#### Basic *, ?, and [...] globs
touch alpha.txt beta.txt gamma.log .hidden
echo *.txt
echo ????.txt
echo [ab]*.txt
## STDOUT:
alpha.txt beta.txt
beta.txt
alpha.txt beta.txt
## END

#### Leading dot is not matched by * or ?
touch .secret visible
echo *
echo .*
## STDOUT:
visible
. .. .secret
## END
## OK bash STDOUT:
visible
.secret
## END

#### Unmatched glob remains literal
echo no_such_file_*.xyz
## stdout: no_such_file_*.xyz

#### Quoted glob metacharacters do not expand
touch a.txt b.txt
echo '*.txt' "*.txt" \*.txt
## stdout: *.txt *.txt *.txt

#### set -f disables pathname expansion
touch a.txt b.txt
set -f
echo *.txt
set +f
echo *.txt
## STDOUT:
*.txt
a.txt b.txt
## END

#### case matching with alternation, negation, and character classes
for w in foo bar 123 _x; do
	case "$w" in
	foo|baz) echo "$w:fb" ;;
	[[:digit:]]*) echo "$w:num" ;;
	[!a-z]*) echo "$w:nonlower" ;;
	*) echo "$w:other" ;;
	esac
done
## STDOUT:
foo:fb
bar:other
123:num
_x:nonlower
## END

#### case with empty clause body succeeds with status 0
false
case x in
x) ;;
esac
echo "status=$?"
## stdout: status=0

#### Linear-time pattern matching on adversarial backtracking input
s="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab"
case "$s" in
*a*a*a*a*a*a*a*a*a*a*a*a*c) echo matched ;;
*) echo no_match ;;
esac
## stdout: no_match
