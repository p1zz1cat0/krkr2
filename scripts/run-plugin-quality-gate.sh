#!/bin/zsh
set -eu

repo_root="${0:A:h:h}"
manifest="$repo_root/tests/plugin-quality-gates.json"
mode=""
plugin=""
skip_build=0
if [[ "${1:-}" == "--all" && $# == 1 ]]; then
  mode=all
elif [[ "${1:-}" == "--plugin" && $# == 2 ]]; then
  mode=plugin
  plugin="$2"
elif [[ "${1:-}" == "--scanner-only" && $# == 1 ]]; then
  mode=all
  skip_build=1
else
  print -u2 "usage: $0 --all | --plugin <name> | --scanner-only"
  exit 64
fi

log_dir="$(mktemp -d "${TMPDIR:-/tmp}/krkr-plugin-quality.XXXXXX")" || exit 70
print "quality_gate_logs=$log_dir"

run_logged() {
  local name="$1"
  shift
  if ! "$@" >"$log_dir/$name.log" 2>&1; then
    print -u2 "FAIL: $name (log=$log_dir/$name.log)"
    tail -80 "$log_dir/$name.log" >&2 || true
    exit 1
  fi
  print "PASS: $name"
}

manifest_query() {
  python3 - "$manifest" "$mode" "$plugin" "$1" <<'PY'
import json,sys
d=json.load(open(sys.argv[1],encoding='utf-8'))
items=d['plugins']
if sys.argv[2]=='plugin': items=[x for x in items if x['name']==sys.argv[3]]
if not items: raise SystemExit('unknown plugin: '+sys.argv[3])
field=sys.argv[4]
for x in items:
    value=x[field]
    print('\t'.join(value) if isinstance(value,list) else value)
PY
}

run_logged scanner-self-test python3 -m unittest discover \
  -s "$repo_root/tests/plugin-safety" -p 'test_*.py'
if [[ "$mode" == all ]]; then
  run_logged scanner "$repo_root/scripts/audit-plugin-safety.py" --root "$repo_root"
else
  paths=("${(@f)$(manifest_query sourceRoots)}")
  run_logged scanner "$repo_root/scripts/audit-plugin-safety.py" --root "$repo_root" --paths "${paths[@]}"
fi
(( skip_build )) && exit 0

if [[ "$(uname -s)" == Linux ]]; then
  build_dir="$repo_root/out/linux/plugin-quality"
  run_logged linux-configure cmake -S "$repo_root" -B "$build_dir" -G Ninja \
    -DLINUX=ON -DENABLE_TESTS=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Release
  run_logged linux-plugin-tests cmake --build "$build_dir" --target plugin-tests
  run_logged linux-ctest ctest --test-dir "$build_dir" -L plugin --output-on-failure
  exit 0
fi

if [[ "$(uname -s)" != Darwin ]]; then
  print -u2 "quality gate supports Linux and macOS"
  exit 2
fi

spatial_dir="${YOGHOURT_SPATIAL_PRESENTER_DIR:-${repo_root:h:h}/RuntimeSupport/SpatialPresenter}"
for configuration in debug release; do
  if [[ "$configuration" == debug ]]; then preset="MacOS Debug Config"; else preset="MacOS Release Config"; fi
  build_dir="$repo_root/out/macos/$configuration"
  run_logged "$configuration-configure" cmake --preset "$preset" \
    -DYOGHOURT_SPATIAL_PRESENTER_DIR="$spatial_dir" -DENABLE_TESTS=ON -DBUILD_TOOLS=OFF
  targets=("${(@f)$(manifest_query buildTarget | sort -u)}")
  run_logged "$configuration-build" cmake --build "$build_dir" --target krkr2 "${targets[@]}"
  run_logged "$configuration-ctest" ctest --test-dir "$build_dir" -L plugin --output-on-failure

  executable="$build_dir/bin/krkr2/krkr2.app/Contents/MacOS/krkr2"
  if [[ "$configuration" == release ]]; then
    print "pre_package_release_executable=${executable:A}"
    anchors=("${(@f)$(manifest_query anchor)}")
    for anchor in "${anchors[@]}"; do
      if ! nm "$executable" | grep -Fq "$anchor"; then
        print -u2 "FAIL: missing release anchor $anchor"
        exit 1
      fi
    done
    print "PASS: release-unstripped-anchors"
  fi

  while IFS=$'\t' read -r fixture marker timeout smoke_kind; do
    run_logged "$configuration-smoke-${fixture:t}" "$repo_root/scripts/run-plugin-smoke.sh" \
      "$executable" "$repo_root/$fixture" "$marker" "$timeout"
  done < <(python3 - "$manifest" "$mode" "$plugin" <<'PY'
import json,sys
d=json.load(open(sys.argv[1],encoding='utf-8'))['plugins']
if sys.argv[2]=='plugin': d=[x for x in d if x['name']==sys.argv[3]]
for x in d: print(x['fixture'],x['marker'],x['timeoutSeconds'],x.get('smokeKind','headless'),sep='\t')
PY
)
done

print "PLUGIN_QUALITY_GATE_PASS mode=$mode"
