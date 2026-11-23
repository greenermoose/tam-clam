# ANSI-C quoting $'...' (POSIX.1-2024)
printf '<%s>\n' $'hello\tworld'
printf '<%s>\n' $'a\'b\\c\"d'
printf '<%s>\n' $'\101\x42\u0043'
printf '<%s>\n' $'\u00e9'
printf '<%s>\n' $''
printf '<%s>\n' $'before\0after'
printf '<%s>\n' "$'literal'"
x=
printf '<%s>\n' "${x:-$'default\tval'}"
printf '<%s>\n' ${x:-$'default\tval'}
