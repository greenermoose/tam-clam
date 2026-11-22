V=hello
E=
echo "$V" "${V}!" "$UNSET" '$' "$ " "\$V" '$V'
echo "${#V}" "${#E}" "${#UNSET}"
echo "${UNSET:-def}" "${UNSET-def}" "${E:-def}" "${E-def}" "${V:-def}"
echo "${NEW1:=first}" "$NEW1" "${E=keep}" "${E:=nowset}" "$E"
echo "${V:+alt}" "${V+alt}" "${UNSET:+alt}" "${UNSET+alt}"
E2=
echo "${E2:+alt}" "${E2+alt}"
echo "${UNSET:-${V:-fallback}}" "${UNSET:-"a b"}"
P=foo/bar/baz.txt
echo "${P%.txt}" "${P%%.txt}" "${P#foo/}" "${P##foo/}" "${P#miss}"
true
echo "t=$?"
false
echo "f=$?"
echo "$UNSET" only
echo $UNSET only
