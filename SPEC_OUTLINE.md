# ORZ format outline

An overview of the `.orz` file format: what a file holds, how it is laid out, and what a decoder must do to get the
replay back. It describes the format rather than this repository's code. Where the format depends on data or
arithmetic that is too large to restate (model tables, symbol orders, context definitions, predictors), the outline
names it and the file that defines it. That code is then normative.

This describes the current, unreleased version 0. Its rules and built-in tables are not frozen: the version number
alone does not identify a compatible experimental build. A decoder needs the same rules and tables as its encoder.

## 1. Scope and guarantees

An `.orz` file holds one osu! replay, given either as a full `.osr` file or as bare replay data (the LZMA stream of
replay text, as servers store it).

Decoding gives back:

- **the replay text exactly** (the LZMA-decompressed `delta|x|y|keys,` list);
- **the `.osr` bytes around the replay data exactly**: the header, the life bar text, the timestamp and the bytes
  after the replay data (exceptions in §13);
- **the replay data as an LZMA stream of that text** (§11): compressed again, so not the original LZMA bytes. Files
  that store the replay data verbatim (§10) come back with those bytes exactly.

Frames are reproduced to the text they print as, not always bit for bit. With 7-digit text (§4), a float only has to
print the same 7 significant digits.

A file may also hold custom containers (§3.2): named blobs outside the format, such as a tool's own metadata.
Decoding the replay ignores them.

The decoding order is:

1. Read the header and locate its containers (§3).
2. Decode the frames for the stored mode and compression priority (§5–§7), or parse the verbatim replay data (§10).
3. For frame-coded data, append the closing seed frame when present (§3.5); then check the CRC (§3.4).
4. Decode the metadata and life bar (§8–§9), using the frame times for a compact life bar.
5. Rebuild the original kind of output (§11), copying verbatim data or printing and recompressing frames (§4).

## 2. Conventions

- **Numbers.** `uN` and `iN` are unsigned and two's-complement signed N-bit integers; `f32` is IEEE 754 binary32.
  Byte-aligned integers and float bit patterns are little-endian unless stated otherwise. `bit_width(0) = 0`;
  for a positive integer it is `floor(log2 v) + 1`. Frame and position indexes start at 0.
- **Bit packing.** Bit fields are written least significant bit first, and bits fill each byte starting from its low
  bit, except where an explicit bit order is given. Padding to a byte boundary adds 0 to 7 zero bits. Reading past
  the end of a bit field gives 0; this does not make a truncated file valid.
- **Lists.** A list with neither a fixed length nor an upper bound must end with a sentinel, and reading 0 must end
  it. Reading past the end gives 0, so damaged or truncated data always ends its lists.
- **`bucket(b1, b2, …)`** is a variable-length unsigned code. For each width `b` in turn:
  - if the value is below `2^b − 1`, write it in `b` bits and stop;
  - otherwise write `b` one bits, subtract `2^b − 1` and continue.

  If every width escapes, let `v = value + 1` and `e = floor(log2 v)`; write `e` in 5 bits, then the `e` bits of
  `v` below its leading 1.
- **`small<N>(v)`**: `N` bits if `v < 2^N − 1`. Otherwise `N` one bits, then `floor(log2 v) − (N − 1)` further
  one bits, a zero, and the `floor(log2 v)` bits of `v` below its leading 1. `v = 2^N − 1` is the exception: it is
  the `N` one bits and the zero alone, as no other value has them.
- **`length_bucket<n,from>(v)`** gives the value's bit length, then the bits below its leading 1. With
  `bitlen = bit_width(v)` and `escape = 2^n − 1`:
  - an `n`-bit code: `bitlen − from` for bit lengths `from` to `from + escape − 1`, otherwise `escape`;
  - after the code `escape`, 5 bits of `bitlen < from ? bitlen : bitlen − escape`;
  - then the `bitlen − 1` bits of `v` below its leading 1. Zero has bit length 0 and no bits after its code.

  So `length_bucket<3,18>` has a 3-bit code that is `bitlen − 18` for bit lengths 18 to 24 and 7 otherwise, and
  `length_bucket<4,0>` has a 4-bit code that is `bitlen` itself up to 14 and 15 otherwise.
- **zigzag** maps signed to unsigned:

  ```math
  \mathrm{zz}(v) = \begin{cases} 2v & v \ge 0 \\ -2v - 1 & v < 0 \end{cases}
  ```
- **Floating point.** Decoding needs plain IEEE 754 arithmetic, so that every machine gives the same bits (§15).
- **ULEB128**: the usual 7-bits-per-byte length used by `.osr` strings.
- **Coded values** inside an arithmetic-coded body (§5) are binary decisions.
  - **Raw** bits are decisions at probability 1/2, most significant bit first.
  - **`gamma(v)`** is Elias gamma in raw decisions: `floor(log2 v)` ones, a zero, then the bits of `v` below its
    leading 1.

The raw arithmetic-coded bits above are different from literal bits in a bit field.

## 3. Container

```
byte 0     magic 0xE6
byte 1..   header, a bit field padded to a byte boundary
then       the containers' data, back to back, in the order the header lists them
```

Bytes after the last container are ignored.

### 3.1 Header (in order)

| Field | Coding | Notes |
|---|---|---|
| version | `bucket(4, 8)` | 0. A reader rejects any other version before reading on. |
| crc | 32 bits | §3.4 |
| encode info | `length_bucket<4,0>` | §3.3 |
| seeded | 1 bit | the replay ended with a closing seed frame (§3.5) |
| seed | `length_bucket<3,18>`, if seeded | §3.5 |
| frame count | `bucket(16)` | frames held by the frames stream, without the seed frame; at most 2^22 |
| standard containers | §3.2 | |
| custom containers | §3.2 | |

A reader rejects a header that ends past the end of the file.

### 3.2 Containers

The header lists the containers, each with its size. Their data follows the header in the same order, each exactly
its size: the standard containers first, then the custom ones. A reader rejects a size above 2^28 bytes, and data
that runs past the end of the file.

**Standard containers.** Each one is announced by a 1 bit, and a 0 bit ends the list. Then:

1. **tag**: the previous container's tag plus 1 (for the first container, 0 plus 1). If a 1 bit follows, a
   `bucket(4)` is added on top.
2. **size**: `bucket(16)`.

Tags therefore rise from one container to the next. A reader rejects a tag above 255 and ignores tags it does not
know.

| Tag | Container | Present when |
|---|---|---|
| 1 | `frames`: the frames stream (§6, §7) | `replay_verbatim` is 0 |
| 2 | `osr_meta_data`: the `.osr` bytes around the replay data, and the life bar (§8, §9) | `has_osr_header` |
| 3 | `osr_verbatim`: the original LZMA replay data (§10) | `replay_verbatim` is 1 |

The reference encoder omits empty standard containers, so a replay with no replay data has neither `frames` nor
`osr_verbatim`.

**Custom containers.**

- 1 bit: whether there are any. If 1, `bucket(4)` gives their count minus 1.
- Per custom container:
  1. **name**: its length in 5 bits (at most 31 characters), then each character in 7 bits (ASCII);
  2. **size**: `bucket(16)`.

### 3.3 Encode info

The encode info is one unsigned number, read from the header as `length_bucket<4,0>` (§2): 4 bits give its bit
length, then come the bits below its leading 1. Its fields are bit ranges of that number, from its lowest bit up;
they are not read from the file one by one.

| Field | Size | Meaning |
|---|---|---|
| mode | 4 bits | what the replay is, and so the form of its frames stream (below) |
| `has_osr_header` | 1 bit | the input was a full `.osr`, so the decoder rebuilds one |
| `trailing_comma` | 1 bit | the replay text ended with `,` |
| `compression_priority` | 2 bits | which probability model codes the frames stream (§5.4): 0 = SIZE, 1 = BALANCED, 2 = SPEED |
| `replay_verbatim` | 1 bit | the replay data is stored as-is (§10) |
| `text_shortest` | 1 bit | floats print as shortest round-trip text, as in osu!lazer; otherwise as 7 significant digits, as in osu!stable (§4) |

A reader rejects priority 3, any bit that is 1 above these 10, a mode of 8 or above, and a verbatim file whose priority is
not 0. An encoder writes a verbatim file with `trailing_comma` and `text_shortest` 0 too.

| Mode | Replay | Frames stream |
|---|---|---|
| 0 | osu! | osu!standard form (§6) |
| 1 | osu!taiko | osu!taiko form (§7) |
| 2 | osu!catch | osu!catch form (§7) |
| 3 | osu!mania | osu!mania form (§7) |
| 4 | osu! relax | osu!standard form, relax |
| 5 | osu!taiko relax | osu!taiko form |
| 6 | osu!catch relax | osu!standard form, relax (the cursor drives the catcher) |
| 7 | osu! autopilot | osu!standard form with the autopilot path model (§6.6) |

- For an `.osr`, the mode is its game mode (0 to 3), plus 4 when its mods have relax (bit 7). osu!mania has no relax.
  For bare replay data, the mode is given with it.
- Modes 4 and 6 select the relax prior sets (§5.6) and time contexts (§5.5); mode 5 is coded exactly as mode 1.
- Mode 7 requires the autopilot head fields (§6.1) and path model (§6.6). The reference encoder selects it by
  fitting the frames of a mode 0 replay; passing mode 7 directly has a known limitation (§13).

### 3.4 CRC

CRC-32C (Castagnoli polynomial), starting from `0xFFFFFFFF` with no final XOR, over:

1. **Every frame** as 16 little-endian bytes: `i32 delta`, `f32 x`, `f32 y`, `u32 keys`. These are the frames as the
   decoder rebuilds them (§1), with the closing seed frame `{−12345, +0, +0, seed}` when there is one. For a verbatim
   file they are the frames its replay text parses to (the source-defined parser in §14), or none if the data does
   not decompress.
2. **`osr_meta_data` and `osr_verbatim`, in that order**, each when present and not empty: its `u32` size, then its
   bytes.
3. **The encode info**, as stored (§3.3), as a `u32`.

The CRC covers the decoded frames rather than the `frames` container's bytes. Besides encode info, header fields
are not hashed directly. Custom and unknown containers, padding and trailing file bytes are not included. For
verbatim data, the header's seed and frame count do not contribute frames to the CRC.

### 3.5 Seed frame

A replay text that ends with the frame `-12345|0|0|<seed>` stores `<seed>` in the header instead of as a frame. The
decoder appends `{−12345, +0, +0, seed}` after the decoded frames.

## 4. Replay text

The replay text is a list of frames `delta|x|y|keys` joined by `,`, with an optional trailing `,`
(`trailing_comma`). Each frame is an `i32` delta, `float` x and y, and `u32` keys. A file stores frames and rebuilds
the text by printing them, so only text that printing reproduces exactly can be stored as frames (§10).

`delta` is elapsed replay time in milliseconds since the previous frame. x, y and keys have the game mode's
meaning: cursor coordinates and a key bitmask for osu!, but held columns in x and scroll speed in y for osu!mania.

`delta` and `keys` print as plain decimal integers. x and y print in one of two float styles, which applies to the
whole replay:

- **7 significant digits** (`text_shortest` = 0), as .NET Framework's default formatting:
  - the digits are the float's exact binary value rounded half away from zero, with trailing zeros removed;
  - both 0 and −0 print as `0`.
- **Shortest round-trip** (`text_shortest` = 1): the shortest digits that parse back to the same float; −0 prints as
  `-0`.

Both styles share one layout, where `e` is the decimal exponent of the leading digit and a negative value starts
with `-`:

- `e ≤ −5` or `e ≥ 7`: the first digit, a decimal point and the other digits when there are any, then `E`, the
  exponent's sign and at least two exponent digits (`1.5E+07`, `1E-05`);
- `e < 0`: `0.`, then `−e − 1` zeros, then the digits;
- otherwise: the digits with a decimal point after `e + 1` of them, zero-padded, and no point when there are no
  fractional digits.

`NaN` and `±Infinity` print as `NaN`, `Infinity` and `-Infinity`.

### 4.1 Text ordinals

A text ordinal is an integer place for each float in the order of the values the replay's float style can print, so
neighbouring printed values have neighbouring ordinals:

- with 7-digit text, a nonzero value with 7 significant digits `m` (1 000 000 to 9 999 999) and exponent `e` is at
  the place below, signed as the value, and 0 is at `K = 0`:

  ```math
  K = \pm\big((e + 50) \cdot 9\,000\,000 + (m - 10^6) + 1\big)
  ```
- with shortest text, `K` is the bit pattern of the float's magnitude, negated for negative values.

Negative zero and non-finite values have no text ordinal.

## 5. The binary coder

Every frames stream is a short bit-field head, then one arithmetic-coded body. The body is a sequence of binary
decisions, each predicted from counters. Encoder and decoder must compute identical predictions, so all model
arithmetic is integer-only and normative (`cm.h`).

In the formulas below, $`\lfloor x / 2^k \rfloor`$ is an arithmetic right shift by `k`: it rounds toward −∞ for
negative values too.

### 5.1 Arithmetic coder

The state is two `u32` values, `x1 = 0` and `x2 = 0xFFFFFFFF`. To code a bit `y` with probability `p` (`P(y=1)`
scaled to 16 bits):

- split the range, in 64-bit arithmetic:

  ```math
  x_\text{mid} = x_1 + \left\lfloor \frac{(x_2 - x_1)\, p}{2^{16}} \right\rfloor
  ```
- `y = 1` keeps the low part (`x2 = xmid`), `y = 0` the high part (`x1 = xmid + 1`);
- while the top bytes of `x1` and `x2` are equal, output that byte and shift: `x1 <<= 8`, `x2 = (x2 << 8) | 255`.

The encoder ends by writing the 4 bytes of `x1`, most significant first. The decoder reads 4 bytes into `x` and
decodes `y = (x ≤ xmid)`. It shifts input in the same way. A valid stream supplies every byte needed. The reference
decoder reads 0 past the end and periodically checks whether it has read more than 16 bytes past it.

A decision with a fixed probability (a raw bit) is coded at `p = 32768`.

### 5.2 Counters

A counter is 16 bits: an 11-bit probability `q` in the high bits and a 5-bit count `n` in the low bits. It starts at
`q = 1024`, `n = 0`. Its probability on the coder's 16-bit scale is `p = 32q`. After a bit `y`:

- move `q` toward the bit, by a rate that falls as `n` grows:

  ```math
  q \leftarrow q + \left\lfloor \frac{(2047\,y - q)\cdot \mathrm{RATE}[n]}{2^{16}} \right\rfloor,
  \qquad \mathrm{RATE}[n] = \left\lfloor \frac{131072}{2n + 3} \right\rfloor
  ```
- then `n` increases by 1 if it is below 31.

### 5.3 Models

Each kind of decision has a model:

- **a table of counters in lines**: `2^bits` lines of `width` counters each, where a line is found by
  `hash & (2^bits − 1)`, with no collision check;
- **sets of mixer weights**, one weight per input: each context's starts at 19661 (0.3 in 16.16 fixed point) and
  the fallback's, the last, at 0.

Binding a decision's `N` contexts (`N` ≤ 7) gives `N + 1` inputs, each with a line. Input `i`'s line is
`hash(salt · 8 + i, context_i)` for `i < N`. The last input is the **fallback**: its line is `hash(salt · 8 + 7, 0)`
whatever the contexts are. A decision names a counter slot and a weight set, and uses the counter at that slot in
each bound line. `hash(a, b)` works on `u64`:

```
h = (a + 1) · 0x9E3779B97F4A7C15 ^ b · 0xD6E8FEB86659FD93
h ^= h >> 29;  h *= 0xBF58476D1CE4E5B9;  h ^= h >> 32
```

The hash operations wrap modulo 2^64.

### 5.4 The coded probability

The header's `compression_priority` picks one of three rules for every modelled decision of the frames stream.

**SIZE** mixes the inputs. `squash` maps a stretched value `d` to a 12-bit probability by interpolating a 33-entry
table `T`. With $`i = \lfloor d / 128 \rfloor + 16`$ and $`r = d - 128 \lfloor d / 128 \rfloor`$:

```math
\mathrm{squash}(d) =
\begin{cases}
1 & d < -2047 \\
4095 & d > 2047 \\
\left\lfloor \dfrac{T[i]\,(128 - r) + T[i+1]\,r + 64}{128} \right\rfloor & \text{otherwise}
\end{cases}
```

```
T = {1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101, 1546, 2047, 2549, 2994, 3348, 3607, 3785,
     3901, 3975, 4022, 4050, 4068, 4079, 4085, 4089, 4092, 4093, 4094}
```

`stretch(p)` is the smallest `d` in −2047…2047 with `squash(d) ≥ p` (2047 if none).

For one decision with counter probabilities $`p_i`$ (one per input) and weight set $`w`$:

1. **Mix** the stretched counters (the sum in 64-bit):

   ```math
   x_i = \mathrm{stretch}\!\left(\left\lfloor \frac{p_i}{16} \right\rfloor\right), \qquad
   p_m = \mathrm{squash}\!\left(\mathrm{clamp}\!\left(\left\lfloor \frac{\sum_i x_i w_i}{2^{16}} \right\rfloor,\ -2047,\ 2047\right)\right)
   ```
2. **Code** the bit with the mixer's probability scaled to 16 bits:

   ```math
   p = \mathrm{clamp}(16\,p_m,\ 32,\ 65504)
   ```

After coding `y`, for each input in order, update its counter (§5.2), then its weight:

```math
w_i \leftarrow w_i + \left\lfloor \frac{40\, x_i\, (4096\,y - p_m) + 2^{15}}{2^{16}} \right\rfloor
```

Every $`x_i`$ is read before any counter is updated, and two inputs that land on the same counter update it twice.

**BALANCED** binds only each model's first context and its fallback, with the same hashes, salts and table sizes.
The coded probability is the context counter's `p` when its `n ≥ 8`, otherwise the fallback's, clamped to
`[32, 65504]`. After the bit, the context counter is updated, then the fallback. There is no mixer.
If the two inputs select the same counter, it is updated twice.

**SPEED** uses only the fallback counter: `p` clamped to `[32, 65504]`, and one update. A model's table is two lines
of `width` counters, indexed directly by the salt (0 or 1), so its size does not depend on the frame count. Only the
long move-residual lengths (§6.2) use salt 1.

### 5.5 Table sizes

Except with SPEED, a model's table size depends on the header's frame count, up to 16,384 frames
(`b = bit_width(min(frame count, 16384))`), so a decoder must size every table exactly as the encoder did.

| Model | Contexts | Table bits | Width | Weight sets |
|---|---|---|---|---|
| keys changed | 5 | clamp(b+4, 16, 22) | 1 | 64 |
| key bits | 4 | clamp(b+2, 14, 20) | 1 | 64 |
| high key bits | 1 | 8 | 1 | 1 |
| time | 4; 6 for modes 1, 4, 5 and 6 | clamp(b, 12, 14) | 64 | 576 |
| repeat | 4 | clamp(b+4, 16, 22) | 1 | 64 |
| move | 6 | clamp(b+3, 12, 16) | 32 | 384 |
| move bits | 2 | clamp(b+2, 12, 20) | 1 | 32 |
| slack | 1 | clamp(b−2, 10, 16) | 16 | 64 |
| escape | 5 | clamp(b+2, 12, 20) | 1 | 48 |
| escape value | 2 | 10 | 64 | 1 |
| autopilot event | 5 | clamp(b, 10, 16) | 1 | 8 |
| autopilot value | 2 | 12 | 64 | 1 |
| catcher moved | 6 | clamp(b+3, 12, 20) | 1 | 64 |
| catcher direction | 4 | clamp(b+1, 10, 18) | 1 | 16 |
| catcher residual | 5 | clamp(b, 10, 14) | 64 | 64 |
| catcher residual bits | 2 | clamp(b+1, 10, 18) | 1 | 8 |

The first four serve every mode. The next eight are osu!standard only, and the last four osu!catch only. The time
model's two extra contexts are its last two, so its first four are the same for every mode.

Each decision's contexts, counter slot and weight set are part of the format but are not restated here: the key
rhythm, time history, tick phase, move history and so on, defined in `orz_frames.h` and `orz_modes.h`.

### 5.6 Priors

Before coding, the models start from a built-in prior: a starting state trained on real replays. The prior set is
chosen by the replay's mode:

| Set | Replays |
|---|---|
| 0, 1, 2 | modes 0 and 7 whose device model (§6.3) is rational, mouse or absolute |
| 3, 4, 5 | the same for modes 4 and 6 |
| 6 | modes 4 and 6 whose device is text |
| 7 | modes 0 and 7 whose device is text |
| 8, 9, 10 | osu!taiko (modes 1 and 5), osu!catch, osu!mania |

The device is the higher axis mode (§6.3) of the two axes' first segments.

A set lists its models in the order of the §5.5 table for the osu!standard form (12 models). For the other form
they are the first four, then the four catcher models.

- **SIZE and BALANCED** take counters from `frames_prior.h`. Each entry is the low 32 bits of a context hash, which
  selects the line under the replay's table size, and a value: the probability in bits 0–15, a confidence in bits
  16–23 and the slot in bits 24–31. Entries are applied in stored order, so a later one replaces an earlier one on
  the same counter.
- **SIZE** then copies each model's mixer weights from the same file.
- **SPEED** takes counters from `frames_fallback_prior.h`: the same value layout with the counter's index
  (`salt · width + slot`) in bits 24–31, and no keys.

A prior counter is set to `q = probability >> 5` and `n = min(confidence, 31)`.

The prior tables are normative data. Encoder and decoder need bit-identical copies.
The tables start from these priors for each replay; decoding needs no state from another replay.

## 6. The frames stream: osu!standard form

### 6.1 Head (bit field, in order)

1. **Delta table.** A frame delta is the time from one frame to the next, in milliseconds. The table lists each
   distinct delta of the replay once, and the frames later give a delta as its index in the table (§6.2). Its order
   is fixed by its three groups: the leading entries, then the low deltas in ascending order, then the rest. A low
   index costs the frames less, so the leading entries are the replay's most used deltas.
   - `n`, the number of entries in the whole table, as `small<5>`.
   - Then the `n` entries, in three groups, one after the other:
     1. **The leading entries.** There are 2 of them, or `n` if the table has fewer than 2: the replay's two most
        used deltas that are not negative, the most used first. Each is 1 bit, then:
        - if the bit is 1, the delta in 5 bits (0 to 31);
        - if it is 0, the delta as `length_bucket<4,0>` (§2).

        A leading entry is never negative. A table that has negative deltas and fewer than two others fills its
        leading entries with deltas the replay does not use.
     2. **The low deltas**, in ascending order. First two numbers: the start `s` in 5 bits (0 to 31), and the
        length `k` as `length_bucket<3,0>` (§2). A reader rejects a `k` above 127. Then:
        - every delta from 0 to `s − 1` is an entry, apart from those that are leading entries, which the table
          already has;
        - `k` bits follow, one for each delta from `s` to `s + k − 1`, the first for `s`. Every delta whose bit is
          1 is an entry.

        A reader reads all `k` bits, and stops adding entries once the table has `n`.
     3. **The rest**, until the table has `n` entries: every other delta, the most used first. Each is a sign bit,
        then a number `m` as `length_bucket<4,0>`. The delta is `−m` if the sign bit is 1, and `m` otherwise.

   The start and the length are the encoder's choice (§12). The start can be as high as the first delta the table
   does not hold. From there a delta within the length costs its one bit whether the replay uses it or not, and one
   past it costs its entry in the rest. The reference encoder takes the highest start, and then the length that
   makes the table smallest.

   A reader stops reading leading entries and the rest at the end of the data, so a truncated table has fewer than
   `n` entries.
2. **Time grid**: 3 bits choosing a period from `{none, 50/3, 25, 25/2, 100/3, 20}` ms (codes 0–5), or code 6 for a
   custom period of `num / den` ms (other play rates), followed by `num − 1` as `bucket(7)` and `den − 1` as
   `bucket(3)`. Each is stored less 1, so a reader adds 1 to what it reads and neither can be 0. A reader rejects
   code 7. The grid is only used as context.
3. **Device models** for x, then for y: §6.3.
4. **Autopilot**, for mode 7 only: the game-speed index (`small<2>`: 0 = 1.0, 1 = 1.5, 2 = 0.75), then the screen
   width and height, each `bucket(4, 8, 16)` and from 1 to 32768 (§6.6).
5. **Native grid** for x, then y: 1 bit. If 1, a 48-bit step (not 0) and a 64-bit offset in 32.32 fixed point,
   with the sum wrapping as a signed 64-bit integer:

   ```math
   \mathrm{value}(j) = \left\lfloor \frac{\text{step} \cdot j + \text{offset}}{2^{32}} \right\rfloor
   ```

   Every coded integer `j` of the axis is replaced by `value(j)` before its device model reads it.
6. **std_default_pos**: 1 bit; if 1, **std_default_time**: 1 bit.
   - `std_default_pos`: frames 0 and 1 are at the usual starting position (256, −500).
   - `std_default_time`: their deltas are also 0 and −1, and are not coded.

The coded body starts at the next byte boundary.

### 6.2 Coded body

Two raw bits come first: whether axis x has escapes, then whether axis y does. Then, for each frame:

1. **Keys**, folded so that K1 and K2 imply M1 and M2 (`k ^ ((k >> 2) & 3)`; unfolding is the same operation):
   - a "keys changed" decision;
   - if they changed: one flip decision for each of the 5 low key bits, from bit 0 up;
   - then a "high bits flipped" decision, skipped (and taken as 1) when no low bit flipped; if 1, the flips of
     the 27 bits above as raw bits.
2. **Delta**: the index into the delta table.
   - A walk down a fixed binary tree of 64 nodes, one decision per node from the root: its 65 leaves are the indexes
     0 to 63 and "64 or more". The tree (`TIME_TREE` in `orz_frames.h`) is normative data, and each node's
     decisions use that node's counter slot.
   - "64 or more" continues as `gamma(index − 63)`.
   - A reader takes an index past the table as its last entry.
   - Nothing is coded for frames 0 and 1 when `std_default_time` is 1.
3. **Repeat** (frames after the first): whether the position is bit-identical to the previous frame's.
4. **For a new position** (not a repeat):
   - an autopilot event, for mode 7 (§6.6);
   - then **each axis**, y first when the absolute value of the move predictor's window term is larger for y;
     ties put x first:
     1. the move residual: the coded integer's step minus the predicted step;
     2. an escape decision, if the axis has escapes;
     3. the slack index (§6.3), for an axis whose current segment is mouse, that moved and was not escaped.

After all frames come the escape values: every escaped position on axis x in order, then axis y (§6.4).

The move predictor, and how the residual and the slack index are split into decisions, are normative and not
restated here (`device_space.h`, `orz_frames.h`).

### 6.3 Device models

Each axis carries a list of segments, and each segment has one axis model. The count starts at 1, so every axis
has a model: an encoder must provide one for each axis, and a decoder must handle every mode. This outline defines how
a decoder turns a stored model into values, not how an encoder picks one (§12). Values a model does not reproduce are
escaped (§6.4).

**Text without device detection.** An encoder that does not detect devices can write each axis as one text segment
(mode 3). Text needs no device information and has no parameters: `K` is the value's text ordinal (§4.1), so it
fits any replay, and only negative zero and non-finite values are escaped. The whole model then
costs 7 bits: x is a count bit of 0 and mode 3, and y is a count bit of 0, a "same as x" bit of 0 and mode 3. The moves
are still predicted and coded, as steps between text ordinals. A device model makes those steps the device's own
counts, which are smaller and more regular, so detecting one usually gives a smaller file, but text is always valid.

Per axis: the segment count. It starts at 1, then you read 1 bit: if 0, the count stays as is; if 1, the next 5 bits are read and
then added to the value, with an additional +1. So the count is from 1 to 33. When both axes have one segment, y's count is
followed by a "same as x" bit (§6.3.1); an encoder may always write it as 0. Axis x never has this bit, nor does y
when either axis has more than one segment. Then, unless the bit is 1, each segment follows:

1. **Its length**, except for the last segment. The segments split the axis's new positions (the frames that are not
   repeats, §6.2) into runs, one after the other: the first segment starts at new position 0 and each later one
   where the one before it ends. A length is at least 1 and is stored less 1 as `length_bucket<4,0>` (§2), so a
   reader adds 1 to what it reads. The last segment runs to the end. For example, segments of 250, 350 and 400 new
   positions store 249 for the first, 349 for the second, and nothing for the third.
2. **Its mode**: 2 bits, one of the four modes below. Every code is a valid mode.
3. **The mode's parameters**, in the order below.

The models turn the coded integer `K` of each new position into a float:

| Mode | Value | Parameters, in order |
|---|---|---|
| 0, rational | $`(a K + b) / D`$, in double, rounded to float | `D`, `a`: `bucket(4, 8, 16)`; `b`: zigzag `bucket(4, 8, 16)` |
| 1, mouse | window pixels move by `counts · sensitivity` per input report, then map to the playfield | sensitivity (a setting); polling rate; play rate; unrounded subtraction (1 bit); the window |
| 2, absolute | a tablet coordinate `K` maps to the window, then to the playfield | mapping (1 bit: to the window, or to the desktop); x87 (1 bit); unrounded subtraction (1 bit); a rounding variant (1 bit); span: `bucket(4, 8, 16)`; origin: zigzag `bucket(4, 8, 16)`; window half-size: `bucket(4, 8, 16)`; area speed (a setting); the window |
| 3, text | `K` is the value's text ordinal (§4.1) | |

The shared parameter codings:

- **A setting**: 1 bit; if 0, hundredths as `bucket(4, 8, 16)`; if 1, the float's 32 bits. A reader rejects a
  setting that is 0, negative, infinite or NaN.
- **The polling rate**: 3 bits choosing from `{125, 250, 500, 1000, 2000, 4000, 8000}` Hz, or code 7 followed by the
  rate in whole Hz in 13 bits (0 to 8191).
- **The play rate**: 2 bits choosing from `{1.0, 1.5, 0.75}`, or code 3 followed by 31 bits, a float without its
  sign bit. A reader rejects one that is 0, infinite or NaN.
- **The window**: 1 bit; if 1, a 5-bit index into 24 common sizes (a reader rejects an index of 24 or more); if 0,
  the axis's scale and offset as two 32-bit floats. A reader rejects a scale that is not a positive normal float
  (0, subnormal, negative, infinite or NaN) and an offset that is infinite or NaN. The sizes, in order: 1920x1080, 2560x1440, 1366x768, 1600x900, 1680x1050, 1024x768, 1440x900,
  1280x800, 2560x1080, 1280x1024, 1920x1440, 1360x768, 1152x864, 800x600, 1280x960, 1920x1200, 3840x2160,
  3440x1440, 1400x1050, 1600x1200, 2560x1600, 1600x1024, 1280x720, 1536x864.

For an absolute axis, the mapping bit is 1 for window and 0 for desktop. The rounding-variant bit selects
`product_rounded` for window mapping or `origin_unrounded` for desktop mapping, as defined in `device_space.h`.

A reader also rejects a rational `D` of 0 and an absolute span of 0. With these rules and the ones above, no
model computes a NaN from its parameters (§15).

The arithmetic of each model replays the game's own, and its rounding at each step is part of the format
(`device_space.h`). Its float values are binary32 and its double intermediates binary64. The variant named x87
emulates the game's 53-bit precision; the stored variant bits select the rounding steps. Fused multiply-add and
reassociation change the result and must not be substituted for those steps (§15).

**Window to playfield**: $`\text{field} = (\text{pixel} - \text{offset}) / \text{scale}`$.

**mouse**:

- A nonzero step `ΔK` is split into `m` reports over the frame's `dt` ms, rounding half away from zero:

  ```math
  m = \mathrm{clamp}\!\left(\mathrm{round}\!\left(\frac{dt}{\text{play rate}} \cdot \frac{\text{poll}}{1000}\right),\ 1,\ \min(|\Delta K|,\ 2^{16})\right)
  ```
- Each report carries `⌊|ΔK| / m⌋` counts, and the first `|ΔK| mod m` reports one more. The pixel position is
  advanced in double and rounded to float after each report, and is clamped to the window when the move starts or
  ends outside it.
- Report splits and float rounding can leave several candidate floats. A slack index chooses one: 0 is the value
  above, then the values of the neighbouring report counts (`m − 1`, `m + 1`, … `m − 8`, `m + 8`), skipping invalid
  counts and bit-identical duplicates; then values +1, −1, +2, −2, … ulps from candidate 0. The index is at most 48.
- An escaped position seeds the pixel position from its true value. The first position of a segment has no
  prediction.

**text**: `K` is the value's text ordinal (§4.1). Values without one are escaped.

Rational, absolute and mouse axes **hold** after an escape: while that axis's integer step is zero, positions keep
the escaped value. A new segment ends the hold.

#### 6.3.1 Sharing x's model

Sharing is optional for an encoder: writing the "same as x" bit as 0 and then y's segment in full is always valid.
A decoder must handle the bit being 1.

When the bit is 1, y has no parameters of its own and its one segment is derived from x's:

- x rational: y is a copy of x's model;
- x mouse with a common window: y is a copy of x's model, with that window's y offset.

A reader rejects the bit being 1 for any other x model.

### 6.4 Escape values

An escape value is the true position where the model's value is wrong. It is coded against the model's prediction,
which text and a mouse segment's first position do not have:

1. on an axis with a native grid, a decision for "one of the two lattice values next to the grid value", then which;
2. without a prediction, 32 raw bits;
3. otherwise a **near** decision, and 32 raw bits if it is 0. A near value is its distance from the prediction:
   in 7-digit mantissas at the same exponent, or in ulps with shortest text.

### 6.5 Rebuilding the frames

1. Each new position takes the next decoded value of each axis; repeated frames take the previous frame's position.
   Frame 0 is always a new position.
2. Deltas come from the delta table and keys are unfolded.
3. With `std_default_pos`, frames 0 and 1 are set to (256, −500).

### 6.6 Autopilot

Mode 7 predicts cursor movement from a path generated by the game. Path predictions use 7-digit text ordinals
(§4.1); the axis models still turn decoded integers into coordinates. The reference encoder fits this model with
7-digit text and text axes. The path is a list of moves of two types: a straight line, or an in-out cubic ease
sampled every `1000/60 · speed` ms. Each move may start with a wait.

**A path event** is a decision at each new position after the first whose time is past the end of the path. If 1,
the next move follows: its type, wait and duration, then per axis its start (an ulp offset, coded only when the move
does not start at the path's last point) and its target (an integer offset, or an offset from a guess).

**A frame on the path** is predicted as the linear interpolation between the path points around its time, mapped
field → screen → field with the stored screen size. A frame off the path is predicted by the line through the last
two new positions. Residuals are still coded.

The path arithmetic is normative (`orz_autopilot.h`).

## 7. The frames stream: osu!taiko, osu!catch and osu!mania form

**Head**: the delta table and time grid as in §6.1. The coded body starts at the next byte boundary.

**Coded body**, in order:

1. **Escaped frames**: `gamma(count + 1) − 1`. Then, per escaped frame:
   - its gap from the frame after the previous escaped frame (from frame 0 for the first), as `gamma(gap + 1) − 1`;
   - a raw bit for "at (256, −500)", or else its x and y as 32 raw bits each.
2. **osu!mania only**:
   - the column count, `gamma(columns)`, from 1 to 18 (the reader clamps larger counts to 18);
   - the scroll-speed changes: `gamma(count + 1) − 1`, then per change its frame, as `gamma(gap + 1) − 1` from the
     previous change's frame, and the speed as 32 raw bits. The speed starts at 0.
3. **Per frame**:
   - keys, with the key decisions of §6.2 but not folded, and with the column count in place of 5 low bits for
     osu!mania; the raw high part has `32 − columns` bits in that case. An escaped osu!mania frame codes no keys,
     and the held columns carry over;
   - the delta, as in §6.2;
   - for osu!catch, the catcher position, unless the frame is escaped.

| Mode | Coded keys | x | y | Escaped when |
|---|---|---|---|---|
| osu!taiko | the keys | 0 if keys 1 or 2 are held, else 640 if keys 4 or 8 are, else 320 | 9999 | x or y differs from that |
| osu!catch | the keys | catcher position, coded as a text value | 9999 | y ≠ 9999, or x has its sign bit 1 or is not finite |
| osu!mania | held columns (keys are always 0) | the columns as an integer | current scroll speed | the frame is at (256, −500) |

**The catcher** is coded in three parts:

1. a decision for whether it moved; if not, keep the previous x and stop;
2. a decision for whether it moved in the same direction as last time;
3. the residual, in text ordinals, against a prediction of walking or dashing for the frame's delta, scaled by
   any hyperdash.

The catcher's prediction and how the residual is split into decisions are normative (`orz_modes.h`).

## 8. The `osr_meta_data` container

It holds the `.osr` bytes outside the replay data in three parts: **the header** (game mode through mods), **the
timestamp** (8 bytes) and **the tail** (what follows the replay data, usually the online score id). The life bar
(§9) follows them to the end of the container. When nothing follows, the `.osr` had no life bar.

The container's first byte picks the form of the three parts:

- **1, fields**: the bit field below, padded to a byte boundary.
- **any other value, raw** (an encoder writes 0): each part as a ULEB128 length and its bytes.

| Field | Coding |
|---|---|
| game mode | 2 bits |
| client version | `bucket(18)` of `zz(version − 20200000)` |
| player name | 1 bit for present; then its length as `bucket(4, 8)` and its bytes |
| beatmap MD5 | 16 bytes |
| replay MD5 | 1 bit; if 1, a 6-bit index into 49 known formulas over the score fields; otherwise 16 bytes |
| count of 300s | `bucket(9, 11)` |
| counts of 100s, 50s, gekis, katus, misses | `bucket(5, 8)` each |
| score | 32 bits |
| combo | `bucket(9, 11)` |
| perfect | 1 bit |
| mods | `bucket(16)` |
| timestamp | Unix seconds in 32 bits; 1 bit; if 1, 24 bits of 100 ns ticks |
| online score id | 2 bits: none, zero, 32 bits or 64 bits; then the id |
| rest of the tail | its length as `bucket(4, 8)` and its bytes |

Strings are rebuilt with the `0x0b` marker and a minimal ULEB128 length, MD5s as 32 lowercase hex digits, and an
absent player name as `0x00`. The replay MD5 formulas are normative data (`orz_parts.h`).

The timestamp is rebuilt as the little-endian `i64` .NET tick count
`621355968000000000 + seconds · 10000000 + sub`, with `sub = 0` when absent. Score-id kinds 1–3 each rebuild an
eight-byte little-endian id; kind 1 rebuilds zero without storing any id bits, and kind 0 adds no id bytes.

An encoder only uses the fields form when decoding it gives back all three parts exactly.

## 9. The life bar

The life bar follows the parts in the `osr_meta_data` container (§8). Its first byte picks the form:

- **0**: the life bar text as stored.
- **any other value, compact** (an encoder writes 1): points of `time|value`, range coded.
  - Each time is coded as the frame it falls on (counted from a guess) or as an escaped difference. Frame times
    are cumulative frame deltas: the decoded frames including the closing seed frame, or the frames parsed from
    verbatim replay text (§14).
  - Each value is coded in hundredths.
  - The decoder prints each point as `time|value,`, with the value as `0`, `1`, or `0.d` / `0.dd`.

A present but empty life bar is the single form byte 0. An absent life bar has no bytes after the metadata parts.

An encoder only uses the compact form when it is smaller and decodes back to the same text. The point coding, the
range coder with its frequency models, and their priors are normative for this form (`orz_life.h`, `rc.h`,
`life_prior.h`).

## 10. Verbatim replay data

Verbatim storage keeps the original LZMA bytes in `osr_verbatim`, with no `frames` container, no seed and a frame
count of 0. The reference encoder uses it when any of these holds:

- the replay data is 13 bytes or shorter, the low 32 bits of its declared text size exceed 2^28, or decompression
  produces a different number of bytes than that low-32-bit size (see the LZMA validation limitation in §13);
- the replay text is empty;
- neither float style reprints the text exactly (§4), after removing one trailing `,`. The 7-digit style is tried
  first;
- there are more than 2^22 frames, counting a closing seed frame;
- outside osu!mania, some `abs(x)` exceeds 2^17; in any mode, some `abs(y)` exceeds 2^17, or a coordinate is NaN;
- an osu!mania frame has keys other than 0, or an x that is not a whole number from 0 to 2^18 − 1.

## 11. Rebuilding the `.osr`

With `has_osr_header`, the decoder writes, in order:

1. the header bytes (§8);
2. the life bar string: `0x0b`, a ULEB128 length and the text, or `0x00` if there is none;
3. the 8 timestamp bytes;
4. a `u32` byte count;
5. the LZMA replay data;
6. the tail.

Without `has_osr_header`, it writes the replay data and tail only.

**The replay data** is the verbatim bytes (§10), or the text of §4 compressed again as an LZMA stream: 5 property
bytes, the `u64` size and no end mark. The compression settings are the decoder's choice. The reference decoder
uses the LZMA SDK's fast encoder (`compress_osr_string` in `orz_shared.h`); the resulting bytes and size need not
match the original stream.

**Minified output.** `DECODE_MINIFY` is a reference-decoder option (`orz_minify.h`), not a file flag. It can discard
or resample frames and changes the output text; the exact-preservation guarantees of §1 do not apply to it.

## 12. What the encoder chooses

A decoder only reads what the encoder chose. The format leaves these choices free:

- the compression priority;
- the device model of each axis (one is required; text fits every replay, §6.3), its segments, the native grids, which
  positions are escaped, the slack indexes, whether y shares x's model (§6.3.1), and whether autopilot is on
  (mode 7);
- which deltas lead the delta table, its start and length and the order of the rest, and the time grid;
- whether to use the fields form of §8 and the compact form of §9;
- storing a replay verbatim when it need not be.

The reference encoder chooses by fitting candidates and comparing coded sizes, and can be given the setups of a
player's earlier replays as hints. None of that search is part of the format. It also stores a replay verbatim when
its frames stream is not smaller than the LZMA data it replaces and, when asked to check the round trip
(`ENCODE_CHECK_ROUND_TRIP`), when decoding its output does not give back the text.

The reference encoder writes no `.orz` for input that is not an `.osr` it can give back: a file shorter than 128
bytes, a game mode above 3, or a header or replay data length that runs past the end of the file.

## 13. Known gaps in the reference implementation

The exact-preservation guarantees have these known implementation limitations:

- **A closing seed frame with −0 in either coordinate** (shortest text) comes back with +0 in both coordinates.
  The CRC does not catch it; the round-trip check does.
- **An osu!mania frame with x = −0** (shortest text) passes the column check but decodes as +0, causing the CRC
  check to fail. The encoder's optional round-trip check falls back to verbatim storage.
- **Mode 7 passed directly to `replay_data_to_orz` without a fitted autopilot path** can produce a frames stream
  without the required autopilot head fields. Decoding fails; the optional round-trip check falls back to verbatim.
- **A life bar marker byte other than `0x00` or `0x0b`** comes back as an empty life bar (`0x0b 0x00`). A non-minimal
  ULEB128 length comes back minimal.
- **LZMA input validation** reads only the low 32 bits of the eight-byte text size, checks those against 2^28 and
  checks the number of output bytes. It ignores the high 32 bits, the LZMA decoder's return code and status, and
  unused input bytes. A malformed stream can therefore be accepted as frame input instead of preserved verbatim.

## 14. Normative data and arithmetic

The following definitions complete the outline. They belong to the same format version as the field layouts;
changing them changes how a file decodes, even if its header layout stays the same.

| Definition | Owning source |
|---|---|
| Bit fields, bucket codes and outer container | [`orz_shared.h`](orz_shared.h) |
| Probability counters, hashes, mixing and binary arithmetic coder | [`cm.h`](cm.h) |
| Time grid | [`orz_time.h`](orz_time.h) |
| Time-index tree, contexts, slots, weight sets, and residual, slack and escape decisions | [`orz_frames.h`](orz_frames.h) |
| Initial frame counters and mixer weights | [`frames_prior.h`](frames_prior.h), [`frames_fallback_prior.h`](frames_fallback_prior.h) |
| Device arithmetic, window-size table and move predictor | [`device_space.h`](device_space.h) |
| Native grid and frame-head reconstruction | [`orz_encoder_std.h`](orz_encoder_std.h) |
| Text formatting and text ordinals | [`orz_dotnet.h`](orz_dotnet.h), [`orz_decimal.h`](orz_decimal.h) |
| Replay text parsing and frame printing | [`orz_text.h`](orz_text.h) |
| Autopilot events and path arithmetic | [`orz_frames.h`](orz_frames.h), [`orz_autopilot.h`](orz_autopilot.h) |
| Other modes, catcher arithmetic and their model sizes | [`orz_modes.h`](orz_modes.h), [`orz_tables.h`](orz_tables.h) |
| Metadata fields and replay-MD5 formulas | [`orz_parts.h`](orz_parts.h) |
| Life-bar point coding, range coder and priors | [`orz_life.h`](orz_life.h), [`rc.h`](rc.h), [`life_prior.h`](life_prior.h) |

## 15. Floating-point arithmetic

The CRC (§3.4) covers the decoded frames' bits, so a decoder must give the same floats on every machine. IEEE 754
defines `+`, `−`, `·`, `/`, comparisons and conversions exactly, and the models use nothing else. No maths library
function (`pow`, `log`, …) is part of decoding, as their results differ between libraries.

That holds only when the arithmetic is plain IEEE 754:

- each operation is rounded once, to its own format: no 80-bit x87 intermediates;
- rounding is to nearest, ties to even;
- subnormals are kept: no flush-to-zero or denormals-are-zero mode;
- nothing is contracted into a fused multiply-add, and nothing is reassociated.

SSE2 on x86 and AArch64 give this. A C or C++ build must not use fast-math options, and GCC and Clang need
`-ffp-contract=off`. A program hosting the decoder must not leave the processor in a flush-to-zero mode.

**NaN.** A NaN's bits are not the same everywhere: an operation that makes a new NaN gives `0xFFC00000` on x86 and
`0x7FC00000` on ARM. The reader's parameter rules (§6.3) keep every model away from 0 · ∞, ∞ − ∞ and 0 / 0, so a
model's result can overflow to an infinity but never becomes a NaN. Escape values (§6.4) are the replay's own floats
and are copied as stored. A stored NaN that seeds a mouse axis only passes through operations with one NaN input,
which keep that NaN's bits (made quiet) on both.
