#!/usr/bin/env bash
# Regnum — запуск двойным щелчком в Finder (macOS).
exec bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/regnum.sh" "$@"
