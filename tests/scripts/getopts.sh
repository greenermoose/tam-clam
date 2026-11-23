set -- -a -b val -cfoo -- rest1 rest2
OPTIND=1
while getopts "ab:c:" opt; do
	echo "opt=$opt arg=${OPTARG:-NONE} ind=$OPTIND"
done
shift $((OPTIND - 1))
echo "rem:$#:$*"

OPTIND=1
while getopts ":ab:" opt -a -z -b; do
	echo "silent=$opt arg=${OPTARG:-NONE}"
done

OPTIND=1
while getopts "xy:" opt -xyval; do
	echo "cluster=$opt arg=${OPTARG:-NONE}"
done
