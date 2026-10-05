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

best_compat=$(printf '%s\n' '男につきて' | "$SUI" -b "$LEDGER")
printf '%s\n' "$best_compat" |
  jq -e 'select(.start == 0 and .end == 15 and (.records | length) == 4)' >/dev/null ||
  fail "best option compatibility"

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

unresolved=$(printf '%s\n' '男につきて何か' | "$SUI" -u "$LEDGER")
case "$unresolved" in
  '!'{*) ;;
  *) fail "unresolved marker" ;;
esac
printf '%s\n' "${unresolved#!}" |
  jq -e 'select(.start == 15 and .end == 21 and .text == "何か")' >/dev/null ||
  fail "unresolved tail"

resolved=$(printf '%s\n' '男につきて' | "$SUI" -u "$LEDGER")
[ -z "$resolved" ] ||
  fail "resolved input produced unresolved output"

context=$(printf '%s\n' '{"text":"男につきて何か","provenance":{"corpus":"taketori","id":37,"token":5}}' |
  "$SUI" -c tests/ledger-config.json -u "$LEDGER")
case "$context" in
  '!'{*) ;;
  *) fail "unresolved provenance marker" ;;
esac
printf '%s\n' "${context#!}" |
  jq -e '
    select(.start == 15 and .end == 21 and .text == "何か" and
           .provenance.corpus == "taketori" and
           .provenance.id == 37 and
           .provenance.token == 5)
  ' >/dev/null ||
  fail "unresolved provenance context"

candy=$(mktemp)
candy_config=$(mktemp)
trap 'rm -f "$candy" "$candy_config"' EXIT
jq --arg filename "$candy" '
  .schema += {
    "word": "string",
    "lemma": "string",
    "kana": "string",
    "lemma-kana": "string",
    "romaji": "string",
    "lemma-romaji": "string",
    "gloss": "string",
    "pos": "string",
    "ku": "string"
  } |
  .candy.filename = $filename |
  .ledger.ignore = ["、", "。"]
' tests/ledger-config.json >"$candy_config"

printf '%s\n' '{"text":"男につきて何か","provenance":{"corpus":"taketori","id":37,"token":5}}' |
  "$SUI" -c "$candy_config" -a "$LEDGER" >/dev/null

candy_line=$(cat "$candy")
case "$candy_line" in
  '!'{*) ;;
  *) fail "candy marker" ;;
esac
printf '%s\n' "${candy_line#!}" |
jq -e '
  select(.start == 15 and .end == 21 and .text == "何か" and
         .provenance.corpus == "taketori" and
         .provenance.id == 37 and
         .provenance.token == 5 and
         .word == "" and
         .lemma == "" and
         .kana == "" and
         ."lemma-kana" == "" and
         .romaji == "" and
         ."lemma-romaji" == "" and
         .gloss == "" and
         .pos == "" and
         .ku == "")
' >/dev/null ||
  fail "candy append"

reused=$(printf '%s\n' '男につきて何か' |
  "$SUI" -c "$candy_config" -u "$LEDGER")
printf '%s\n' "${reused#!}" |
jq -e '
  select(.start == 15 and .end == 21 and .text == "何か")
' >/dev/null ||
  fail "unapproved candy does not advance"

cat > "$candy" <<'EOF'
!{"start":6,"end":21,"text":"竹の中に、","word":"","lemma":"","kana":"","lemma-kana":"","romaji":"","lemma-romaji":"","gloss":"","pos":""}
EOF

reused=$(printf '%s\n' 'その竹の中に、' |
  "$SUI" -c "$candy_config" -u "$LEDGER")
printf '%s\n' "${reused#!}" |
jq -e '
  select(.start == 6 and .end == 21 and .text == "竹の中に、")
' >/dev/null ||
  fail "unapproved incomplete candy does not advance"


cat > "$candy" <<'EOF'
{"start":0,"end":21,"text":"その","word":"その","lemma":"その","kana":"その","lemma-kana":"その","romaji":"sono","lemma-romaji":"sono","gloss":"that","pos":"ADN"}
{"start":6,"end":21,"text":"竹","word":"竹","lemma":"竹","kana":"たけ","lemma-kana":"たけ","romaji":"take","lemma-romaji":"take","gloss":"bamboo","pos":"N"}
{"start":9,"end":21,"text":"の","word":"の","lemma":"の","kana":"の","lemma-kana":"の","romaji":"no","lemma-romaji":"no","gloss":"GEN","pos":"P"}
EOF

mixed=$(printf '%s\n' 'その竹の中に、' |
  "$SUI" -c "$candy_config" "$LEDGER")
printf '%s\n' "$mixed" |
jq -e '
  select(.start == 0 and .end == 21 and
         (.records | length) == 5 and
         [.records[].word] == ["その", "竹", "の", "中", "に"] and
         all(.records[]; has("start") | not) and
         all(.records[]; has("end") | not) and
         all(.records[]; has("text") | not))
' >/dev/null ||
  fail "approved candy bridges ledger runs"

echo "PASS: sui regression tests"
