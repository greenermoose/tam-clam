classify() {
	case $1 in
		a|b) echo "ab:$1" ;;
		(c|d) echo "cd:$1" ;;
		'*') echo "literal-star" ;;
		empty) ;;
		*) echo "default:$1"
	esac
}

classify a
classify b
classify c
classify d
classify '*'
classify empty
echo "empty-status:$?"
classify other

false
case nomatch in
	foo) echo no ;;
esac
echo "nomatch:$?"

false
case x in
esac
echo "noclauses:$?"

pat=hello
case hello in
	$pat) echo "var-match" ;;
	*) echo "no" ;;
esac
