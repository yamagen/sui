#!/bin/sh
set -eu

SUI=./sui
LEDGER=ledger.dat

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

full=$(printf '%s\n' '男につきて' | "$SUI" "$LEDGER")
printf '%s\n' "$full" |
  jq -e 'select(.start == 0 and .end == 15 and (.records | length) == 4)' >/dev/null ||
  fail "full path"

partial=$(printf '%s\n' '男につきて何か' | "$SUI" "$LEDGER")
printf '%s\n' "$partial" |
  jq -e 'select(.start == 0 and .end == 15 and (.records | length) == 4)' >/dev/null ||
  fail "partial path"

alternatives=$(printf '%s\n' '男女' | "$SUI" "$LEDGER")
[ "$(printf '%s\n' "$alternatives" | jq -s 'length')" -eq 2 ] ||
  fail "alternative path count"
printf '%s\n' "$alternatives" |
  jq -s -e '
    any(.[]; (.records | length) == 1 and .records[0].word == "男女") and
    any(.[]; (.records | length) == 2 and
             .records[0].word == "男" and .records[1].word == "女")
  ' >/dev/null ||
  fail "alternative paths"

unconnected=$(printf '%s\n' '男の' | "$SUI" "$LEDGER")
printf '%s\n' "$unconnected" |
  jq -e '
    select(.start == 0 and .end == 3 and
           (.records | length) == 1 and .records[0].word == "男")
  ' >/dev/null ||
  fail "unconnected continuation"

echo "PASS: sui regression tests"
