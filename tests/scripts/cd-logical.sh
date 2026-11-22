BASE=/tmp/tam-clam-cd-test-$$
rm -rf "$BASE"
mkdir -p "$BASE/real/sub" "$BASE/other/target"
ln -s "$BASE/real" "$BASE/link"
cd "$BASE/link"
echo "L1=${PWD#$BASE}"
cd sub
echo "L2=${PWD#$BASE}"
cd ..
echo "L3=${PWD#$BASE}"
cd -P "$BASE/link"
echo "P1=${PWD#$BASE}"
cd - > "$BASE/out1"
sed "s|$BASE|BASE|g" "$BASE/out1"
echo "BACK=${PWD#$BASE}"
CDPATH="$BASE/other"
cd target > "$BASE/out2"
sed "s|$BASE|BASE|g" "$BASE/out2"
echo "CDPATH=${PWD#$BASE}"
cd /
rm -rf "$BASE"
