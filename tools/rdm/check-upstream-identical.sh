#!/usr/bin/env bash
# Proves that builds without RDM flags are identical to the upstream base (06-implementatieplan-v1.md, WP10).
#
#   tools/rdm/check-upstream-identical.sh [--base <rev>] [--envs "<env> ..."] [--work <dir>] [--keep] [--compare-only]
#
# Builds <rev> and the current working tree (including uncommitted files) one after the other in the same
# directory, so absolute paths in debug info and __FILE__ are equal, then compares per env:
#   - the set of object files and every object byte for byte,
#   - the preprocessor output (-E -P, from compile_commands.json) of the upstream sources RDM touches and of every
#     other compiled source that differs between the two trees (firmware envs only),
#   - the firmware images (firmware.bin / .hex / .uf2) where the env produces them.
# native* envs are built with `pio test --without-testing`. SOURCE_DATE_EPOCH is fixed to the base commit time.
# Objects that differ only in debug info and ESP32 images that differ only in their ELF/image hashes count as
# code-identical. Exit 0 if everything is identical or code-identical.
set -euo pipefail

BASE=22baa5e3
ENVS="Heltec_v3_companion_radio_ble RAK_4631_companion_radio_ble Tbeam_SX1262_companion_radio_ble native native_kiss_modem"
TMPDIR="${TMPDIR:-/tmp}"
WORK="${TMPDIR%/}/rdm-compat"
KEEP=0
COMPARE_ONLY=0
TOUCHED="src/Mesh.cpp src/helpers/BaseChatMesh.cpp examples/companion_radio/MyMesh.cpp examples/companion_radio/DataStore.cpp"

while [ $# -gt 0 ]; do
  case "$1" in
    --base) BASE="$2"; shift 2 ;;
    --envs) ENVS="$2"; shift 2 ;;
    --work) WORK="$2"; shift 2 ;;
    --keep) KEEP=1; shift ;;
    --compare-only) COMPARE_ONLY=1; shift ;;   # reuse $WORK/{base,head} of an earlier run
    -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
TREE="$WORK/tree"
LOG="$WORK/build.log"
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd -P)"   # compile_commands.json uses resolved paths (/private/var on macOS)
TREE="$WORK/tree"
LOG="$WORK/build.log"
: > "$LOG"
git -C "$REPO" rev-parse --verify --quiet "$BASE^{commit}" >/dev/null || { echo "unknown base: $BASE" >&2; exit 2; }
# RadioLib (BuildOpt.h), rweather Crypto (RNG.cpp) and the ESP32 core (firmware_msc_fat.c) embed __DATE__/__TIME__;
# GCC takes both from SOURCE_DATE_EPOCH, so both sides get the same value.
export SOURCE_DATE_EPOCH="$(git -C "$REPO" log -1 --format=%ct "$BASE")"

snapshot() {   # $1 = base|head
  local stage="$WORK/stage-$1"
  rm -rf "$stage"; mkdir -p "$stage" "$TREE"
  if [ "$1" = base ]; then
    git -C "$REPO" archive "$BASE" | tar -x -C "$stage"
  else
    rsync -a --exclude .pio --exclude .git --exclude .DS_Store "$REPO/" "$stage/"
  fi
  # keep $TREE/.pio (libdeps, toolchain links) but never its build output
  rsync -a --delete --exclude /.pio "$stage/" "$TREE/"
  rm -rf "$TREE/.pio/build"
}

build_side() {   # $1 = base|head
  local side="$1" out="$WORK/$1"
  rm -rf "$out"; mkdir -p "$out"
  snapshot "$side"
  for env in $ENVS; do
    echo "[$side] building $env"
    mkdir -p "$out/$env/obj" "$out/$env/pp" "$out/$env/img"
    case "$env" in
      native*)
        (cd "$TREE" && pio test -e "$env" --without-testing) >>"$LOG" 2>&1 || { echo "[$side] build failed: $env (see $LOG)"; return 1; }
        ;;
      *)
        (cd "$TREE" && pio run -e "$env") >>"$LOG" 2>&1 || { echo "[$side] build failed: $env (see $LOG)"; return 1; }
        (cd "$TREE" && pio run -e "$env" -t compiledb) >>"$LOG" 2>&1 || { echo "[$side] compiledb failed: $env"; return 1; }
        preprocess "$env" "$out/$env/pp"
        ;;
    esac
    local b="$TREE/.pio/build/$env"
    (cd "$b" && find . -name '*.o' -type f) | while IFS= read -r f; do
      mkdir -p "$out/$env/obj/$(dirname "$f")"; cp -p "$b/$f" "$out/$env/obj/$f"
    done
    for img in firmware.bin firmware.hex firmware.uf2; do
      if [ -f "$b/$img" ]; then cp -p "$b/$img" "$out/$env/img/$img"; fi
    done
  done
}

preprocess() {   # $1 = env, $2 = output dir
  local extra=""
  if [ -d "$WORK/stage-base" ] && [ -d "$WORK/stage-head" ]; then
    extra="$(cd "$WORK" && diff -rq stage-base stage-head 2>/dev/null | sed -n 's#^Files stage-base/\(.*\) and stage-head/.* differ$#\1#p' | grep -E '\.(c|cpp)$' || true)"
  fi
  python3 - "$TREE" "$1" "$2" $TOUCHED $extra <<'PY'
import json, os, shlex, subprocess, sys
tree, env, out, files = os.path.realpath(sys.argv[1]), sys.argv[2], sys.argv[3], set(sys.argv[4:])
db = json.load(open(os.path.join(tree, "compile_commands.json")))
done = 0
for e in db:
    path = os.path.relpath(os.path.realpath(os.path.join(e["directory"], e["file"])), tree)
    if path not in files:
        continue
    args = e["arguments"] if "arguments" in e else shlex.split(e["command"])
    cmd, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a == "-o":
            skip = True
            continue
        if a == "-c":
            continue
        cmd.append(a)
    dest = os.path.join(out, path + ".i")
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    subprocess.run(cmd + ["-E", "-P", "-o", dest], cwd=e["directory"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    done += 1
print(f"  preprocessed {done} source(s) for {env}")
if done == 0:
    sys.exit("no sources preprocessed: compile_commands.json does not match the expected paths")
PY
}

# Objects that differ only in debug info (line numbers shift when #ifdef blocks add lines; ESP32 builds with -g).
debug_only_diff() {   # $1, $2 = object files; true if identical after --strip-debug with a matching objcopy
  local oc ta tb machine pattern
  ta="$WORK/strip-a.o"; tb="$WORK/strip-b.o"
  # pick the objcopy from the ELF machine field (bytes 18-19), never probe other toolchains
  machine="$(od -An -tu2 -j18 -N2 "$1" 2>/dev/null | tr -d ' ')"
  case "$machine" in
    94) pattern="toolchain-xtensa-esp32*/bin/xtensa-*-objcopy" ;;
    40) pattern="toolchain-gccarmnoneeabi*/bin/arm-none-eabi-objcopy" ;;
    243) pattern="toolchain-riscv32-esp*/bin/riscv32-*-objcopy" ;;
    *) return 1 ;;
  esac
  for oc in "$HOME"/.platformio/packages/$pattern; do
    [ -x "$oc" ] || continue
    "$oc" --strip-debug "$1" "$ta" 2>/dev/null && "$oc" --strip-debug "$2" "$tb" 2>/dev/null || return 1
    cmp -s "$ta" "$tb"
    return
  done
  return 1
}

# ESP32 app images embed the SHA256 of the ELF (esp_app_desc_t.app_elf_sha256, image offset 0xB0) and end with a
# SHA256 of the image; both follow from debug info. Compare with those 32 + 33 bytes masked.
esp_image_same_code() {   # $1, $2 = firmware.bin
  python3 - "$1" "$2" <<'PY'
import struct, sys
a, b = (bytearray(open(p, "rb").read()) for p in sys.argv[1:3])
if len(a) != len(b) or len(a) < 0x100 or struct.unpack_from("<I", a, 0x20)[0] != 0xABCD5432:
    sys.exit(1)
for buf in (a, b):
    buf[0xB0:0xD0] = bytes(32)
    buf[-33:] = bytes(33)
sys.exit(0 if a == b else 1)
PY
}

compare_env() {   # $1 = env; prints findings, returns 1 on difference, 2 when only debug info differs
  local env="$1" a="$WORK/base/$1" b="$WORK/head/$1" rc=0 kind
  for kind in obj pp img; do
    local la lb
    la="$(cd "$a/$kind" && find . -type f | sort)"
    lb="$(cd "$b/$kind" && find . -type f | sort)"
    if [ "$la" != "$lb" ]; then
      echo "  $env/$kind: file sets differ"
      diff <(echo "$la") <(echo "$lb") | sed 's/^/    /' | head -20
      rc=1
    fi
    local n=0 nd=0
    while IFS= read -r f; do
      [ -n "$f" ] || continue
      [ -f "$b/$kind/$f" ] || continue
      n=$((n + 1))
      if ! cmp -s "$a/$kind/$f" "$b/$kind/$f"; then
        if [ "$kind" = obj ] && debug_only_diff "$a/$kind/$f" "$b/$kind/$f"; then
          echo "  $env/$kind: debug info only: ${f#./}"; [ $rc -eq 1 ] || rc=2
        elif [ "$kind" = img ] && [ "${f##*/}" = firmware.bin ] && esp_image_same_code "$a/$kind/$f" "$b/$kind/$f"; then
          echo "  $env/$kind: only the embedded ELF/image hashes differ: ${f#./}"; [ $rc -eq 1 ] || rc=2
        else
          echo "  $env/$kind: differs: ${f#./}"; nd=$((nd + 1)); rc=1
        fi
      fi
    done <<< "$la"
    printf '  %-34s %-4s %4d compared, %d different in code or data\n' "$env" "$kind" "$n" "$nd"
  done
  return $rc
}

echo "base: $(git -C "$REPO" rev-parse --short "$BASE")  head: working tree of $REPO"
echo "envs: $ENVS"
echo "work: $WORK (log: $LOG)"
if [ $COMPARE_ONLY -eq 0 ]; then
  # stage both trees first, so the preprocessor step knows which sources differ
  snapshot base
  snapshot head
  build_side base
  build_side head
fi

status=0
echo "result:"
for env in $ENVS; do
  rc=0; compare_env "$env" || rc=$?
  if [ $rc -eq 1 ]; then status=1; elif [ $rc -eq 2 ] && [ $status -eq 0 ]; then status=2; fi
done
case $status in
  0) echo "IDENTICAL: builds without RDM flags match $BASE byte for byte" ;;
  2) echo "CODE-IDENTICAL: machine code, data and preprocessor output match $BASE; only debug info (line numbers) and the hashes derived from it differ"; status=0 ;;
  *) echo "DIFFERENT: see the lines above" ;;
esac
[ $KEEP -eq 1 ] || rm -rf "$WORK/tree/.pio/build" "$WORK/stage-base" "$WORK/stage-head"
exit $status
