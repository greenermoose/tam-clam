# set -C (noclobber) POSIX.1-2024 rules
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

set -C

# Creating a new file succeeds
echo first >"$tmp/f"
echo "new=$? content=$(cat "$tmp/f")"

# Overwriting existing regular file with > fails and preserves contents
echo second >"$tmp/f" 2>/dev/null
echo "clobber_blocked=$? content=$(cat "$tmp/f")"

# >| overrides noclobber
echo forced >|"$tmp/f"
echo "force=$? content=$(cat "$tmp/f")"

# Non-regular file (/dev/null) is allowed even with noclobber
echo discarded >/dev/null
echo "devnull=$?"

# set +C allows overwriting again
set +C
echo overwritten >"$tmp/f"
echo "plus_C=$? content=$(cat "$tmp/f")"
