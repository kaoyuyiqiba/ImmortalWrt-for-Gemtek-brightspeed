#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
tmp="$(mktemp -d)"
trap 'rm -f -- "$tmp/test_stock_pcie"; rmdir -- "$tmp"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wpedantic -g \
    -fsanitize=undefined -fno-sanitize-recover=undefined \
    stock_pcie.c test_stock_pcie.c -o "$tmp/test_stock_pcie"
"$tmp/test_stock_pcie"
