# SUI

SUI builds and uses a ledger of validated word-gloss records.

SUI does not generate glosses. It retrieves existing analyses from the ledger,
follows attested sequences, and leaves unresolved material for human review.

## Quick start

Build:

```sh
make
```

Build `ledger.dat` from the prepared JSONL data:

```sh
./mkledger -c data/ledger-config.json data/ledger.jsonl
```

Analyze a string:

```sh
printf '%s\n' 'その竹の中に、桜の花の咲きにけり' \
  | ./sui -b -c data/ledger-config.json ledger.dat
```

Run the regression tests:

```sh
make test
```

## Documentation

- [mkledger](doc/mkledger.md) — build `ledger.dat`
- [sui](doc/sui.md) — analyze text with a ledger

## Repository directories

- `data/` — corpus data, prepared ledger input, and runtime configuration
- `tests/` — regression tests and test-only data
- `doc/` — current user documentation
- `memo/` — design notes and development records
- `src/` — source code

The files under `memo/` record how the design developed. They are not the
current command-line specification; use `doc/` for that.
