#!/bin/zsh
set -u

if (( $# != 4 )); then
  print -u2 "usage: $0 <executable> <fixture-directory> <exact-pass-marker> <timeout-seconds>"
  exit 64
fi

executable="${1:A}"
fixture="${2:A}"
marker="$3"
timeout_seconds="$4"
if [[ ! -x "$executable" || ! -f "$fixture/startup.tjs" ||
      ! "$timeout_seconds" =~ '^[1-9][0-9]*$' ]]; then
  print -u2 "plugin-smoke: invalid executable, fixture, or timeout"
  exit 66
fi

log_dir="$(mktemp -d "${TMPDIR:-/tmp}/krkr-plugin-smoke.XXXXXX")" || exit 70
stdout_log="$log_dir/stdout.log"
stderr_log="$log_dir/stderr.log"
combined_log="$log_dir/combined.log"
failure_pattern='PLUGIN_ASSERT_FAIL|script exception|An exception occurred|Member .* does not exist|cannot load (plugin|module)|EmotePlayer::clear: argument carries no tTJSNI_BaseLayer|Segmentation fault|Abort trap|\[critical\]|\[fatal\]'

print "smoke_executable=$executable"
print "smoke_fixture=$fixture"
print "smoke_logs=$log_dir"
"$executable" "$fixture" >"$stdout_log" 2>"$stderr_log" &
process_id=$!
deadline=$(( SECONDS + timeout_seconds ))
verdict=""

while kill -0 "$process_id" 2>/dev/null; do
  command cat "$stdout_log" "$stderr_log" >"$combined_log"
  if command grep -Eiq "$failure_pattern" "$combined_log"; then
    verdict="failure-marker"
    break
  fi
  marker_count="$(command grep -Foc "$marker" "$combined_log" | awk '{s+=$1} END {print s+0}')"
  if (( marker_count > 0 )); then
    verdict="pass-marker"
    break
  fi
  if (( SECONDS >= deadline )); then
    verdict="timeout"
    break
  fi
  sleep 0.1
done

if [[ -z "$verdict" ]]; then verdict="exited-before-pass"; fi
if kill -0 "$process_id" 2>/dev/null; then kill -TERM "$process_id" 2>/dev/null || true; fi
wait "$process_id" 2>/dev/null
process_status=$?
command cat "$stdout_log" "$stderr_log" >"$combined_log"
marker_count="$(command grep -Foc "$marker" "$combined_log" | awk '{s+=$1} END {print s+0}')"

if command grep -Eiq "$failure_pattern" "$combined_log"; then verdict="failure-marker"; fi
if [[ "$verdict" == "exited-before-pass" && "$process_status" == "0" &&
      "$marker_count" == "1" ]]; then
  verdict="pass-marker"
fi
if [[ "$verdict" == "pass-marker" && "$marker_count" == "1" ]]; then
  print "PASS: unique functional marker observed"
  [[ "${KRKR_PLUGIN_SMOKE_KEEP_LOGS:-0}" == "1" ]] || command rm -rf -- "$log_dir"
  exit 0
fi

print -u2 "FAIL: verdict=$verdict marker_count=$marker_count process_status=$process_status logs=$log_dir"
command grep -Ein "$failure_pattern|PLUGIN_SMOKE_PASS|READY|Loading startup script" "$combined_log" | tail -80 >&2 || true
exit 1
