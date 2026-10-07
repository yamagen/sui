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
transfer_dir=$(mktemp -d)
surface_dir=$(mktemp -d)
trap 'rm -f "$candy" "$candy_config"; rm -rf "$transfer_dir" "$surface_dir"' EXIT
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

cat >"$transfer_dir/config.json" <<EOF
{
  "version": "1.0",
  "filename": "config.json",
  "ledger": {"sequence": "id", "trie": "word"},
  "schema": {"id": "integer", "word": "string"},
  "provenance": [],
  "candy": {"filename": "$transfer_dir/candy.jsonl"}
}
EOF

cat >"$transfer_dir/input.jsonl" <<'EOF'
{"id":1,"word":"A"}
{"id":2,"word":"B"}
{"id":2,"word":"C"}
EOF

cat >"$transfer_dir/candy.jsonl" <<'EOF'
{"text":"A","word":"A"}
{"text":"B","word":"B"}
{"text":"A","word":"A"}
{"text":"BC","word":"BC"}
EOF

(
  cd "$transfer_dir"
  "$OLDPWD/mkledger" -c config.json input.jsonl >/dev/null
)

transfer_plain=$(printf '%s\n' 'ABC' |
  "$SUI" -c "$transfer_dir/config.json" "$transfer_dir/ledger.dat")
printf '%s\n' "$transfer_plain" |
jq -e '
  select(.start == 0 and .end == 3 and
         [.records[].word] == ["A", "B", "C"])
' >/dev/null ||
  fail "plain candy candidate order"

transfer_best=$(printf '%s\n' 'ABC' |
  "$SUI" -b -c "$transfer_dir/config.json" "$transfer_dir/ledger.dat")
printf '%s\n' "$transfer_best" |
jq -e '
  select(.start == 0 and .end == 3 and
         [.records[].word] == ["A", "BC"])
' >/dev/null ||
  fail "best prefers fewer transfers"

cat >"$surface_dir/config.json" <<EOF
{
  "version": "1.0",
  "filename": "config.json",
  "ledger": {"sequence": "id", "trie": "word"},
  "schema": {"id": "integer", "word": "string"},
  "provenance": [],
  "candy": {"filename": "$surface_dir/candy.jsonl"}
}
EOF

cat >"$surface_dir/input.jsonl" <<'EOF'
{"id":1,"word":"A"}
EOF

cat >"$surface_dir/candy.jsonl" <<'EOF'
{"text":"A","word":"A"}
{"text":"B","word":"B"}
{"text":"A","word":"A"}
{"text":"BC","word":"BC"}
EOF

(
  cd "$surface_dir"
  "$OLDPWD/mkledger" -c config.json input.jsonl >/dev/null
)

surface_plain=$(printf '%s\n' 'ABC' |
  "$SUI" -c "$surface_dir/config.json" "$surface_dir/ledger.dat")
printf '%s\n' "$surface_plain" |
jq -e '
  select(.start == 0 and .end == 2 and
         [.records[].word] == ["A", "B"])
' >/dev/null ||
  fail "plain longer-surface candidate order"

surface_best=$(printf '%s\n' 'ABC' |
  "$SUI" -b -c "$surface_dir/config.json" "$surface_dir/ledger.dat")
printf '%s\n' "$surface_best" |
jq -e '
  select(.start == 0 and .end == 3 and
         [.records[].word] == ["A", "BC"])
' >/dev/null ||
  fail "best prefers longer surface after transfer tie"

echo "PASS: sui regression tests"
