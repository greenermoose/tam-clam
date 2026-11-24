# Parameter expansion and tilde expansion (POSIX.1-2024 XCU 2.6.1, 2.6.2)

#### Plain and braced variable expansion
a=foo
echo "$a ${a}bar ${unset_var}end"
## stdout: foo foobar end

#### Length expansion ${#var} and ${#@}
s=hello
set -- a b c d
echo "${#s} ${#unset_var} ${#@} ${#*}"
## stdout: 5 0 4 4

#### Default value - vs :-
empty=""
echo "${unset_v-d1}|${empty-d2}|${unset_v:-d3}|${empty:-d4}"
## stdout: d1||d3|d4

#### Assign default = vs :=
empty=""
echo "${u1=val1}|${u1}|${empty:=val2}|${empty}"
## stdout: val1|val1|val2|val2

#### Alternate value + vs :+
empty=""
x=hi
echo "${unset_v+a}|${empty+b}|${empty:+c}|${x:+d}"
## stdout: |b||d

#### Error expansion :? exits non-interactive shell
(echo "${unset_v:?custom msg}"; echo never) 2>/dev/null
echo "rc=$?"
## status: 0
## stdout: rc=1

#### Prefix and suffix trimming #, ##, %, %%
p="a/b/c.tar.gz"
echo "${p#*/}|${p##*/}|${p%.*}|${p%%.*}"
## stdout: b/c.tar.gz|c.tar.gz|a/b/c.tar|a/b/c

#### Nested parameter expansion
unset a
b=fallback
echo "${a:-${b}}"
## stdout: fallback

#### Positional parameters beyond 9 require braces
set -- 1 2 3 4 5 6 7 8 9 ten eleven
echo "$10 ${10} ${11}"
## stdout: 10 ten eleven

#### Tilde expansion in words and assignments
HOME=/tmp
x=~/bin:~/lib
echo "~ $x" ~/doc
## stdout: ~ /tmp/bin:/tmp/lib /tmp/doc

#### Quoted tilde does not expand
HOME=/tmp
echo "~" '~' \~/a
## stdout: ~ ~ ~/a

#### LINENO increments across lines
echo "$LINENO"
echo "$LINENO"
## STDOUT:
1
2
## END
