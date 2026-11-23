# Arithmetic expansion $((...))
echo $((1 + 2 * 3)) $(((1 + 2) * 3)) $(( ))
echo $((010 + 0x10))
echo $((20 / 3)) $((20 % 3)) $((-5 + 2))
echo $((!0)) $((!5)) $((~0))
echo $((1 << 4)) $((32 >> 2)) $((5 & 3)) $((5 | 2)) $((5 ^ 3))
echo $((3 < 4 && 4 <= 4 && 5 > 4 && 5 >= 5))
echo $((3 == 3 && 3 != 4)) $((0 || 1))
echo $((1 ? 10 : 20)) $((0 ? 10 : 20))

# Variables and assignments
a=10
echo $((a += 5)) "$a"
echo $((a *= 2)) "$a"
echo $((b = a - 3)) "$b"

# Short-circuiting skips side effects and division by zero
c=1
echo $((0 && (c = 99) && (1 / 0))) "$c"
echo $((1 || (c = 99) || (1 / 0))) "$c"
echo $((1 ? 7 : (1 / 0)))

# Nested arithmetic and command substitution inside $((...))
echo $(( $((2 + 3)) * $(echo 4) ))
