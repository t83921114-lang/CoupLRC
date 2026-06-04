#!/usr/bin/env bash
# 在本机重新生成 compile_commands.json，供 clangd / C++ 扩展跳转定义使用
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ROOT}/project/cmake/build"
mkdir -p "${BUILD}"
cmake -S "${ROOT}/project" -B "${BUILD}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cp -f "${BUILD}/compile_commands.json" "${ROOT}/compile_commands.json"
ln -sf cmake/build/compile_commands.json "${ROOT}/project/compile_commands.json"
echo "Updated: ${ROOT}/compile_commands.json"
echo "Entries: $(python3 -c "import json; print(len(json.load(open('${ROOT}/compile_commands.json'))))")"
