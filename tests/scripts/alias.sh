alias greet='echo hello'
greet world

alias echo='echo [aliased]'
echo once
'echo' quoted

alias pfx='echo prefix '
alias target='expanded'
pfx target

alias greet
unalias greet
alias greet >/dev/null 2>&1
echo "after-unalias:$?"

unalias -a
echo plain
