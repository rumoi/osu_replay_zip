# ORZ format

.orz is a lossless compression format to store osu replay frames. [SPEC_OUTLINE.md](SPEC_OUTLINE.md) outlines the format.

**The format is specified, but V0 is not pinned quite yet.**\
**The priors are still being trained, once they are finished the format is ready.**

Replay set: https://drive.google.com/file/d/1i2oe929loEzhvCfRYG-DeZZQ_YouaBYD/view

Sizes are totals over the whole set, in bytes:

- **raw**: the frames as the raw required binary, 16 bytes each (int delta, float x, float y, u32 keys)
- **.osr**: the replay data inside the .osr (the frames as text, LZMA compressed)
- **.orz**: the replay data as .orz, at each compression priority (SIZE, the default; BALANCED; SPEED)
- **encode / decode**: the time from the frame values and back to them, summed over the replays (one process pinned to
  core 0). Each replay runs all four formats interleaved, in a rotating order, the best of 3 each. .osr: the replay text
  written and LZMA compressed with osu!'s settings, then decompressed and parsed; .orz: the encoder from the frames, the
  decoder to the frames (no text either way)

# osu!
4,163 replays, 45,036,306 frames

| | bytes | of raw | of .osr | encode | decode |
|---|---:|---:|---:|---:|---:|
| raw | 720,580,896 | 100% | | | |
| .osr | 226,602,499 | 31.45% | 100% | 258.42 s (62.1 ms) | 14.83 s (3.56 ms) |
| .orz-SIZE | 61,921,830 | **8.59%** | **27.33%** | 115.70 s (27.8 ms) | 50.27 s (12.07 ms) |
| .orz-BALANCED | 67,461,668 | 9.36% | 29.77% | 96.39 s (23.2 ms) | 29.81 s (7.16 ms) |
| .orz-SPEED | 75,729,997 | 10.51% | 33.42% | 84.97 s (20.4 ms) | 17.71 s (4.25 ms) |

128 bits a frame raw, 40.3 as .osr, 11.0 as .orz-SIZE, 12.0 as .orz-BALANCED, 13.5 as .orz-SPEED.

<p>
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="charts/osu_size_dark.svg">
  <img alt="osu! size, % of .osr" src="charts/osu_size_light.svg" width="600">
</picture><br>
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="charts/osu_encode_dark.svg">
  <img alt="osu! encode, ms per replay" src="charts/osu_encode_light.svg" width="600">
</picture><br>
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="charts/osu_decode_dark.svg">
  <img alt="osu! decode, ms per replay" src="charts/osu_decode_light.svg" width="600">
</picture>
</p>

# taiko
457 replays, 4,064,380 frames

| | bytes | of raw | of .osr | encode | decode |
|---|---:|---:|---:|---:|---:|
| raw | 65,030,080 | 100% | | | |
| .osr | 2,794,463 | 4.30% | 100% | 25.08 s (54.9 ms) | 0.39 s (0.85 ms) |
| .orz-SIZE | 1,237,109 | **1.90%** | **44.27%** | 1.40 s (3.1 ms) | 1.32 s (2.90 ms) |
| .orz-BALANCED | 1,617,479 | 2.49% | 57.88% | 0.73 s (1.6 ms) | 0.64 s (1.40 ms) |
| .orz-SPEED | 2,086,189 | 3.21% | 74.65% | 0.40 s (0.9 ms) | 0.26 s (0.56 ms) |

<p>
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="charts/taiko_size_dark.svg">
  <img alt="taiko size, % of .osr" src="charts/taiko_size_light.svg" width="600">
</picture><br>
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="charts/taiko_encode_dark.svg">
  <img alt="taiko encode, ms per replay" src="charts/taiko_encode_light.svg" width="600">
</picture><br>
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="charts/taiko_decode_dark.svg">
  <img alt="taiko decode, ms per replay" src="charts/taiko_decode_light.svg" width="600">
</picture>
</p>

# mania
1,562 replays, 16,344,692 frames

| | bytes | of raw | of .osr | encode | decode |
|---|---:|---:|---:|---:|---:|
| raw | 261,515,072 | 100% | | | |
| .osr | 14,573,760 | 5.57% | 100% | 100.58 s (64.4 ms) | 2.31 s (1.48 ms) |
| .orz-SIZE | 6,643,539 | **2.54%** | **45.59%** | 5.33 s (3.4 ms) | 4.91 s (3.14 ms) |
| .orz-BALANCED | 8,185,006 | 3.13% | 56.16% | 2.99 s (1.9 ms) | 2.57 s (1.65 ms) |
| .orz-SPEED | 9,588,713 | 3.67% | 65.79% | 1.74 s (1.1 ms) | 1.10 s (0.70 ms) |

<p>
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="charts/mania_size_dark.svg">
  <img alt="mania size, % of .osr" src="charts/mania_size_light.svg" width="600">
</picture><br>
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="charts/mania_encode_dark.svg">
  <img alt="mania encode, ms per replay" src="charts/mania_encode_light.svg" width="600">
</picture><br>
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="charts/mania_decode_dark.svg">
  <img alt="mania decode, ms per replay" src="charts/mania_decode_light.svg" width="600">
</picture>
</p>

# fruit
373 replays, 3,292,540 frames

| | bytes | of raw | of .osr | encode | decode |
|---|---:|---:|---:|---:|---:|
| raw | 52,680,640 | 100% | | | |
| .osr | 10,419,674 | 19.78% | 100% | 16.95 s (45.4 ms) | 0.87 s (2.32 ms) |
| .orz-SIZE | 4,650,899 | **8.83%** | **44.64%** | 2.44 s (6.5 ms) | 2.15 s (5.75 ms) |
| .orz-BALANCED | 4,951,605 | 9.40% | 47.52% | 1.61 s (4.3 ms) | 1.25 s (3.35 ms) |
| .orz-SPEED | 5,503,868 | 10.45% | 52.82% | 1.09 s (2.9 ms) | 0.70 s (1.87 ms) |

<p>
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="charts/fruit_size_dark.svg">
  <img alt="fruit size, % of .osr" src="charts/fruit_size_light.svg" width="600">
</picture><br>
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="charts/fruit_encode_dark.svg">
  <img alt="fruit encode, ms per replay" src="charts/fruit_encode_light.svg" width="600">
</picture><br>
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="charts/fruit_decode_dark.svg">
  <img alt="fruit decode, ms per replay" src="charts/fruit_decode_light.svg" width="600">
</picture>
</p>

# Total
6,555 replays, 68,737,918 frames

| | bytes | of raw | of .osr | encode | decode |
|---|---:|---:|---:|---:|---:|
| raw | 1,099,806,688 | 100% | | | |
| .osr | 254,390,396 | 23.13% | 100% | 401.02 s (61.2 ms) | 18.39 s (2.81 ms) |
| .orz-SIZE | 74,453,377 | **6.77%** | **29.27%** | 124.87 s (19.1 ms) | 58.64 s (8.95 ms) |
| .orz-BALANCED | 82,215,758 | 7.48% | 32.32% | 101.72 s (15.5 ms) | 34.27 s (5.23 ms) |
| .orz-SPEED | 92,908,767 | 8.45% | 36.52% | 88.20 s (13.5 ms) | 19.77 s (3.02 ms) |

<p>
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="charts/total_size_dark.svg">
  <img alt="Total size, % of .osr" src="charts/total_size_light.svg" width="600">
</picture><br>
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="charts/total_encode_dark.svg">
  <img alt="Total encode, ms per replay" src="charts/total_encode_light.svg" width="600">
</picture><br>
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="charts/total_decode_dark.svg">
  <img alt="Total decode, ms per replay" src="charts/total_decode_light.svg" width="600">
</picture>
</p>
