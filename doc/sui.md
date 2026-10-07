# sui

`sui` analyzes an input string using `ledger.dat`.

It does not generate glosses. It retrieves validated records already present in
the ledger, follows attested sequences, and can use human-approved candy records
when the ledger alone cannot continue.

## Synopsis

```text
sui [-b] [-c config] [-m | -u | -a] ledger.dat
```

Use `-h` to display the current short usage message.

## Basic example

```sh
printf '%s\n' 'その竹の中に、桜の花の咲きにけり' \
  | ./sui -b -c data/ledger-config.json ledger.dat
```

Input is read from standard input. The current implementation processes one
input line per invocation.

## Input

### Plain text

A line that does not begin with `{` is treated as the text to analyze.

```text
その竹の中に、桜の花の咲きにけり
```

### JSON input

A line beginning with `{` is parsed as JSON. It must contain `text` and all
provenance fields required by the configuration.

Conceptually:

```json
{"text":"...","provenance":{...}}
```

The exact provenance fields are defined by the `provenance` array in the
configuration. With `data/ledger-config.json`, these are `corpus`, `id`,
and `token`.

JSON input therefore requires a configuration file.

## Options

### `-b` — best route

Select the best route when more than one route can cover the same input.

The current evaluation prefers fewer transfers between continuous attested or
approved runs. Longer surfaces are used as a tie-breaker where applicable.

This is the normal option for review-oriented output.

### `-c FILE` — configuration

Read the ledger/runtime configuration from `FILE`.

For example:

```sh
-c data/ledger-config.json
```

The configuration defines such things as the sequence field, trie surface field,
ignored text, schema, provenance fields, and candy file.

### `-m` — monitor / diagnostic mode

Display internal information used to inspect routing and the ledger, including
lattice edges, surface combines, paths, occurrence adjacencies, observed paths,
longest paths, reach, and uncovered adjacencies.

This is a diagnostic mode rather than normal word-gloss output.

`-m` cannot be combined with `-u` or `-a`.

### `-u` — unresolved output

Do not emit the normal resolved route. If part of the input remains unresolved,
emit that unresolved material in candy/work-row form to standard output.

This is useful for seeing what still requires human analysis.

### `-a` — append unresolved/candy work

`-a` enables unresolved mode and also appends work records to the candy file
specified by `candy.filename` in the configuration.

It therefore requires both `-c` and a `candy.filename` setting.

Approved candy rows can subsequently be indexed as runtime surfaces and used to
continue a route.

## Candy

Candy is the human-review path for material that the ledger cannot directly
resolve.

A row beginning with `!` is unresolved work. Human approval is represented by
removing the leading `!`.

Two useful cases are:

- a ledger-backed candidate for an unknown input surface (soft candy);
- unresolved text for which the human supplies segmentation and analysis.

Soft candy is not normalization. If the input surface is `桜` and an existing
ledger record with `word:"櫻"` has `lemma:"桜"`, that record can support a
candidate, but SUI does not silently replace `桜` with `櫻`. The actual input
surface remains the runtime surface after approval.

Offsets in candy records are UTF-8 byte offsets.

## Provenance

Normal resolved output can include provenance.

In SUI, provenance is evidence for an attested adjacency, not merely evidence
that an individual word occurred. For a selected edge `A -> B`, provenance on
`B` records where that adjacency was observed.

At a candy boundary there is no ledger-attested adjacency, so provenance beside
that boundary is empty.

This provenance is useful for research and review, but it is editing/research
scaffolding rather than part of the underlying word-gloss analysis.

A provenance-less word-gloss output option is planned but is not part of the
current CLI yet.

## Ignored text

Text listed in `ledger.ignore` in the configuration is treated as a boundary
for sequence continuity.

For example, with `、` ignored, SUI does not require an attested adjacency
across the punctuation. After the boundary it may board an attested sequence
again from the following surface.

The configuration does not claim that the boundary is syntactic or grammatical;
it only controls routing.

## Related documentation

- [mkledger](mkledger.md) — building `ledger.dat`
- [README](../README.md) — quick start and repository layout
