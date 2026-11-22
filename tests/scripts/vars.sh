X=hello
Y=$X
echo "$X $Y"
A=1 B=$A
echo "$A $B"
X=temp echo "$X"
X=exported env | grep '^X='
echo "after=$X"
S=persist :
echo "special=$S"
R=transient true
echo "regular=$R"
G=*.c
echo "$G"
