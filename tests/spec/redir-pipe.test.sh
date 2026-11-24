# Redirections, here-documents, pipelines, and lists (POSIX.1-2024 XCU 2.7, 2.9)

#### Output, append, and input redirection
echo one >f
echo two >>f
cat <f
## STDOUT:
one
two
## END

#### Left-to-right redirection ordering (2>&1 vs 1>f 2>&1)
{ echo out; echo err >&2; } >both 2>&1
cat both
## STDOUT:
out
err
## END

#### Closing a descriptor with >&-
echo hi >&- 2>/dev/null
## status: nonzero

#### Here-document with expansion vs quoted delimiter
x=42
cat <<EOF
val=$x
EOF
cat <<'EOF'
val=$x
EOF
## STDOUT:
val=42
val=$x
## END

#### Tab-stripping here-document <<-
cat <<-EOF
	indented
		more
	EOF
## STDOUT:
indented
more
## END

#### Pipeline passes stdout to stdin and returns rightmost status
printf 'c\na\nb\n' | sort | tr '\n' ','
echo
false | true
echo "rc1=$?"
true | false
echo "rc2=$?"
## STDOUT:
a,b,c,
rc1=0
rc2=1
## END

#### Pipeline negation !
! false
echo "nf=$?"
! true
echo "nt=$?"
! (exit 5)
echo "n5=$?"
## STDOUT:
nf=0
nt=1
n5=0
## END

#### AND-OR lists are left-associative
false && echo a || echo b
true && echo c || echo d
false || echo e && echo f
## STDOUT:
b
c
e
f
## END

#### set -C (noclobber) blocks > on regular file, allows >| and /dev/null
set -C
echo a >f
echo b >f 2>/dev/null
echo "blocked=$? val=$(cat f)"
echo c >|f
echo "forced=$? val=$(cat f)"
echo d >/dev/null
echo "devnull=$?"
## STDOUT:
blocked=1 val=a
forced=0 val=c
devnull=0
## END

#### set -o pipefail returns rightmost non-zero status (POSIX.1-2024)
set -o pipefail
(exit 3) | (exit 17) | true
echo "rc=$?"
true | true
echo "ok=$?"
## STDOUT:
rc=17
ok=0
## END
