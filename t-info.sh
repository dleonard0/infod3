#!/bin/sh

# Test the `info` tool against the `infod` daemon.
# Uses a private socket based on script's PID.
# Export V=1 for verbosity


# Executables being tested
infod=./infod
info=./info

# newline
nl="
"
# Prepare temporary files
TMP=/tmp/t-info.tmp.$$
export INFOD_SOCKET=$TMP.socket
rm -f $INFOD_SOCKET

# Start a private server running
$infod -f $TMP.db &
INFOD_PID=$!
trap "kill $INFOD_PID; rm -f $TMP.db $INFOD_SOCKET; exit" 0 1 2
while nice test ! -e $INFOD_SOCKET; do : ;done # busy wait

print_last () {
	# Something failed. Abort.
	printf ' command:  [1m%s[m\n' "$last_cmd"
	printf ' exitcode: %d\n' $last_exitcode
	if [ -s $TMP.out ]; then
	    echo ' stdout:'
	    sed 's/^/ [36m|[m/;s/$/[36m$[m/' < $TMP.out
	fi
	if [ -s $TMP.err ]; then
	    echo ' stderr:'
	    sed 's/^/ [36m|[m/;s/$/[36m$[m/' < $TMP.err
	fi
}
run () {
	# Run the command, saving its stdout, stderr and exit code
	last_cmd="$*"
	[ "$V" ] && echo "+ $*" >&2
	"$@" >$TMP.out 2>$TMP.err
	last_exitcode=$?
}

expect () { # exitcode stdout stderr
	# Check the exit code, expected stdout and stderr
	if [ $last_exitcode -ne ${1-0} ]; then
		failed="Expected exit code $1, actual $last_exitcode"
	elif test x"$2" != x"$(cat $TMP.out)"; then
		failed="Expected standard output: <$2>"
	elif test x"$3" != x"$(cat $TMP.err)"; then
		failed="Expected standard error: <$3>"
	fi
	if [ "$failed" ]; then
		echo "[31mFAIL:[m expect failed when running:" >&2
		print_last >&2
		echo "[31mERROR:[m $failed" >&2
		exit 1
	fi
}
assert () { #cmd
	if ! "$@"; then
		failed="assert failed"
		echo "[31mFAIL:[m 'assert $*' failed after running:" >&2
		print_last >&2
		echo "[31mERROR:[m $failed" >&2
		exit 1
	fi
}

sort_stdout () {
	sort $TMP.out > $TMP.out.sorted &&
	mv $TMP.out.sorted $TMP.out
}

# Basic test: we can store a key and get its value back
run $info foo=bar
  expect 0
run $info foo
  expect 0 "bar"

# We can delete a key
run $info -d foo
  expect 0
run $info foo
  expect 1

# We can add and delete the same key and its gone
run $info -w a=b -d a -r a
  expect 1
run $info a
  expect 1

# The -k option displays the key
run $info -k -w somekey=value -r somekey
  expect 0 "somekey value"
# The -k option takes a string for delimiting
run $info -k"FOO" -r somekey
  expect 0 "somekeyFOOvalue"

# -k has no effect for deleted keys
run $info -k -d somekey -r somekey
  expect 1
# -k has an effect if you add -b for blank
run $info -b -k -r somekey
  expect 1 "somekey "

# -A dumps all keys
run $info foo=bar ducks=arecool
  expect 0
run $info -A
  sort_stdout
  expect 0 "ducks=arecool${nl}foo=bar"

# -C clears all the keys
run $info -C
  expect 0

# no args has no effect
run $info
  expect 0

# help is available to all that request it
run $info -?
  assert [ $last_exitcode -eq 2 ]
  assert grep usage: $TMP.err >/dev/null

# try some weird keys and values
run $info -w key=e=e=e=e=e
  expect 0
run $info -r key
  expect 0 "e=e=e=e=e"

