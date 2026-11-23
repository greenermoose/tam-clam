if true; then echo yes; fi
if false; then echo no; fi
echo "nomatch:$?"
if false; then
	echo a
elif true; then
	echo b
else
	echo c
fi
if false; then
	echo a
elif false; then
	echo b
else
	echo c
fi
if true; then
	if false; then
		echo inner-no
	else
		echo inner-yes
	fi
fi
if true; then echo piped; fi | tr a-z A-Z
