# Run from the game folder. The Builder supplies this as /bin/sh -c source so
# it is available even after the Builder's AppImage mount goes away.
# Standard error is one end of a socket and standard input the other. The game
# takes a socket on stderr for nowhere to log, so it writes hookshot_log.txt
# itself; this keeps what it prints before that log opens, and its exit status.
umask 077
temporary=$(mktemp -d .hookshot-startup.XXXXXX) || exec "$1" </dev/null 2>/dev/null
trap 'rm -f -- "$temporary/stderr" "$temporary/status"; rmdir -- "$temporary"' EXIT

# Drain all of it so a noisy game can continue after the retained prefix is
# full. The game keeps stdout discarded and gets neither the reading end nor
# standard input.
exec 3<&0 </dev/null
{
    head -c 65536 > "$temporary/stderr"
    cat >/dev/null
} <&3 2>/dev/null &
reader=$!
exec 3<&-
"$1" >/dev/null
printf '\nHookshot exited with status %s\n' "$?" > "$temporary/status"
if [ "$#" -gt 1 ]; then
    printf '%s\n' "$2" >> "$temporary/status"
fi
exec 2>/dev/null
wait "$reader"
if [ -f "$temporary/stderr" ] && [ -f "$temporary/status" ]; then
    cat "$temporary/status" >> "$temporary/stderr" &&
        mv -fT -- "$temporary/stderr" hookshot_startup.log
fi
