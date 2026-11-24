## Brace expansion (0.3.1)

#### Basic comma list with preamble and postscript
echo pre{a,b,c}post
## N-I dash
## N-I ash
## stdout: preapost prebpost precpost

#### Empty alternatives and Cartesian product
echo a{,b}{1,2}
## N-I dash
## N-I ash
## stdout: a1 a2 ab1 ab2

#### Nested braces
echo {a,{b,c},d}
## N-I dash
## N-I ash
## stdout: a b c d

#### Integer sequences forward, reverse, stepped, negative, zero-padded
echo {1..5}
echo {5..1}
echo {0..10..2}
echo {10..0..3}
echo {-2..2}
echo {01..05}
echo {001..005..2}
## N-I dash
## N-I ash
## STDOUT:
1 2 3 4 5
5 4 3 2 1
0 2 4 6 8 10
10 7 4 1
-2 -1 0 1 2
01 02 03 04 05
001 003 005
## END

#### Character sequences forward, reverse, stepped
echo {a..e}
echo {e..a}
echo {A..G..2}
## N-I dash
## N-I ash
## STDOUT:
a b c d e
e d c b a
A C E G
## END

#### Quoted braces and invalid braces remain literal
echo "{a,b}" '{1..3}' \{x,y\} {solo} {1..3..x} {a..1}
## N-I dash
## N-I ash
## stdout: {a,b} {1..3} {x,y} {solo} {1..3..x} {a..1}

#### Brace expansion precedes parameter expansion and does not split assignments
x=hi
echo {$x,bye}
v={a,b}
echo "$v"
a=({1..3})
echo "${#a[@]}:${a[*]}"
## N-I dash
## N-I ash
## STDOUT:
hi bye
{a,b}
3:1 2 3
## END
