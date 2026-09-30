#!/usr/bin/env bash
# Runs every native test env and the mbxd pytest suite, then prints one summary (06-implementatieplan-v1.md, WP10).
#
#   tools/rdm/run-all-tests.sh [--envs "<env> ..."] [--jobs N] [--serial] [--no-pytest]
#
# Default envs: every [env:native*] section in platformio.ini. The first env runs alone: it fetches the shared
# packages and writes .pio/build/project.checksum, which a second pio started at the same time would wipe together
# with the whole build directory. The remaining envs and pytest run N at a time (default: cores / 2, at least 1;
# --serial is --jobs 1) with a log per env. Keeps going after a failure; exit 0 only if all pass.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO" || exit 2

ENVS=""
JOBS=""
PYTEST=1
while [ $# -gt 0 ]; do
  case "$1" in
    --envs) ENVS="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --serial) JOBS=1; shift ;;
    --no-pytest) PYTEST=0; shift ;;
    -h|--help) sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$ENVS" ] || ENVS="$(sed -n 's/^\[env:\(native[A-Za-z0-9_]*\)\]$/\1/p' platformio.ini | tr '\n' ' ')"
read -r -a envs <<< "$ENVS"
[ ${#envs[@]} -ge 1 ] || { echo "no envs" >&2; exit 2; }
if [ -z "$JOBS" ]; then
  cores="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"
  JOBS=$((cores / 2))
  [ "$JOBS" -ge 1 ] || JOBS=1
fi
case "$JOBS" in
  ''|*[!0-9]*|0) echo "--jobs needs a positive number" >&2; exit 2 ;;
esac
[ $PYTEST -eq 0 ] || [ -f examples/mailbox_server/mbxd/test_mbxd.py ] || PYTEST=0

LOGDIR="${TMPDIR:-/tmp}"
LOGDIR="${LOGDIR%/}/rdm-tests-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$LOGDIR"
export LOGDIR

run_job() {   # $1 = env, or pytest-mbxd; writes $LOGDIR/<job>.log and <job>.rc
  local job="$1" rc
  if [ "$job" = pytest-mbxd ]; then
    echo "== pytest examples/mailbox_server/mbxd"
    PYTHONDONTWRITEBYTECODE=1 python3 -m pytest -q -p no:cacheprovider examples/mailbox_server/mbxd >"$LOGDIR/$job.log" 2>&1
    rc=$?
  else
    echo "== pio test -e $job"
    pio test -e "$job" >"$LOGDIR/$job.log" 2>&1
    rc=$?
  fi
  echo $rc >"$LOGDIR/$job.rc"
  echo "== $job done (exit $rc)"
}
export -f run_job

rest=("${envs[@]:1}")
[ $PYTEST -eq 0 ] || rest+=(pytest-mbxd)
run_job "${envs[0]}"
if [ ${#rest[@]} -ge 1 ]; then
  # shellcheck disable=SC2016   # $1 is the job name xargs hands to each bash
  printf '%s\n' "${rest[@]}" | xargs -P "$JOBS" -n 1 bash -c 'run_job "$1"' _
fi

rows=()
status=0
for env in "${envs[@]}"; do
  log="$LOGDIR/$env.log"
  rc="$(cat "$LOGDIR/$env.rc" 2>/dev/null || echo 1)"
  summary="$(grep -E '[0-9]+ test cases:' "$log" | tail -1 | sed 's/=//g; s/^ *//; s/ *$//')"
  skipped="$(echo "$summary" | sed -En 's/.*[^0-9]([0-9]+) skipped.*/\1/p')"
  failed_suites="$(grep -E "^$env +[A-Za-z0-9_]+ +(FAILED|ERRORED)" "$log" | awk '{print $2}' | tr '\n' ' ')"
  if [ "$rc" -eq 0 ]; then result=PASS; else result=FAIL; status=1; fi
  [ -n "$summary" ] || summary="no test summary (see log)"
  rows+=("$(printf '%-20s %-5s %s  skipped: %d%s' "$env" "$result" "$summary" "${skipped:-0}" "${failed_suites:+  failed: $failed_suites}")")
done
if [ $PYTEST -eq 1 ]; then
  log="$LOGDIR/pytest-mbxd.log"
  rc="$(cat "$LOGDIR/pytest-mbxd.rc" 2>/dev/null || echo 1)"
  summary="$(tail -1 "$log" | sed 's/=//g; s/^ *//; s/ *$//')"
  if [ "$rc" -eq 0 ]; then result=PASS; else result=FAIL; status=1; fi
  rows+=("$(printf '%-20s %-5s %s' "pytest-mbxd" "$result" "$summary")")
fi

echo
echo "summary (logs: $LOGDIR)"
printf '%s\n' "${rows[@]}"
[ $status -eq 0 ] && echo "ALL PASSED" || echo "FAILURES"
exit $status
