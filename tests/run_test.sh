#!/usr/bin/env bash
# Runs oclgdb against a <name>.txt script and checks its "# expect:" annotations.
#
# Each script line of the form
#   # expect: <substring>
# asserts that <substring> appears somewhere in oclgdb's combined stdout/stderr for the
# whole run. This is intentionally coarse -- these are end-to-end smoke tests against a
# real ptrace'd pocl process, not unit tests, so we assert on the human-readable facts
# that matter (a breakpoint fired, a variable printed the right value, the bug's wrong
# value showed up) rather than exact formatting.
set -u
OCLGDB="$1"
SCRIPT="$2"
ROOT="$3"

OUT=$(cd "$ROOT" && POCL_DEVICES=basic "$OCLGDB" --batch -x "$SCRIPT" 2>&1)
STATUS=0

while IFS= read -r line; do
    case "$line" in
        "# expect: "*)
            needle="${line#\# expect: }"
            if ! grep -qF -- "$needle" <<<"$OUT"; then
                echo "MISSING: $needle"
                STATUS=1
            fi
            ;;
    esac
done < "$SCRIPT"

if [ "$STATUS" -ne 0 ]; then
    echo "--- full output ---"
    echo "$OUT"
fi
exit $STATUS
