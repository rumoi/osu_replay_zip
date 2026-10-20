# ORZ format

.orz is a lossless compression format to store osu replay frames. [SPEC_OUTLINE.md](SPEC_OUTLINE.md) outlines the format.

The format is still maturing, please do NOT use this format just yet!
There can and will be breaking changes as the initial V0 is tuned on a dataset thats over a terabyte graciously provided by cmyui (akatsuki private server)

Replay set: https://drive.google.com/file/d/1i2oe929loEzhvCfRYG-DeZZQ_YouaBYD/view

Sizes are totals over the whole set: the .osr replay data (LZMA) in, the .orz out, in bytes.

# osu!
27.496% of the original size (4,163 replays)

226,602,499 -> 62,307,113

# taiko
45.346% of the original size (457 replays)

2,794,463 -> 1,267,165

# mania
46.615% of the original size (1,562 replays)

14,573,760 -> 6,793,586

# fruit
44.847% of the original size (373 replays)

10,419,674 -> 4,672,936

# Total
29.498% of the original size (6,555 replays)

254,390,396 -> 75,040,800