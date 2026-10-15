#!/bin/sh

jq -c '
  .. | objects
  | select(has("id") and has("word-gloss"))
  | . as $p
  | $p["word-gloss"]
  | to_entries[]
  | {
      corpus: "tosa",
      id: $p.id,
      token: .key
    } + .value
' tosanikki.json > ledger.jsonl
