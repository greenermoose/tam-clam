## Recursive ** globbing with globstar (0.3.2)

#### ** is single-level by default (POSIX compatible)
mkdir -p a/b
touch top.c a/mid.c a/b/deep.c
echo **/*.c
## stdout: a/mid.c

#### shopt -s globstar enables recursive **/*.c
mkdir -p a/b .hidden
touch top.c a/mid.c a/b/deep.c .hidden/skip.c
shopt -s globstar
echo **/*.c
## N-I dash
## N-I ash
## stdout: a/b/deep.c a/mid.c top.c

#### shopt -s globstar: **/ matches directories only
mkdir -p a/b c
touch top.c a/f.c
shopt -s globstar
echo **/
## N-I dash
## N-I ash
## stdout: a/ a/b/ c/

#### shopt -s globstar: trailing ** matches files and dirs recursively
mkdir -p d/sub
touch d/one d/sub/two
shopt -s globstar
echo d/**
## N-I dash
## N-I ash
## stdout: d/ d/one d/sub d/sub/two

#### shopt -s globstar: symlink loops do not cause infinite recursion
mkdir -p dir/sub
touch dir/sub/leaf.c
ln -s ../.. dir/sub/loop
shopt -s globstar
echo dir/**/*.c
## N-I dash
## N-I ash
## stdout: dir/sub/leaf.c

#### Quoted ** is not recursive even with globstar
mkdir -p a/b
touch top.c a/b/deep.c
shopt -s globstar
echo "**"/*.c
## N-I dash
## N-I ash
## stdout: **/*.c

#### shopt -u globstar disables recursive ** again
mkdir -p a/b
touch top.c a/mid.c a/b/deep.c
shopt -s globstar
shopt -u globstar
echo **/*.c
## N-I dash
## N-I ash
## stdout: a/mid.c
