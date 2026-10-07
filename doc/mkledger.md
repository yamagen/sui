# mkledger

`mkledger` compiles validated JSONL records into `ledger.dat`, which is then
used by `sui`.

## Synopsis

```text
mkledger [options] [ledger.in]
```

If no input file is supplied, `mkledger` reads JSONL from standard input.
If one file is supplied, it reads that file.

The output file is currently named `ledger.dat`.

## Basic example

The repository contains prepared ledger input in `data/ledger.jsonl`:

```sh
./mkledger -c data/ledger-config.json data/ledger.jsonl
```

This creates:

```text
ledger.dat
```

The larger source corpus is kept separately as `data/tosanikki.json`.
`data/ledger.jsonl` is the prepared line-oriented input consumed by
`mkledger`.

## Input format

`mkledger` reads one JSON object per line.

Fields are validated against the `schema` section of the configuration. The
field named by `ledger.sequence` identifies the source sequence, and the field
named by `ledger.trie` supplies the surface stored in the sequence trie.

With `data/ledger-config.json`:

```json
"ledger": {
  "sequence": "id",
  "trie": "word",
  "ignore": ["、", "。"]
}
```

Thus `id` identifies sequences and `word` is the trie surface.

The same configuration declares provenance fields:

```json
"provenance": ["corpus", "id", "token"]
```

and the remaining word-gloss fields in `schema`.

## Options

The current `mkledger` CLI provides:

- `-c FILE` — use `FILE` as the configuration file
- `-f` — output pair frequencies
- `-m` — monitor/display the loaded configuration
- `-p` — show one combine with multiple provenance records
- `-s` — show statistics
- `-h` — show help
- `-v` — show version

Without `-c`, the current default configuration path is
`ledger-config.json`. In this repository the maintained runtime configuration
is under `data/`, so examples specify it explicitly.

## Configuration

The repository configuration is `data/ledger-config.json`.

Its main sections are:

- `ledger.sequence` — field used to identify sequences
- `ledger.trie` — field whose values become trie surface tokens
- `ledger.ignore` — text that breaks sequence continuity for SUI routing
- `schema` — accepted fields and their types
- `provenance` — fields identifying corpus evidence
- `candy.filename` — candy work file used by SUI

A surface is a string of arbitrary length. The ledger does not require surfaces
to be single characters.

## Data flow

```text
validated corpus data
        |
        v
 prepared JSONL
 (data/ledger.jsonl)
        |
        v
    mkledger
        |
        v
    ledger.dat
        |
        v
       sui
```

`mkledger` builds the ledger; `sui` uses it. Gloss generation is outside
both programs.

## Repository directories

`data/` contains runtime/corpus data. Test fixtures belong under `tests/`.
Design history belongs under `memo/`; current command documentation belongs
under `doc/`.

## Related documentation

- [sui](sui.md) — using `ledger.dat`
- [README](../README.md) — quick start and repository layout
