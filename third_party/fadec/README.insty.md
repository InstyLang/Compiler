# Vendored: Fadec encoder (faenc / encode2 API)

Source: https://github.com/aengelke/fadec
Upstream commit: `340a7a86117895b7b71e56deac99d96340eab587`
License: BSD-3-Clause (see `LICENSE`).

Only the x86-64 **encoder** (the `fe64_*` "encode2" API) is vendored. The
decoder, formatter, and the older `fadec-enc.h` API are not used by Insty.

## Files

| File | Origin |
|------|--------|
| `fadec-enc2.h` | upstream, verbatim |
| `encode2.c` | upstream, verbatim |
| `fadec-encode2-public.inc` | generated (see below) |
| `fadec-encode2-private.inc` | generated (see below) |
| `LICENSE` | upstream, verbatim |

The two `.inc` files are produced by Fadec's `parseinstrs.py` table generator
from `instrs.txt`. They are committed here so the Insty build needs **no Python
at build time**.

## Regenerating the tables

Only needed when bumping the vendored Fadec version. Requires Python 3.

```sh
git clone https://github.com/aengelke/fadec
cd fadec
python parseinstrs.py encode2 instrs.txt \
    fadec-encode2-public.inc fadec-encode2-private.inc \
    --32 --64 --with-undoc
```

Then copy `fadec-enc2.h`, `encode2.c`, the two generated `.inc` files, and
`LICENSE` into this directory and update the commit hash above.
