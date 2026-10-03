#!/usr/bin/env bash
# Regnum — сборка (Windows через Git Bash, Linux, macOS). Без внешних библиотек и систем сборки.
#
#   tools/Regnum/build.sh                 — собрать всё (regnum, regnum-cli, regnum-tests)
#   tools/Regnum/build.sh regnum          — только приложение
#   tools/Regnum/build.sh test-geo        — тесты одного модуля (модуль + зависимости)
#   tools/Regnum/build.sh platform-demo   — окно-демонстрация платформы; bin/<os>/platform-demo --selftest — самопроверка
#   tools/Regnum/build.sh --test [фильтр] — собрать regnum-tests и запустить
#   tools/Regnum/build.sh --target linux-x64 regnum regnum-cli — сборка для другой системы через Zig
#     (цели: linux-x64, linux-arm64, macos-arm64, macos-x64; Zig скачивается в .wmma/toolchains с проверкой SHA-256)
#   --debug | --release (по умолчанию release), --clean, -j N
#
# Компилятор: переменная CXX; на Windows по умолчанию — закреплённый LLVM-MinGW из .wmma/toolchains.
# Результат: tools/Regnum/bin/<os>/ (regnum[.exe], regnum-cli[.exe], regnum-tests[.exe]); при --target — bin/<цель>/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
SRC="$ROOT/src"

CONFIG=release
TARGETS=()
RUN_TESTS=0
TEST_FILTER=()
CLEAN=0
JOBS=""
CROSS=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --target) CROSS="$2"; shift ;;
    --debug) CONFIG=debug ;;
    --release) CONFIG=release ;;
    --clean) CLEAN=1 ;;
    --test) RUN_TESTS=1; shift; while [[ $# -gt 0 && "$1" != -* ]]; do TEST_FILTER+=("$1"); shift; done; continue ;;
    -j) JOBS="$2"; shift ;;
    -*) echo "Неизвестный параметр: $1" >&2; exit 2 ;;
    *) TARGETS+=("$1") ;;
  esac
  shift
done

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) OS=windows; EXE=.exe ;;
  Darwin) OS=macos; EXE= ;;
  *) OS=linux; EXE= ;;
esac
HOST_OS=$OS

# Закреплённый компилятор для Windows (LLVM-MinGW). Скачивается один раз с проверкой SHA-256.
TC_NAME=llvm-mingw-20260908-ucrt-x86_64
TC_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/20260908/$TC_NAME.zip"
TC_SHA=1bcf74d06b724aeecaa6412ca85f5b26fb1da770e7cdcefa9263c9c5c3ad34b6
fetch_windows_toolchain() {
  local dir="$REPO/.wmma/toolchains" zip
  zip="$dir/$TC_NAME.zip"
  mkdir -p "$dir"
  echo "== загрузка компилятора $TC_NAME"
  curl -L --fail -o "$zip.part" "$TC_URL"
  local got
  got="$(sha256sum "$zip.part" | cut -d' ' -f1)"
  if [[ "$got" != "$TC_SHA" ]]; then rm -f "$zip.part"; echo "Контрольная сумма компилятора не совпала" >&2; exit 1; fi
  mv "$zip.part" "$zip"
  (cd "$dir" && unzip -q -o "$TC_NAME.zip")
}

if [[ -z "${CXX:-}" ]]; then
  if [[ $OS == windows ]]; then
    TC="$REPO/.wmma/toolchains/$TC_NAME"
    if [[ ! -x "$TC/bin/clang++.exe" ]] && ! command -v clang++ >/dev/null 2>&1; then fetch_windows_toolchain; fi
    if [[ -x "$TC/bin/clang++.exe" ]]; then CXX="$TC/bin/clang++.exe"; else CXX=clang++; fi
  elif command -v clang++ >/dev/null 2>&1; then CXX=clang++; else CXX=g++; fi
fi
[[ -z "$JOBS" ]] && JOBS="$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 8)"

# Сборка для другой системы: Zig (clang + libc++ + заглушки glibc/libSystem). GUI-библиотеки ОС подключаются
# во время выполнения через dlopen, поэтому системные заголовки целевой ОС не нужны.
ZIG_VER=0.16.0
fetch_zig() {
  local dir="$REPO/.wmma/toolchains" name url sha
  case "$HOST_OS" in
    windows) name="zig-x86_64-windows-$ZIG_VER"; sha=68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e; url="https://ziglang.org/download/$ZIG_VER/$name.zip" ;;
    *) echo "Сборка для другой системы поддерживается с Windows; на Linux и macOS собирайте без --target." >&2; exit 2 ;;
  esac
  ZIG="$dir/$name/zig.exe"
  [[ -x "$ZIG" ]] && return 0
  mkdir -p "$dir"
  echo "== загрузка Zig $ZIG_VER"
  curl -L --fail -o "$dir/$name.zip.part" "$url"
  local got
  got="$(sha256sum "$dir/$name.zip.part" | cut -d' ' -f1)"
  if [[ "$got" != "$sha" ]]; then rm -f "$dir/$name.zip.part"; echo "Контрольная сумма Zig не совпала" >&2; exit 1; fi
  mv "$dir/$name.zip.part" "$dir/$name.zip"
  (cd "$dir" && unzip -q -o "$name.zip")
}
if [[ -n "$CROSS" ]]; then
  case "$CROSS" in
    linux-x64) TRIPLE=x86_64-linux-gnu.2.28; OS=linux ;;
    linux-arm64) TRIPLE=aarch64-linux-gnu.2.28; OS=linux ;;
    macos-arm64) TRIPLE=aarch64-macos.13.3; OS=macos ;;
    macos-x64) TRIPLE=x86_64-macos.13.3; OS=macos ;;
    *) echo "Неизвестная цель: $CROSS (linux-x64, linux-arm64, macos-arm64, macos-x64)" >&2; exit 2 ;;
  esac
  EXE=
  fetch_zig
  # Пути для Zig (родная программа Windows) — в виде D:/..., чтобы их не переписывал MSYS.
  if [[ $HOST_OS == windows ]]; then
    ROOT="$(cygpath -m "$ROOT")"; REPO="$(cygpath -m "$REPO")"; SRC="$ROOT/src"; ZIG="$(cygpath -m "$ZIG")"
  fi
  mkdir -p "$ROOT/.build"
  WRAP="$ROOT/.build/zigcxx-$CROSS.sh"
  printf '#!/usr/bin/env bash
exec %q c++ -target %s "$@"
' "$ZIG" "$TRIPLE" > "$WRAP"
  chmod +x "$WRAP"
  CXX="$WRAP"
fi

BUILD="$ROOT/.build/${CROSS:-$OS}-$CONFIG"
BIN="$ROOT/bin/${CROSS:-$OS}"
[[ $CLEAN == 1 ]] && rm -rf "$BUILD"
mkdir -p "$BUILD/obj" "$BIN"

CXXFLAGS=(-std=c++20 -I"$SRC" -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fno-strict-aliasing)
if [[ $CONFIG == debug ]]; then CXXFLAGS+=(-O0 -g -DRG_DEBUG=1); else CXXFLAGS+=(-O2 -DNDEBUG); fi
LDFLAGS=()
GUI_LDFLAGS=()
case $OS in
  windows)
    CXXFLAGS+=(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
    LDFLAGS+=(-static -lgdi32 -luser32 -lshell32 -lole32 -luuid -lcomdlg32 -limm32 -ldwmapi -lshcore -lwinmm)
    GUI_LDFLAGS+=(-mwindows)
    PLATFORM=win32 ;;
  linux)
    CXXFLAGS+=(-pthread)
    LDFLAGS+=(-pthread -ldl)
    PLATFORM=x11 ;;
  macos)
    LDFLAGS+=(-ldl)
    PLATFORM=cocoa ;;
esac
if [[ -n "$CROSS" && $CONFIG == release ]]; then LDFLAGS+=(-s); fi   # без отладочной информации

# Модули и зависимости. Исходники модуля: src/<модуль>/**/*.cpp
deps_of() {
  case "$1" in
    base) echo "" ;;
    codec) echo "base" ;;
    gfx) echo "base codec" ;;
    core) echo "base codec" ;;
    geo) echo "base codec core" ;;
    rules) echo "base codec core geo" ;;
    map) echo "base codec gfx core geo rules" ;;
    ui) echo "base codec gfx" ;;
    app) echo "base codec gfx core geo rules map ui" ;;
    *) return 1 ;;
  esac
}
ALL_MODULES="base codec gfx core geo rules map ui app"

module_sources() {  # $1 = модуль
  local d="$SRC/$1"
  [[ -d "$d" ]] || return 0
  find "$d" -name '*.cpp' | sort
}

platform_sources() {  # $1 = win32|x11|cocoa|headless
  local f="$SRC/platform/$1.cpp"
  [[ -f "$f" ]] && echo "$f"
  local c="$SRC/platform/common.cpp"
  [[ -f "$c" ]] && echo "$c"
  return 0
}

resolve_modules() {  # список модулей с зависимостями, в порядке ALL_MODULES
  local want=" $* "
  local changed=1
  while [[ $changed == 1 ]]; do
    changed=0
    for m in $want; do
      for d in $(deps_of "$m"); do
        [[ "$want" == *" $d "* ]] || { want="$want$d "; changed=1; }
      done
    done
  done
  local out=""
  for m in $ALL_MODULES; do [[ "$want" == *" $m "* ]] && out="$out $m"; done
  echo $out
}

obj_of() { local rel="${1#$SRC/}"; echo "$BUILD/obj/${rel%.cpp}.o"; }

needs_build() {  # $1 = src, $2 = obj
  local src="$1" obj="$2" dep="$2.d"
  [[ -f "$obj" ]] || return 0
  [[ "$src" -nt "$obj" ]] && return 0
  [[ -f "$dep" ]] || return 0
  local f
  while IFS= read -r f; do
    [[ -n "$f" && "$f" != *: && -f "$f" && "$f" -nt "$obj" ]] && return 0
  done < <(sed -e 's/\\ /\x01/g' -e 's/\\$//' -e 's/^obj://' "$dep" | tr ' \t' '\n\n' | sed -e '/^$/d' -e 's/\x01/ /g')
  return 1
}

# Скрипт компиляции одного файла (пути с пробелами и кириллицей экранируются через %q).
write_cc() {
  {
    echo '#!/usr/bin/env bash'
    printf 'CXX=%q\n' "$CXX"
    printf 'FLAGS=('; printf '%q ' "${CXXFLAGS[@]}"; echo ')'
    printf 'SRC=%q\nBUILD=%q\n' "$SRC" "$BUILD"
    cat <<'EOS'
s="$1"; rel="${s#$SRC/}"; o="$BUILD/obj/${rel%.cpp}.o"
mkdir -p "$(dirname "$o")"
if ! "$CXX" "${FLAGS[@]}" -MMD -MT obj -MF "$o.d" -c "$s" -o "$o" 2> "$o.log"; then
  echo "ОШИБКА: $rel"; cat "$o.log"; rm -f "$o"; exit 255
fi
if [[ -s "$o.log" ]]; then echo "-- предупреждения: $rel"; cat "$o.log"; fi
exit 0
EOS
  } > "$BUILD/cc.sh"
}

compile_all() {  # аргументы — исходники
  local todo=()
  local s o
  for s in "$@"; do
    o="$(obj_of "$s")"
    if needs_build "$s" "$o"; then todo+=("$s"); fi
  done
  [[ ${#todo[@]} -eq 0 ]] && return 0
  echo "== компиляция: ${#todo[@]} файл(ов), $JOBS потоков"
  write_cc
  printf '%s\0' "${todo[@]}" | xargs -0 -n 1 -P "$JOBS" bash "$BUILD/cc.sh" || { echo "Сборка остановлена из-за ошибок."; exit 1; }
}

link_exe() {  # $1 = имя, $2 = gui|console, остальные — исходники
  local name="$1" kind="$2"; shift 2
  compile_all "$@"
  local objs=() s
  for s in "$@"; do objs+=("$(obj_of "$s")"); done
  local out="$BIN/$name$EXE"
  local extra=()
  [[ $kind == gui ]] && extra+=("${GUI_LDFLAGS[@]}")
  if [[ $OS == windows && $kind == gui && -f "$ROOT/assets/regnum.rc" ]]; then
    local rc_obj="$BUILD/obj/regnum_res.o"
    local windres; windres="$(dirname "$CXX")/windres.exe"
    if [[ ! -f "$rc_obj" || "$ROOT/assets/regnum.rc" -nt "$rc_obj" ]]; then
      (cd "$ROOT/assets" && "$windres" regnum.rc -O coff -o "$rc_obj")
    fi
    objs+=("$rc_obj")
  fi
  echo "== сборка $name$EXE"
  "$CXX" "${objs[@]}" -o "$out" "${extra[@]}" "${LDFLAGS[@]}"
}

sources_for_modules() { local m; for m in "$@"; do module_sources "$m"; done; }
nontest() { grep -v '/tests/' || true; }

build_target() {
  local t="$1"
  case "$t" in
    regnum)
      S=(); while IFS= read -r _l; do [[ -n "$_l" ]] && S+=("$_l"); done < <(sources_for_modules $(resolve_modules app); platform_sources $PLATFORM)
      [[ -f "$SRC/app/main.cpp" ]] || { echo "Нет src/app/main.cpp — приложение ещё не собрано из модулей."; return 0; }
      link_exe regnum gui "${S[@]}" ;;
    regnum-cli)
      S=(); while IFS= read -r _l; do [[ -n "$_l" ]] && S+=("$_l"); done < <(sources_for_modules $(resolve_modules map); find "$SRC/cli" -name '*.cpp' 2>/dev/null | sort; platform_sources headless)
      [[ -f "$SRC/cli/cli_main.cpp" ]] || { echo "Нет src/cli/cli_main.cpp — пропуск regnum-cli."; return 0; }
      link_exe regnum-cli console "${S[@]}" ;;
    regnum-tests)
      S=(); while IFS= read -r _l; do [[ -n "$_l" ]] && S+=("$_l"); done < <(sources_for_modules $(resolve_modules app) | grep -v '/app/main.cpp' ; platform_sources headless; find "$SRC/tests" -name '*.cpp' | sort)
      link_exe regnum-tests console "${S[@]}" ;;
    test-*)
      local m="${t#test-}"
      deps_of "$m" >/dev/null || { echo "Неизвестный модуль: $m"; exit 2; }
      S=(); while IFS= read -r _l; do [[ -n "$_l" ]] && S+=("$_l"); done < <(sources_for_modules $(resolve_modules "$m") | grep -v '/app/main.cpp'; [[ $m == ui || $m == app || $m == map ]] && platform_sources headless; echo "$SRC/tests/test_main.cpp"; find "$SRC/tests" -name "test_${m}*.cpp" | sort)
      link_exe "regnum-test-$m" console "${S[@]}" ;;
    platform-demo)
      S=(); while IFS= read -r _l; do [[ -n "$_l" ]] && S+=("$_l"); done < <(sources_for_modules base; platform_sources $PLATFORM; find "$SRC/platform/demo" -name '*.cpp' | sort)
      link_exe platform-demo console "${S[@]}" ;;
    *) echo "Неизвестная цель: $t"; exit 2 ;;
  esac
}

if [[ ${#TARGETS[@]} -eq 0 ]]; then
  if [[ $RUN_TESTS == 1 ]]; then TARGETS=(regnum-tests); else TARGETS=(regnum regnum-cli regnum-tests); fi
fi
echo "Regnum: $OS/$CONFIG, компилятор: $CXX"
for t in "${TARGETS[@]}"; do build_target "$t"; done

if [[ $RUN_TESTS == 1 ]]; then
  for t in "${TARGETS[@]}"; do
    case "$t" in
      regnum-tests) exe="$BIN/regnum-tests$EXE" ;;
      test-*) exe="$BIN/regnum-test-${t#test-}$EXE" ;;
      *) continue ;;
    esac
    echo "== запуск $(basename "$exe")"
    (cd "$REPO" && "$exe" "${TEST_FILTER[@]}")
  done
fi
