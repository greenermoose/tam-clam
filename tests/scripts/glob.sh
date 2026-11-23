# Pathname expansion (globbing), case patterns, and ${x#pat}/${x%pat}
tmp=$(mktemp -d)
cd "$tmp" || exit 1
: > alpha.c
: > beta.c
: > gamma.txt
: > .dotfile
mkdir sub
: > sub/one.txt
: > sub/two.txt

echo *.c
echo [ab]*.c
echo [!a]*.c
echo *
echo .d*
echo */
echo sub/*.txt
echo *.nomatch
echo "*.c"

# case full patterns
for w in foo.c bar.h 123 '*literal'; do
	case "$w" in
		'*\l'*) echo "quoted-no" ;;
		'*'*) echo "star:$w" ;;
		*.c) echo "c:$w" ;;
		[[:digit:]][[:digit:]][[:digit:]]) echo "num:$w" ;;
		*) echo "other:$w" ;;
	esac
done

# ${x#pat}, ${x##pat}, ${x%pat}, ${x%%pat}
p="/usr/local/bin/sh"
echo "${p#*/}"
echo "${p##*/}"
f="archive.tar.gz"
echo "${f%.*}"
echo "${f%%.*}"
q="a*b*c"
echo "${q#a\*}"

cd /
rm -rf "$tmp"
