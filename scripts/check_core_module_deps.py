#!/usr/bin/env python3
"""
解析 cpp/core 下各模块 CMakeLists.txt 的 target_link_libraries，
检测 core / archive / tjs2 目标之间的环依赖。

用法:
    python3 scripts/check_core_module_deps.py
    python3 scripts/check_core_module_deps.py --dot   # 输出 Graphviz DOT
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE_DIR = ROOT / "cpp" / "core"

# 只关心这些前缀的目标（忽略 vcpkg / cocos 等外部库）
TRACKED_PREFIXES = (
    "tjs2",
    "core_",
    "archive_",
    "krkr2core",
)

# Phase 3 之前遗留的 domain 互依赖环（见 CORE_MODULE_DECOUPLING.md §2.1）
LEGACY_DOMAIN_MODULES = frozenset({
    "core_base_module",
    "core_visual_module",
    "core_sound_module",
    "core_environ_module",
    "core_utils_module",
    "core_plugin_module",
    "core_movie_module",
    "core_extension_module",
})

# 新分层模块：这些目标之间不应出现环
FOUNDATION_MODULES = frozenset({
    "tjs2",
    "core_io",
    "core_msg",
    "core_storage",
    "core_storage_tool",
    "archive_xp3",
    "archive_zip",
    "archive_7z",
    "archive_tar",
})


def is_tracked(name: str) -> bool:
    return any(name.startswith(p) or name == p for p in TRACKED_PREFIXES)


def parse_project_name(text: str) -> str | None:
    m = re.search(r"project\s*\(\s*(\w+)", text)
    return m.group(1) if m else None


def parse_link_edges(text: str, target: str) -> list[tuple[str, str]]:
    """返回 (from, to, visibility) 边列表。"""
    edges: list[tuple[str, str]] = []
    pattern = re.compile(
        r"target_link_libraries\s*\(\s*"
        r"(?:\$\{PROJECT_NAME\}|" + re.escape(target) + r")"
        r"\s+(PUBLIC|PRIVATE|INTERFACE)([^)]*)\)",
        re.DOTALL,
    )
    for m in pattern.finditer(text):
        deps_block = m.group(2)
        for token in re.findall(r"[\w:]+", deps_block):
            if is_tracked(token):
                edges.append((target, token))
    return edges


def collect_modules() -> dict[str, Path]:
    modules: dict[str, Path] = {}
    for cmake in sorted(CORE_DIR.rglob("CMakeLists.txt")):
        text = cmake.read_text(encoding="utf-8")
        name = parse_project_name(text)
        if name:
            modules[name] = cmake
    # krkr2core 在 cpp/core/CMakeLists.txt
    core_cmake = CORE_DIR / "CMakeLists.txt"
    if core_cmake.exists():
        modules.setdefault("krkr2core", core_cmake)
    return modules


def build_graph(modules: dict[str, Path]) -> dict[str, set[str]]:
    graph: dict[str, set[str]] = {name: set() for name in modules}
    for name, path in modules.items():
        text = path.read_text(encoding="utf-8")
        for _, dep in parse_link_edges(text, name):
            if dep in modules or is_tracked(dep):
                graph.setdefault(name, set()).add(dep)
                graph.setdefault(dep, set())
    return graph


def find_cycles(graph: dict[str, set[str]]) -> list[list[str]]:
    cycles: list[list[str]] = []
    visited: set[str] = set()
    stack: list[str] = []
    on_stack: set[str] = set()

    def dfs(node: str) -> None:
        visited.add(node)
        on_stack.add(node)
        stack.append(node)
        for nei in sorted(graph.get(node, ())):
            if nei not in visited:
                dfs(nei)
            elif nei in on_stack:
                start = stack.index(nei)
                cycle = stack[start:] + [nei]
                cycles.append(cycle)
        stack.pop()
        on_stack.remove(node)

    for node in sorted(graph):
        if node not in visited:
            dfs(node)
    return cycles


def cycle_is_known(cycle: list[str]) -> bool:
    nodes = frozenset(cycle[:-1])
    # 旧 domain 层互环：记录但不阻塞 Phase 1/2/3 增量工作
    if nodes <= LEGACY_DOMAIN_MODULES:
        return True
    return False


def cycle_touches_foundation(cycle: list[str]) -> bool:
    nodes = frozenset(cycle[:-1])
    return bool(nodes & FOUNDATION_MODULES)


def emit_dot(graph: dict[str, set[str]]) -> None:
    print("digraph core_modules {")
    print('  rankdir=LR;')
    print('  node [shape=box, fontname="Helvetica"];')
    for src in sorted(graph):
        for dst in sorted(graph[src]):
            print(f'  "{src}" -> "{dst}";')
    print("}")


def main() -> int:
    parser = argparse.ArgumentParser(description="检查 core 模块 CMake 依赖环")
    parser.add_argument("--dot", action="store_true", help="输出 Graphviz DOT")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="任意环均失败（含遗留 domain 环）",
    )
    args = parser.parse_args()

    modules = collect_modules()
    graph = build_graph(modules)

    if args.dot:
        emit_dot(graph)
        return 0

    print(f"已解析 {len(modules)} 个 CMake 目标\n")

    cycles = find_cycles(graph)
    legacy_cycles = [c for c in cycles if cycle_is_known(c)]
    bad_cycles = [
        c
        for c in cycles
        if not cycle_is_known(c) and cycle_touches_foundation(c)
    ]
    other_cycles = [
        c for c in cycles if not cycle_is_known(c) and not cycle_touches_foundation(c)
    ]

    if cycles:
        print("检测到依赖环:")
        shown: set[tuple[str, ...]] = set()
        for cycle in cycles:
            key = tuple(sorted(set(cycle)))
            if key in shown:
                continue
            shown.add(key)
            if cycle_is_known(cycle):
                tag = "遗留"
            elif cycle_touches_foundation(cycle):
                tag = "新增(含基础层)"
            else:
                tag = "新增"
            print(f"  [{tag}] {' -> '.join(cycle)}")
        print()

    if args.strict and cycles:
        print("错误: --strict 模式下不允许任何依赖环。", file=sys.stderr)
        return 1

    if bad_cycles:
        print(
            "错误: 基础层/归档模块出现新的依赖环，请修复 CMake。",
            file=sys.stderr,
        )
        return 1

    if legacy_cycles:
        print(f"遗留 domain 环 {len({tuple(sorted(set(c))) for c in legacy_cycles})} 组（Phase 4 目标）。")
    if other_cycles:
        print(f"警告: 其他新环 {len(other_cycles)} 条（domain 层，非基础层）。")
    if not cycles:
        print("未检测到依赖环。")
    else:
        print("基础层未引入新环。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
