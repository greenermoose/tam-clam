a=(one "two three" [5]=five)
a+=(six)
a[1]+=more
declare -A m=([k]=v ["a b"]="c d")
m[k]+=2
echo "${a[@]}" "${a[*]}" "${#a[@]}" "${!a[@]}" "${a[-1]}" "${a[@]%.txt}" "${m["a b"]}"
echo $((a[0] + a[5]++))
unset 'a[1]' 'm[k]'
