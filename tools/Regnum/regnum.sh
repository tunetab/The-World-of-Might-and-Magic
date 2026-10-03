#!/usr/bin/env bash
# Regnum — запуск на Linux и macOS. Берёт готовую сборку из bin/<система>-<архитектура>;
# если её нет — собирает системным компилятором (clang++ или g++) в bin/<система>.
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
case "$(uname -m)" in
  arm64|aarch64) ARCH=arm64 ;;
  *) ARCH=x64 ;;
esac
case "$(uname -s)" in
  Darwin) OS=macos ;;
  MINGW*|MSYS*|CYGWIN*) exec "$DIR/bin/windows/regnum.exe" "$@" ;;
  *) OS=linux ;;
esac

BIN="$DIR/bin/$OS-$ARCH/regnum"
if [ ! -f "$BIN" ]; then
  BIN="$DIR/bin/$OS/regnum"
  if [ ! -x "$BIN" ]; then
    echo "Сборка Regnum…"
    bash "$DIR/build.sh" regnum
  fi
fi
chmod +x "$BIN" 2>/dev/null || true
# macOS: снять карантин загрузки, иначе Gatekeeper не даст запустить неподписанную программу.
if [ "$OS" = macos ]; then xattr -d com.apple.quarantine "$BIN" 2>/dev/null || true; fi
exec "$BIN" "$@"
