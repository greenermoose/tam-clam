VAR=world
cat <<EOF
alpha
hello $VAR
${UNSET:-fallback}
\$literal
EOF
cat <<'EOF'
$literal
`also`
EOF
cat <<-EOF
	indented
		more
	EOF
cat <<A | tr a-z A-Z
lower
A
cat <<ONE; cat <<TWO
first
ONE
second
TWO
