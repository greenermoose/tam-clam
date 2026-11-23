for x in alpha beta gamma; do
	echo "item:$x"
done
echo "after:$x"

false
for empty in; do
	echo never
done
echo "empty:$?"

for q in "a b" c; do
	echo "<$q>"
done

show_params() {
	for p; do
		echo "p:$p"
	done
}
show_params one "two three" four

show_params2() {
	for p
	do
		echo "p2:$p"
	done
}
show_params2 x y
