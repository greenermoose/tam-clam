# Command substitution: $(...) and `...`
echo "a:$(echo hello):b"
echo "nested:$(echo "inner:$(echo deep)")"
echo "empty:$(   ):end"
echo "newlines:$(printf 'x\n\n\n'):end"
echo "backq:`echo hi there`"
echo "backq-dq:`echo \"quoted\"`"
echo "backq-nest:`echo \`echo deep2\``"

# Case and comments inside $(...) (parse-once)
echo "case:$(case foo in f*) echo matched;; esac)"
echo "comment:$(echo ok # ) not closing
)"

# Disambiguation: $((cmd); (cmd)) is command substitution, not arithmetic
echo "subshell:$((echo one); (echo two))"

# Exit status of assignment-only command with command substitution
x=$(echo val; exit 7)
echo "assign:$x:$?"
$(exit 5)
echo "bare:$?"

# Command substitution inside unquoted here-document and inside function
cat <<EOF
hd:$(echo from-hd):$((2 + 3))
EOF

f() {
	echo "fn:$(echo "$1"):`echo "$2"`"
}
f alpha beta
