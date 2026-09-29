#!/usr/bin/env bash
# Copies the files a work package owns (docs/reliable-dm/06-implementatieplan-v1.md par. 5) from a worker's
# worktree into this checkout, then runs tools/rdm/run-all-tests.sh (all native envs and the mbxd pytest).
#
#   tools/rdm/integrate.sh [--dry-run] [--no-test] [--with-headers] <worktree> [wp...]
#
# --with-headers also takes the module headers of these packages (06 par. 3 allows workers to add private
# members), but only if tools/rdm/check-headers.py still finds every fixed declaration in the result.
#
# Without wp arguments they are derived from the worktree name: wp1-2 -> wp1 wp2, wp8 -> wp8.
# Package "sim" is the simulator of dm-current (test/sim, test/test_sim_*, 07-simulator.md).
# Owned directories are mirrored (files that disappeared in the worktree are removed here as well).
# Overwritten and removed files are backed up first. platformio.ini is never copied (shared file).
set -euo pipefail

usage() { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

DRY_RUN=0
RUN_TESTS=1
WITH_HEADERS=0
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) DRY_RUN=1; shift ;;
    --no-test) RUN_TESTS=0; shift ;;
    --with-headers) WITH_HEADERS=1; shift ;;
    -h|--help) usage ;;
    --) shift; break ;;
    -*) echo "unknown option: $1" >&2; usage ;;
    *) break ;;
  esac
done
[ $# -ge 1 ] || usage

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$(cd "$1" && pwd)"
shift
[ "$SRC" != "$REPO" ] || { echo "worktree is this checkout" >&2; exit 2; }

WPS="$*"
if [ -z "$WPS" ]; then
  name="$(basename "$SRC")"
  case "$name" in
    wp[0-9]*-[0-9]*) a="${name#wp}"; lo="${a%-*}"; hi="${a#*-}"; for ((i = lo; i <= hi; i++)); do WPS="$WPS wp$i"; done ;;
    wp[0-9]*) WPS="$name" ;;
    *) echo "cannot derive work packages from '$name'; pass them explicitly" >&2; exit 2 ;;
  esac
fi

R=src/helpers/rdm
owned() {
  case "$1" in
    wp0) echo "$R/RdmTypes.h $R/RdmConfig.h $R/RdmCrypto.h $R/RdmCodec.h $R/RdmStorage.h $R/RdmClock.h $R/RdmContacts.h
               $R/RdmOutbox.h $R/RdmInbox.h $R/RdmFetcher.h $R/RdmNode.h $R/RdmChatMesh.h $R/MailboxCore.h
               test/rdm_support/MemFileIO.h test/rdm_support/MemFileIO.cpp test/rdm_support/SimFileIO.h
               test/test_rdm_headers/" ;;
    wp1) echo "$R/RdmCrypto.cpp $R/RdmCodec.cpp test/test_rdm_crypto/ test/test_rdm_codec/" ;;
    wp2) echo "$R/RdmStorage.cpp $R/RdmClock.cpp $R/RdmContacts.cpp $R/ArduinoFileIO.h $R/ArduinoFileIO.cpp
               test/test_rdm_storage/ test/test_rdm_clock/ test/test_rdm_contacts/" ;;
    wp3) echo "$R/RdmOutbox.cpp test/test_rdm_outbox/" ;;
    wp4) echo "$R/RdmInbox.cpp $R/RdmFetcher.cpp test/test_rdm_inbox/ test/test_rdm_fetcher/" ;;
    wp5) echo "$R/RdmNode.cpp $R/RdmChatMesh.cpp src/Mesh.cpp src/helpers/BaseChatMesh.h src/helpers/BaseChatMesh.cpp
               test/test_rdm_node/ test/rdm_support/RdmSimCompanion.h test/rdm_support/RdmSimCompanion.cpp" ;;
    wp6) echo "examples/companion_radio/main.cpp examples/companion_radio/MyMesh.h examples/companion_radio/MyMesh.cpp
               examples/companion_radio/DataStore.h examples/companion_radio/DataStore.cpp
               examples/companion_radio/RdmCompanionProto.h
               variants/heltec_v3/platformio.ini variants/rak4631/platformio.ini variants/lilygo_tbeam_SX1262/platformio.ini
               test/test_rdm_companion_proto/" ;;
    wp7) echo "$R/MailboxCore.cpp examples/mailbox_server/main.cpp examples/mailbox_server/MailboxMesh.h
               examples/mailbox_server/MailboxMesh.cpp examples/mailbox_server/SerialPiBackend.h
               examples/mailbox_server/SerialPiBackend.cpp test/test_rdm_mailbox_core/
               test/rdm_support/RdmSimMailbox.h test/rdm_support/RdmSimMailbox.cpp" ;;
    wp8) echo "examples/mailbox_server/mbxd/ test/rdm_vectors/ test/rdm_support/MemMailboxBackend.h
               test/rdm_support/MemMailboxBackend.cpp test/test_rdm_mailbox_conformance/" ;;
    wp9) echo "test/test_rdm_scenarios/ test/rdm_support/RdmSimApp.h test/rdm_support/RdmScenario.h
               test/rdm_support/RdmScenario.cpp test/rdm_support/SubprocessBackend.h test/rdm_support/SubprocessBackend.cpp" ;;
    wp10) echo "tools/rdm/check-upstream-identical.sh tools/rdm/run-all-tests.sh" ;;
    sim) echo "test/sim/ test/test_sim_dm/ test/test_sim_companion/ test/test_sim_infra/ docs/reliable-dm/07-simulator.md" ;;   # dm-current
    *) return 1 ;;
  esac
}

module_headers() {
  case "$1" in
    wp1) echo "$R/RdmCrypto.h $R/RdmCodec.h" ;;
    wp2) echo "$R/RdmStorage.h $R/RdmClock.h $R/RdmContacts.h" ;;
    wp3) echo "$R/RdmOutbox.h" ;;
    wp4) echo "$R/RdmInbox.h $R/RdmFetcher.h" ;;
    wp5) echo "$R/RdmNode.h $R/RdmChatMesh.h" ;;
    wp7) echo "$R/MailboxCore.h" ;;
    *) echo "" ;;
  esac
}

OWNED=""
for wp in $WPS; do
  paths="$(owned "$wp")" || { echo "unknown work package: $wp" >&2; exit 2; }
  OWNED="$OWNED $paths"
done

TMPDIR="${TMPDIR:-/tmp}"
BACKUP="${TMPDIR%/}/rdm-integrate/$(date +%Y%m%d-%H%M%S)"
n_new=0; n_changed=0; n_same=0; n_removed=0; n_missing=0

backup() {
  mkdir -p "$BACKUP/$(dirname "$1")"
  cp -p "$REPO/$1" "$BACKUP/$1"
}

copy_file() {   # $1 = repo-relative path
  local rel="$1" src="$SRC/$1" dst="$REPO/$1"
  if [ ! -f "$dst" ]; then
    echo "  new      $rel"; n_new=$((n_new + 1))
  elif cmp -s "$src" "$dst"; then
    n_same=$((n_same + 1)); return
  else
    echo "  changed  $rel"; n_changed=$((n_changed + 1))
    [ $DRY_RUN -eq 1 ] || backup "$rel"
  fi
  if [ $DRY_RUN -eq 0 ]; then
    mkdir -p "$(dirname "$dst")"
    cp -p "$src" "$dst"
  fi
}

remove_file() {
  echo "  removed  $1"; n_removed=$((n_removed + 1))
  if [ $DRY_RUN -eq 0 ]; then backup "$1"; rm -f "$REPO/$1"; fi
}

echo "worktree: $SRC"
echo "packages: $WPS"
[ $DRY_RUN -eq 1 ] && echo "dry run: nothing is copied, no tests are run"

for p in $OWNED; do
  case "$p" in
    */)
      d="${p%/}"
      if [ -d "$SRC/$d" ]; then
        while IFS= read -r f; do copy_file "$d/${f#./}"; done < <(cd "$SRC/$d" && find . -type f ! -name '.DS_Store' ! -name '*.pyc' ! -path '*/__pycache__/*' ! -path '*/.pytest_cache/*' | sort)
        if [ -d "$REPO/$d" ]; then
          while IFS= read -r f; do
            [ -f "$SRC/$d/${f#./}" ] || remove_file "$d/${f#./}"
          done < <(cd "$REPO/$d" && find . -type f ! -name '.DS_Store' ! -name '*.pyc' ! -path '*/__pycache__/*' ! -path '*/.pytest_cache/*' | sort)
        fi
      else
        echo "  missing  $p (not in worktree)"; n_missing=$((n_missing + 1))
      fi
      ;;
    *)
      if [ -f "$SRC/$p" ]; then copy_file "$p"; else echo "  missing  $p (not in worktree)"; n_missing=$((n_missing + 1)); fi
      ;;
  esac
done

TAKEN_HEADERS=""
if [ $WITH_HEADERS -eq 1 ]; then
  for wp in $WPS; do TAKEN_HEADERS="$TAKEN_HEADERS $(module_headers "$wp")"; done
  overlay="$(mktemp -d)"
  cp "$REPO/$R"/*.h "$overlay/"
  for h in $TAKEN_HEADERS; do [ -f "$SRC/$h" ] && cp "$SRC/$h" "$overlay/"; done
  if ! python3 "$REPO/tools/rdm/check-headers.py" "$overlay"; then
    rm -rf "$overlay"
    echo "refusing --with-headers: the worktree headers change the fixed public API (06 par. 3)" >&2
    exit 1
  fi
  rm -rf "$overlay"
  for h in $TAKEN_HEADERS; do
    if [ -f "$SRC/$h" ]; then copy_file "$h"; else echo "  missing  $h (not in worktree)"; n_missing=$((n_missing + 1)); fi
  done
fi

# Ownership checks: headers are WP0's, and a worker may only change its own files.
drift=""
for p in $(owned wp0); do
  case "$p" in */) continue ;; esac
  case " $WPS " in *" wp0 "*) continue ;; esac
  case " $TAKEN_HEADERS " in *" $p "*) continue ;; esac
  if [ -f "$SRC/$p" ] && [ -f "$REPO/$p" ] && ! cmp -s "$SRC/$p" "$REPO/$p"; then drift="$drift $p"; fi
done
if [ -n "$drift" ]; then
  echo "WARNING: the worktree changed WP0 files (not copied; public API changes go through the tech lead):"
  for p in $drift; do echo "  $p"; done
fi

if git -C "$SRC" rev-parse --git-dir >/dev/null 2>&1; then
  foreign=""
  while IFS= read -r line; do
    f="${line:3}"
    ok=0
    for p in $OWNED $(owned wp0) $TAKEN_HEADERS; do
      case "$p" in
        */) case "$f" in "$p"*) ok=1 ;; esac ;;
        *) [ "$f" = "$p" ] && ok=1 ;;
      esac
    done
    case "$f" in platformio.ini|docs/*|test/sim/*|test/test_sim_*) ok=1 ;; esac
    # copies of files already in this checkout (the foundation, other packages) are not a problem
    if [ $ok -eq 0 ] && [ -f "$REPO/$f" ] && cmp -s "$SRC/$f" "$REPO/$f"; then ok=1; fi
    [ $ok -eq 1 ] || foreign="$foreign $f"
  done < <(git -C "$SRC" status --porcelain --untracked-files=all)
  if [ -n "$foreign" ]; then
    echo "WARNING: changes in the worktree outside the owned files of $WPS (not copied):"
    for f in $foreign; do echo "  $f"; done
  fi
fi
if [ -f "$SRC/platformio.ini" ] && ! cmp -s "$SRC/platformio.ini" "$REPO/platformio.ini"; then
  echo "NOTE: platformio.ini in the worktree differs from this checkout; merge by hand if needed."
fi

echo "summary: $n_new new, $n_changed changed, $n_removed removed, $n_same unchanged, $n_missing missing"
[ $DRY_RUN -eq 1 ] || [ $((n_changed + n_removed)) -eq 0 ] || echo "backup of overwritten/removed files: $BACKUP"

if [ $DRY_RUN -eq 1 ]; then
  echo "would run: tools/rdm/run-all-tests.sh"
  exit 0
fi
[ $RUN_TESTS -eq 1 ] || exit 0

exec "$REPO/tools/rdm/run-all-tests.sh"
