> /tmp/tc_redir_a
cat < /tmp/tc_redir_a
echo one > /tmp/tc_redir_a
echo two >> /tmp/tc_redir_a
echo three >| /tmp/tc_redir_a
cat < /tmp/tc_redir_a
echo four >> /tmp/tc_redir_a
cat < /tmp/tc_redir_a
printf 'hello\n' > /tmp/tc_redir_b
cat 0< /tmp/tc_redir_b
sh -c 'echo out; echo err >&2' > /tmp/tc_redir_a 2>&1
cat /tmp/tc_redir_a
sh -c 'echo closed >&-' 2>/dev/null || echo closed-failed
true < /tmp/tc_no_such_file 2>/dev/null || echo builtin-redir-restored
rm -f /tmp/tc_redir_a /tmp/tc_redir_b
