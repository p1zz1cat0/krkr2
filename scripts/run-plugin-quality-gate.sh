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

# Session logs and macos build trees live under the host project's .work
# directory when YOGHOURT_WORK_BUILD_ROOT is provided (the bootstrap
# gateway); otherwise walk up from this fork checkout to the nearest
# project .work so locked-source checkouts (under .work/build/sources/)
# and developer worktrees (Vendor/krkr2) resolve identically.
work_root_build="${YOGHOURT_WORK_BUILD_ROOT:-}"
if [[ -z "$work_root_build" ]]; then
  local candidate="${repo_root:A}"
  while [[ "$candidate" != "/" ]]; do
    if [[ -d "$candidate/.work" ]]; then
      work_root_build="$candidate/.work/build"
      break
    fi
    candidate="${candidate:h}"
  done
fi
work_root_build="${work_root_build:-$repo_root/out}"
log_dir="${YOGHOURT_QUALITY_LOG_DIR:-${work_root_build:h}/logs/quality-gate}"
mkdir -p -- "$log_dir"

# 环境前置检查。这两项缺失时 CMake 的报错离根因很远（toolchain 路径拼成
# "/scripts/buildsystems/vcpkg.cmake"、bison 报语法要求），先在这里失败并
# 给出可执行的修法。
if [[ -z "${VCPKG_ROOT:-}" ]]; then
  # 与 Scripts/bootstrap/common.sh 的默认值保持一致。
  if [[ -d "$work_root_build/vcpkg/scripts/buildsystems" ]]; then
    export VCPKG_ROOT="$work_root_build/vcpkg"
  else
    print -u2 "quality-gate: VCPKG_ROOT 未设置，且 $work_root_build/vcpkg 不存在。"
    print -u2 "  先跑一次 Scripts/bootstrap-runtimes.sh build krkr2，或显式导出 VCPKG_ROOT。"
    exit 78
  fi
fi
if [[ ! -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]]; then
  print -u2 "quality-gate: VCPKG_ROOT=$VCPKG_ROOT 下找不到 scripts/buildsystems/vcpkg.cmake"
  exit 78
fi

# tjs2 的 .y 文件要求 bison >= 3.8.2；macOS 自带的是 2.3。
required_bison="3.8.2"
bison_ok=0
for candidate in "${BISON:-}" bison /opt/homebrew/opt/bison/bin/bison /usr/local/opt/bison/bin/bison; do
  [[ -z "$candidate" ]] && continue
  bison_path="$(command -v "$candidate" 2>/dev/null || true)"
  [[ -z "$bison_path" ]] && continue
  bison_version="$("$bison_path" --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1)"
  [[ -z "$bison_version" ]] && continue
  if [[ "$(printf '%s\n' "$required_bison" "$bison_version" | sort -V | head -1)" == "$required_bison" ]]; then
    export PATH="${bison_path:h}:$PATH"
    bison_ok=1
    break
  fi
done
if (( ! bison_ok )); then
  print -u2 "quality-gate: 需要 bison >= $required_bison，PATH 上只有更旧的版本（macOS 自带 2.3）。"
  print -u2 "  brew install bison，或导出 BISON=<路径> 指向 3.8.2 以上的可执行文件。"
  exit 78
fi
(setopt NULL_GLOB; rm -f -- "$log_dir"/*.log)
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
relay_dir="${YOGHOURT_SURFACE_RELAY_DIR:-${repo_root:h:h}/RuntimeSupport/SurfaceRelay}"
for configuration in debug release; do
  build_dir="$work_root_build/krkr2-gate/$configuration"
  run_logged "$configuration-configure" env VCPKG_ROOT="${VCPKG_ROOT:-}" \
    cmake -S "$repo_root" -B "$build_dir" -G Ninja \
      -DMACOS=ON \
      -DVCPKG_TARGET_TRIPLET="${VCPKG_TARGET_TRIPLET:-arm64-osx-static}" \
      -DCMAKE_BUILD_TYPE="${configuration}" \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DYOGHOURT_SPATIAL_PRESENTER_DIR="$spatial_dir" \
      -DYOGHOURT_SURFACE_RELAY_DIR="$relay_dir" \
      -DENABLE_TESTS=ON -DBUILD_TOOLS=OFF
  targets=("${(@f)$(manifest_query buildTarget | sort -u)}")
  # plugin-tests keeps the macOS ctest stage on the same unit-test set the
  # Linux stage already builds (line above in the linux branch). Without it
  # only whatever binaries happened to exist got discovered, so labelled
  # suites silently never ran here.
  run_logged "$configuration-build" cmake --build "$build_dir" --target krkr2 plugin-tests core-tests "${targets[@]}"
  run_logged "$configuration-ctest" ctest --test-dir "$build_dir" -L plugin --output-on-failure
  # core-tests 与 -L core 必须成对出现：只加目标则用例被构建但不运行，只加
  # 标签则 ctest 找不到二进制。此前两者都缺，core/{movie,visual,tjs2,telemetry}
  # 四个套件在门里完全不可见。
  run_logged "$configuration-ctest-core" ctest --test-dir "$build_dir" -L core --output-on-failure

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
