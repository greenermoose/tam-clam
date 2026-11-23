echo
echo one
echo one two three
echo -n no-newline
echo :after

printf '<%s>\n' a b c
printf -- '-%s-\n' dash
printf '[%5s][%-5s][%.3s]\n' hi hi hello
printf '[%*s][%.*s]\n' 4 x 2 abcd
printf '%d %i %u %o %x %X\n' -7 42 42 64 255 255
printf '%c %d %d\n' Z "'A" '"a'
printf '%b\n' 'line1\n\tline2\0101'
printf '%b' 'before\cafter'
printf '!\n'
printf '100%%\n'
printf 'def:%d:%s:\n'
