# Market data lives here and is never committed

Nothing in this directory is in git except this file. NASDAQ's ITCH files are
large (4-18 GB compressed) and are distributed under NASDAQ's terms, so the
repository ships the code, a synthetic generator, and the script that produces
the files — not the files.

## Get a sample day

NASDAQ publishes free historical TotalView-ITCH 5.0 samples in the ITCH folder
of its EMI server:

    https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/

Sizes observed on 2026-10-13 (browse the folder, do not trust this list to stay
accurate):

| file                          | compressed  |
| ----------------------------- | ----------- |
| `07302019.NASDAQ_ITCH50.gz`   |  3.7 GB     |
| `08302019.NASDAQ_ITCH50.gz`   |  4.1 GB     |
| `01302019.NASDAQ_ITCH50.gz`   |  4.8 GB     |
| `S112825-v50.txt.gz`          |  4.7 GB     |
| `S121225-v50.txt.gz`          | 12.6 GB     |
| `S061226-v50.txt.gz`          | 17.9 GB     |

Uncompressed is roughly three to four times the compressed size, so budget
about 15 GB for a 2019 day and 40 GB or more for a 2026 day. A 300 MB slice is
enough for every run in the README.

Check NASDAQ's current terms before redistributing anything derived from these
files. The `.md5sum` files next to each `.gz` are worth checking after download.

## Cut a slice without downloading the whole day

    ./scripts/make_slice.sh \
        'https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/07302019.NASDAQ_ITCH50.gz' \
        data/slice_20190730.itch 300000000

This streams `curl | zcat | make_slice.py`, stops reading once 300 MB of whole
messages have been written, and aborts the download behind it. Expect to
transfer roughly a third of the compressed size.

The slicer cuts on a message boundary, so the result is a valid (if incomplete)
file: the parser reports no truncation error and the message count is stable
across runs. `head -c` does not do this and leaves a half message at the end.

## Synthetic files

No real data needed for tests, demos, or a first benchmark:

    ./tools/gen_synthetic.py -o data/synthetic.itch --messages 1000000

The generator writes specification-ordered messages with the 2-byte length
prefix, then re-reads its own output and checks every length against the spec
table before it exits. Numbers measured on synthetic data are labelled
synthetic: the file is structurally faithful, the message mix is not real.

`data/synthetic_seed.itch` is committed and is the only data file in git: 201
messages, 7,234 bytes, seed 7, containing all ten decoded types plus four
skipped `H` messages. Regenerate it byte-for-byte with:

    ./tools/gen_synthetic.py -o data/synthetic_seed.itch --messages 200 --seed 7
