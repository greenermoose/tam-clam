# IFS field splitting (XCU 2.6.5)
show() {
	for w in "$@"; do
		printf '<%s>' "$w"
	done
	printf '\n'
}

x="  a   b	c  "
show $x
show "$x"

# Empty unquoted expansion produces 0 words
e="   "
show $e
show "$e"

# Custom non-whitespace IFS
IFS=:
y=":a::b:"
show $y
z="a::"
show $z

# Mixed IFS whitespace and non-whitespace
IFS=' :'
m=" : a : : b : "
show $m
m2="a : b"
show $m2

# Empty IFS disables field splitting
IFS=""
s="a b:c"
show $s

# Restore default IFS; command substitution splitting
IFS=' 	
'
show $(printf '  one\ttwo\nthree  ')
show "$(printf '  one  two  ')"
