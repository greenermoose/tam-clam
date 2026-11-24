## Process substitution <(...) and >(...) (0.3.2)

#### Basic input process substitution
cat <(printf 'alpha\nbeta\n')
## N-I dash
## N-I ash
## STDOUT:
alpha
beta
## END

#### Two simultaneous input process substitutions
paste -d, <(printf 'a\nb\n') <(printf '1\n2\n')
## N-I dash
## N-I ash
## STDOUT:
a,1
b,2
## END

#### Process substitution as redirection target for compound command
acc=""
while IFS= read -r item; do
    acc="${acc}<${item}>"
done < <(printf 'one\ntwo\nthree\n')
echo "$acc"
## N-I dash
## N-I ash
## stdout: <one><two><three>

#### Output process substitution >(...)
printf 'hello\nworld\n' | tee >(tr a-z A-Z > out.txt) >/dev/null
cat out.txt
## N-I dash
## N-I ash
## STDOUT:
HELLO
WORLD
## END

#### Process substitution inside a shell function
compare_pair() {
    paste -d= <(printf '%s\n' "$1") <(printf '%s\n' "$2")
}
compare_pair key val
## N-I dash
## N-I ash
## stdout: key=val

#### Quoted process substitution stays literal
printf '%s %s\n' "<(echo a)" '>(cat)'
## N-I dash
## N-I ash
## stdout: <(echo a) >(cat)
