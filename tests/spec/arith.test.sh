# Arithmetic expansion (POSIX.1-2024 XCU 2.6.4)

#### Precedence and parentheses
echo $((1 + 2 * 3)) $(((1 + 2) * 3)) $((20 / 3)) $((20 % 3))
## stdout: 7 9 6 2

#### Decimal, octal, and hex constants
echo $((10 + 010 + 0x10 + 0X10))
## stdout: 50

#### Bitwise and shift operators
echo $(( (1 << 4) | 3 )) $(( 19 & 7 )) $(( 15 ^ 6 )) $(( ~0 )) $(( -8 >> 1 ))
## stdout: 19 3 9 -1 -4

#### Relational and logical operators with short-circuiting
x=0
echo $(( 0 && (x = 1) )) "$x"
echo $(( 1 || (x = 2) )) "$x"
echo $(( 1 ? 10 : (x = 3) )) "$x"
echo $(( 0 ? (x = 4) : 20 )) "$x"
## STDOUT:
0 0
1 0
10 0
20 0
## END

#### Assignment and increment/decrement operators
a=5
echo $((a += 3)) $((a++)) $((a)) $((--a))
## stdout: 8 8 9 8

#### Unset and empty variables evaluate to 0 when nounset is off
unset u
e=""
echo $((u + 5)) $((e + 7))
## stdout: 5 7

#### 64-bit signed range
echo $(( 2147483647 + 1 ))
## stdout: 2147483648

#### Division by zero fails
(echo $((1 / 0)); echo never) 2>/dev/null
## status: nonzero

#### Signed 64-bit overflow is a reported error in tam-clam
(echo $((9223372036854775807 + 1)); echo never) 2>/dev/null
## status: nonzero
## BUG bash status: 0
## BUG bash STDOUT:
-9223372036854775808
never
## END
