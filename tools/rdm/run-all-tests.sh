#!/usr/bin/env bash
# Runs every native test env and the mbxd pytest suite, then prints one summary (06-implementatieplan-v1.md, WP10).
#
#   tools/rdm/run-all-tests.sh [--envs "<env> ..."] [--no-pytest]
#
# Default envs: every [env:native*] section in platformio.ini. Keeps going after a failure; exit 0 only if all pass.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO"

ENVS=""
PYTEST=1
while [ $# -gt 0 ]; do
  case "$1" in
    --envs) ENVS="$2"; shift 2 ;;
    --no-pytest) PYTEST=0; shift ;;
    -h|--help) sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$ENVS" ] || ENVS="$(sed -n 's/^\[env:\(native[A-Za-z0-9_]*\)\]$/\1/p' platformio.ini | tr '\n' ' ')"

LOGDIR="${TMPDIR:-/tmp}"
LOGDIR="${LOGDIR%/}/rdm-tests-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$LOGDIR"

rows=""
status=0
for env in $ENVS; do
  log="$LOGDIR/$env.log"
  echo "== pio test -e $env"
  pio test -e "$env" >"$log" 2>&1
  rc=$?
  summary="$(grep -E '[0-9]+ test cases:' "$log" | tail -1 | sed 's/=//g; s/^ *//; s/ *$//')"
  failed_suites="$(grep -E "^$env +[A-Za-z0-9_]+ +(FAILED|ERRORED)" "$log" | awk '{print $2}' | tr '\n' ' ')"
  if [ $rc -eq 0 ]; then result=PASS; else result=FAIL; status=1; fi
  [ -n "$summary" ] || summary="no test summary (see log)"
  rows="$rows$(printf '%-20s %-5s %s %s' "$env" "$result" "$summary" "${failed_suites:+failed: $failed_suites}")\n"
done

if [ $PYTEST -eq 1 ] && [ -f examples/mailbox_server/mbxd/test_mbxd.py ]; then
  log="$LOGDIR/pytest-mbxd.log"
  echo "== pytest examples/mailbox_server/mbxd"
  PYTHONDONTWRITEBYTECODE=1 python3 -m pytest -q -p no:cacheprovider examples/mailbox_server/mbxd >"$log" 2>&1
  rc=$?
  summary="$(tail -1 "$log" | sed 's/=//g; s/^ *//; s/ *$//')"
  if [ $rc -eq 0 ]; then result=PASS; else result=FAIL; status=1; fi
  rows="$rows$(printf '%-20s %-5s %s' "pytest-mbxd" "$result" "$summary")\n"
fi

echo
echo "summary (logs: $LOGDIR)"
printf "$rows"
[ $status -eq 0 ] && echo "ALL PASSED" || echo "FAILURES"
exit $status
