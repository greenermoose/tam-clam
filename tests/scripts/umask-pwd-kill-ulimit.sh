(
	umask 0027
	umask
	umask -S
	umask u=rwx,g=rx,o=rx
	umask
	umask -S
)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/real"
ln -s "$tmp/real" "$tmp/sym"
(
	cd -L "$tmp/sym"
	l=$(pwd -L)
	p=$(pwd -P)
	[ "$l" = "$tmp/sym" ]; echo "pwd-L:$?"
	[ "$p" = "$tmp/real" ]; echo "pwd-P:$?"
)

kill -l 15
kill -l 143

got=0
trap 'got=1' USR1
kill -USR1 $$
echo "trap-kill:$got"
trap - USR1

sleep 30 &
pid=$!
kill -TERM "$pid"
wait "$pid"
st=$?
[ "$st" -gt 128 ]; echo "killed-bg:$?"

(
	ulimit -S -n 64
	ulimit -S -n
)
