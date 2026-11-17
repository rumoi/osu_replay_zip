# ORZ format

.orz is a lossless compression format to store osu replay frames. [SPEC_OUTLINE.md](SPEC_OUTLINE.md) outlines the format.

**The format is specified, but V0 is not pinned quite yet.**\
**The priors are still being trained, once they are finished the format is ready.**

Replay set: https://drive.google.com/file/d/1i2oe929loEzhvCfRYG-DeZZQ_YouaBYD/view

Sizes are totals over the whole set, in bytes:

- **raw**: the frames as the raw required binary, 16 bytes each (int delta, float x, float y, u32 keys)
- **.osr**: the replay data inside the .osr (the frames as text, LZMA compressed)
- **.orz**: the replay data as .orz

# osu!
4,163 replays, 45,036,306 frames

| | bytes | of raw | of .osr |
|---|---:|---:|---:|
| raw | 720,580,896 | 100% | |
| .osr | 226,602,499 | 31.45% | 100% |
| .orz | 61,921,830 | **8.59%** | **27.33%** |

128 bits a frame raw, 40.3 as .osr, 11.0 as .orz.

![box_whisk](./graph.png)

# taiko
457 replays, 4,064,380 frames

| | bytes | of raw | of .osr |
|---|---:|---:|---:|
| raw | 65,030,080 | 100% | |
| .osr | 2,794,463 | 4.30% | 100% |
| .orz | 1,237,109 | **1.90%** | **44.27%** |

# mania
1,562 replays, 16,344,692 frames

| | bytes | of raw | of .osr |
|---|---:|---:|---:|
| raw | 261,515,072 | 100% | |
| .osr | 14,573,760 | 5.57% | 100% |
| .orz | 6,643,539 | **2.54%** | **45.59%** |

# fruit
373 replays, 3,292,540 frames

| | bytes | of raw | of .osr |
|---|---:|---:|---:|
| raw | 52,680,640 | 100% | |
| .osr | 10,419,674 | 19.78% | 100% |
| .orz | 4,650,899 | **8.83%** | **44.64%** |

# Total
6,555 replays, 68,737,918 frames

| | bytes | of raw | of .osr |
|---|---:|---:|---:|
| raw | 1,099,806,688 | 100% | |
| .osr | 254,390,396 | 23.13% | 100% |
| .orz | 74,453,377 | **6.77%** | **29.27%** |
