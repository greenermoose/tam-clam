# Quoting, comments, line continuation, and $'...' (POSIX.1-2024 XCU 2.2)

#### Single quotes preserve everything literally
echo '$HOME `echo x` \ "a"'
## stdout: $HOME `echo x` \ "a"

#### Double quotes expand $, preserve spaces and non-special backslashes
x=world
echo "hello   $x \a \" \\ \$"
## stdout: hello   world \a " \ $

#### Backslash line continuation joins lines outside single quotes
echo hel\
lo "wor\
ld"
## stdout: hello world

#### Backslash-newline inside single quotes is literal
printf '[%s]\n' 'a\
b'
## STDOUT:
[a\
b]
## END

#### Comments start only at unquoted # at word start
echo foo#bar # trailing comment
echo '#' "#notcomment"
## STDOUT:
foo#bar
# #notcomment
## END

#### Quoted reserved word is not a reserved word
'if' 2>/dev/null
echo "status=$?"
## stdout: status=127

#### ANSI-C quoting $'...' basic escapes (POSIX.1-2024)
printf '%s' $'a\tb\nc\\d\'e\"f' | od -An -tx1 | tr -s ' \n' ' '
echo
## stdout:  61 09 62 0a 63 5c 64 27 65 22 66 

#### ANSI-C quoting hex and octal escapes
printf '%s\n' $'\x41\102\u0043'
## stdout: ABC

#### ANSI-C quoting inside double quotes is not special
echo "$'hello\n'"
## stdout: $'hello\n'

#### Empty quotes produce empty argument
set -- '' "" a''b
echo "$#:$1:$2:$3"
## stdout: 3:::ab
