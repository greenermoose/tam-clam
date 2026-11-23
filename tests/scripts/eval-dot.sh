# eval and . (dot) special builtins.

# eval concatenates args with space.
eval echo hello world

# eval with empty / whitespace-only returns 0.
false
eval ""
echo "eval-empty:$?"
false
eval "   "
echo "eval-ws:$?"

# eval sets variables in current shell.
name=X
eval "$name=42"
echo "eval-var:$X"

# eval with -- separator.
eval -- echo dash-dash

# . (dot) with direct path, variable persistence, and return.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/lib.sh" <<'EOF'
DOT_VAR=from_dot
echo "in-dot"
return 9
echo "unreachable"
EOF

. "$tmp/lib.sh"
echo "dot-ret:$?:$DOT_VAR"

# . (dot) searching PATH.
cat >"$tmp/pathlib.sh" <<'EOF'
PATH_DOT=found_via_path
EOF
OLDPATH=$PATH
PATH="$tmp:$PATH"
. pathlib.sh
PATH=$OLDPATH
echo "path-dot:$PATH_DOT"

# . (dot) on empty file returns 0.
: >"$tmp/empty.sh"
false
. "$tmp/empty.sh"
echo "dot-empty:$?"
