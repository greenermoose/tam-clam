false
while false; do
	echo never
done
echo "while-false:$?"

false
until true; do
	echo never
done
echo "until-true:$?"

s=
while test "$s" != "xxx"; do
	s="${s}x"
	echo "w:$s"
done

s=
until test "$s" = "yy"; do
	s="${s}y"
	echo "u:$s"
done

for a in 1 2 3; do
	if test "$a" = "2"; then
		continue
	fi
	echo "cont:$a"
done

for a in 1 2 3; do
	for b in x y z; do
		if test "$b" = "y"; then
			break
		fi
		echo "b1:$a$b"
	done
done

for a in 1 2 3; do
	for b in x y; do
		echo "b2:$a$b"
		if test "$a" = "2"; then
			break 2
		fi
	done
done

for a in 1 2; do
	for b in x y; do
		echo "c2:$a$b"
		if test "$b" = "x"; then
			continue 2
		fi
		echo "never:$a$b"
	done
done
