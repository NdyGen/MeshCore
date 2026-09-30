#!/usr/bin/env bash
# Runs every native test env and the meshcore-mailboxd cargo tests, then prints one summary (06-implementatieplan-v1.md, WP10).
#
#   tools/rdm/run-all-tests.sh [--envs "<env> ..."] [--jobs N] [--serial] [--mbxd-crate <dir>] [--no-mbxd]
#
# Default envs: every [env:native*] section in platformio.ini. The first env runs alone: it fetches the shared
# packages and writes .pio/build/project.checksum, which a second pio started at the same time would wipe together
# with the whole build directory. The remaining envs and `cargo test` run N at a time (default: cores / 2, at least
# 1; --serial is --jobs 1) with a log per env. The daemon crate (default examples/mailbox_server/meshcore-mailboxd)
# is built with `cargo build --release` before the envs, so the scenarios with a real daemon find the binary
# (RDM_MBXD); a missing crate is a failure unless --no-mbxd. Keeps going after a failure; exit 0 only if all pass.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO" || exit 2

ENVS=""
JOBS=""
MBXD=1
CRATE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --envs) ENVS="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --serial) JOBS=1; shift ;;
    --mbxd-crate) CRATE="$2"; shift 2 ;;
    --no-mbxd) MBXD=0; shift ;;
    -h|--help) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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
[ -n "$CRATE" ] || CRATE="$REPO/examples/mailbox_server/meshcore-mailboxd"
[ "${CRATE#/}" != "$CRATE" ] || CRATE="$PWD/$CRATE"

LOGDIR="${TMPDIR:-/tmp}"
LOGDIR="${LOGDIR%/}/rdm-tests-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$LOGDIR"
export LOGDIR CRATE

daemon_name() {   # the crate's bin target, from cargo itself (it was mbxd before the crate became meshcore-mailboxd)
  (cd "$CRATE" && cargo metadata --no-deps --format-version 1 2>/dev/null) | python3 -c '
import json, sys
for p in json.load(sys.stdin)["packages"]:
    for t in p["targets"]:
        if "bin" in t["kind"]:
            print(t["name"]); sys.exit(0)
sys.exit(1)'
}

status=0
mbxd_row=""   # printed after the env rows
if [ $MBXD -eq 1 ]; then
  if [ ! -f "$CRATE/Cargo.toml" ]; then
    mbxd_row="$(printf '%-20s %-5s %s' cargo-mailboxd FAIL "crate not found: $CRATE (pass --mbxd-crate <dir> or --no-mbxd)")"
    status=1; MBXD=0
  else
    echo "== cargo build --release ($CRATE)"
    DAEMON="$CRATE/target/release/$(daemon_name)"
    if ! (cd "$CRATE" && cargo build --release) >"$LOGDIR/cargo-build.log" 2>&1; then
      mbxd_row="$(printf '%-20s %-5s %s' cargo-mailboxd FAIL "cargo build --release failed (see $LOGDIR/cargo-build.log)")"
      status=1; MBXD=0
    elif [ ! -x "$DAEMON" ]; then
      mbxd_row="$(printf '%-20s %-5s %s' cargo-mailboxd FAIL "no binary at $DAEMON after cargo build")"
      status=1; MBXD=0
    elif [ -n "${RDM_MBXD:-}" ]; then
      echo "   scenarios use RDM_MBXD=$RDM_MBXD (already set)"
    elif [ -f examples/mailbox_server/mbxd/mbxd.py ]; then
      # while mbxd.py is in the tree the C++ harness (SubprocessBackend) still spawns it under python3 and cannot
      # run a binary; the rule goes when mbxd.py does
      echo "   scenarios use examples/mailbox_server/mbxd/mbxd.py (still in the tree), not $DAEMON"
    else
      export RDM_MBXD="$DAEMON"
      echo "   scenarios use RDM_MBXD=$RDM_MBXD"
    fi
  fi
fi

run_job() {   # $1 = env, or cargo-test; writes $LOGDIR/<job>.log and <job>.rc
  local job="$1" rc
  if [ "$job" = cargo-test ]; then
    echo "== cargo test ($CRATE)"
    (cd "$CRATE" && cargo test) >"$LOGDIR/$job.log" 2>&1
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
[ $MBXD -eq 0 ] || rest+=(cargo-test)
run_job "${envs[0]}"
if [ ${#rest[@]} -ge 1 ]; then
  # shellcheck disable=SC2016   # $1 is the job name xargs hands to each bash
  printf '%s\n' "${rest[@]}" | xargs -P "$JOBS" -n 1 bash -c 'run_job "$1"' _
fi

rows=()
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
if [ $MBXD -eq 1 ]; then
  log="$LOGDIR/cargo-test.log"
  rc="$(cat "$LOGDIR/cargo-test.rc" 2>/dev/null || echo 1)"
  # one "test result:" line per test binary (unit, integration, doc tests)
  summary="$(awk '/^test result:/ { for (i = 1; i < NF; i++) { if ($(i + 1) == "passed;") p += $i; if ($(i + 1) == "failed;") f += $i; if ($(i + 1) == "ignored;") s += $i } n++ }
                  END { if (n) printf "%d passed, %d failed, %d ignored in %d test binaries", p, f, s, n }' "$log")"
  if [ "$rc" -eq 0 ] && [ -n "$summary" ]; then result=PASS; else result=FAIL; status=1; fi
  [ -n "$summary" ] || summary="no test result (see log)"
  mbxd_row="$(printf '%-20s %-5s %s' cargo-mailboxd "$result" "$summary")"
fi
[ -z "$mbxd_row" ] || rows+=("$mbxd_row")

echo
echo "summary (logs: $LOGDIR)"
printf '%s\n' "${rows[@]}"
[ $status -eq 0 ] && echo "ALL PASSED" || echo "FAILURES"
exit $status
