# Methods of Lattice Evaluation

## Purpose

This memo records the evaluation method used when SUI has more than one possible
lattice route for the same input.

SUI does not assume a morphological dictionary that defines the correct word
boundaries in advance.  A surface or sequence attested in the ledger must not be
rejected merely because another segmentation appears more familiar.  This is
especially important for historical texts: we are not speakers of the language
of the period, and apparent boundary ambiguity may itself be meaningful (for
example, in wordplay).

The purpose of lattice evaluation is therefore not to recover a predefined
morphological analysis.  It is to choose, as conservatively as possible, among
routes supported by attested data.

## Best output

The proposed option is:

```
-b, --best
```

Without `-b`, SUI may retain the detailed candidate output for inspection.
With `-b`, SUI evaluates the lattice and emits the best output.  In ordinary
cases this should result in one JSON line.

## Evaluation order

### 1. Prefer fewer switches

Among competing lattice routes, prefer the route that requires fewer switches
between attested sequences.

The primary criterion is continuity of attested sequence, not a predefined word
dictionary.

### 2. If tied, prefer the longer surface

If routes remain tied in switching cost and differ structurally, prefer the
longer attested surface.

For example:

```
新幹線
新 -> 幹線
```

If the two routes are otherwise tied, prefer `新幹線`.

This is a tie-breaker, not a claim that the longer surface is linguistically the
"correct word".  The ledger supplies evidence of attestation; SUI should not
reject that evidence by imposing a modern word boundary.

### 3. Merge minor word-gloss differences field by field

After route selection, differences among records belonging to the same selected
surface route are not treated as competing lattice routes.

If a field has several attested values, preserve them in a single string,
separated by commas:

```json
"pos": "ADN,ATD,DET"
```

or:

```json
"gloss": "among,inside"
```

This notation preserves the alternatives without forcing SUI to choose among
human interpretations.

The comma is deliberately useful downstream.  `glosslint` does not accept
comma-separated alternatives as a final controlled value, so it can warn and
allow the editor to jump to the unresolved field.  A human can then delete the
unwanted alternatives or leave the source data for later review.

### 4. Merge provenance

When records are merged, retain their provenance as an array:

```json
"provenance": [
  {"corpus":"tosa","id":3,"token":0},
  {"corpus":"tosa","id":141,"token":0}
]
```

Provenance is not required to explain which analysis produced which value.
Its purpose is to provide a route back to the source records.  The source text
is there; gloss and POS are human interpretations and can be reviewed there.

This supports the workflow:

```
SUI -> glosslint -> tag jump -> human review -> source JSON correction
    -> ledger rebuild -> SUI
```

A systematic decision (for example, normalizing an obsolete POS label to
`DET`) may be applied to the source JSON with a jq script rather than editing
each occurrence manually.

## When multiple best outputs remain

Multiple best outputs are necessary only when all preceding criteria fail to
produce a single mergeable result:

1. switching cost is tied;
2. longer-surface tie-breaking does not resolve the structural difference; and
3. the remaining difference cannot be represented safely as field-wise
   comma-separated alternatives.

Thus multiple output lines indicate a genuine unresolved structural ambiguity,
not merely provenance multiplicity or a minor difference in human annotation.

## Principle

The evaluation separates two different problems:

```
lattice route selection
        |
        v
record aggregation
        |
        v
human-readable output
```

Route ambiguity is evaluated by SUI.  Minor differences in human analysis are
preserved rather than guessed away.  Human review resolves them when necessary.

In short:

> Prefer continuity; use longer attested surfaces only as a tie-breaker;
> preserve minor interpretive differences; do not manufacture certainty.
