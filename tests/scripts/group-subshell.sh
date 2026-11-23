x=outer
{
	x=group
	echo "in-group:$x"
}
echo "after-group:$x"

(
	x=subshell
	echo "in-sub:$x"
)
echo "after-sub:$x"

orig_pwd=$PWD
(cd /; echo "sub-pwd:$PWD")
if test "$PWD" = "$orig_pwd"; then
	echo "pwd-preserved"
fi

parent_pid=$$
(
	if test "$$" = "$parent_pid"; then
		echo "pid-same"
	fi
)

(exit 7)
echo "sub-exit:$?"

{ echo one; echo two; } | tr a-z A-Z
(echo sub1; echo sub2) | tr a-z A-Z

{ echo hidden; } >/dev/null
echo "redir-status:$?"

! { false; }
echo "bang-group:$?"
