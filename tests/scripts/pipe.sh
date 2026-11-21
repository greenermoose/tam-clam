echo one | tr a-z A-Z
printf 'c\na\nb\n' | sort | tr a-z A-Z
! false && echo negated-false
! true || echo negated-true
exit 5 | true
echo status-after-pipe
echo multi |
tr a-z A-Z
