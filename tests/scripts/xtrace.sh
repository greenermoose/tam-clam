# set -x (xtrace), PS4 expansion, and LINENO

# Basic trace with default PS4
(
	PS4='+ '
	set -x
	echo hello
	set +x
) 2>&1

# Custom PS4 with LINENO and variable
(
	tag=dbg
	PS4='+$tag:$LINENO: '
	set -x
	x=42
	echo "two words" "it's"
	set +x
) 2>&1

# LINENO tracks script lines
echo "ln1=$LINENO"
echo "ln2=$LINENO"
