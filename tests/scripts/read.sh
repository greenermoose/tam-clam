printf '  alpha  beta  gamma  delta  \n' | {
	read a b c
	echo "<$a><$b><$c>"
}

printf 'one:two:three:four\n' | {
	IFS=: read x y z
	echo "<$x><$y><$z>"
}

printf 'only1\n' | {
	read p q r
	echo "<$p><$q><$r>"
}

printf 'hello\\\nworld\n' | {
	read line
	echo "cont:<$line>"
}

printf 'a\\ b\n' | {
	read -r raw
	echo "raw:<$raw>"
}

printf 'a\\ b\n' | {
	read cooked
	echo "cooked:<$cooked>"
}

printf 'no-newline' | {
	read partial
	echo "eof:$?:<$partial>"
}

printf 'default-reply\n' | {
	read
	echo "reply:<$REPLY>"
}
