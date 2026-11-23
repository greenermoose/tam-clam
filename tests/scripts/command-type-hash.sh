echo() {
	printf 'FUNC:%s\n' "$*"
}
echo shadow
command echo bypassed
unset -f echo

command -v echo
command -v set
command -v if
command -v nosuch_cmd_012 >/dev/null 2>&1
echo "missing-v:$?"

(
	PATH=""
	command -p sh -c 'echo default-path-ok'
)

command shift 99 2>/dev/null
echo "survived-special:$?"

type echo >/dev/null
echo "type-ok:$?"
type nosuch_cmd_012 >/dev/null 2>&1
echo "type-miss:$?"

hash -r
hash sh
hash -r
echo "hash-ok:$?"
