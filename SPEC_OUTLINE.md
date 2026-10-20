# ORZ format outline

An overview of the `.orz` file format: what a file holds, how it is laid out, and what a decoder must do to get the
replay back. It describes the format rather than this repository's code. Where the format depends on data that is
too large to restate (model tables, symbol orders, context definitions), the outline names that data and where it
is defined.

The format is version 0 and still changing. Every format change so far has been breaking, and the version number
has not yet been raised.

## 1. Scope and guarantees

An `.orz` file holds one osu! replay, given either as a full `.osr` file or as bare replay data (the LZMA stream of
replay text, as servers store it).

Decoding gives back:

- **the replay text exactly** (the LZMA-decompressed `delta|x|y|keys,` list);
- **the `.osr` bytes around the replay data exactly**: the header, the life bar text, the timestamp and the bytes
  after the replay data (exceptions in §13);
- **the LZMA bytes of the replay data recompressed** the way osu! compresses them (§11), which gives back the original
  bytes for replay data osu! wrote. Files that store the replay data verbatim (§10) come back exactly too.

Frames are reproduced to the text they print as, not always bit for bit. With classic 7-digit text (§4), a float
only has to print the same 7 significant digits.

## 2. Conventions

- **Bit packing.** Bit fields are written least significant bit first, and bits fill each byte starting from its low
  bit. Reading past the end of a bit field gives 0.
- **Lists.** A list with neither a fixed length nor an upper bound must end with a sentinel. The sentinel may be a
  direct value, or inferred. A list may define its own sentinel, but reading 0 must also end the list. Reading past
  the end of a bit field gives 0, so damaged or truncated data always ends its lists. Every such list added to the
  format must follow this rule.
- **`bucket(b1, b2, …)`** is a variable-length unsigned code. For each width `b` in turn:
  - if the value is below `2^b − 1`, write it in `b` bits and stop;
  - otherwise write `b` one bits, subtract `2^b − 1` and continue.

  If every width escapes, let `v = value + 1` and `e = floor(log2 v)`; write `e` in 5 bits, then the `e` bits of
  `v` below its leading 1. `bucket()` with no widths is that final form alone.
- **`small<N>(v)`**: `N` bits if `v < 2^N − 1`. Otherwise `N` one bits, then `floor(log2 v) − (N − 1)` further
  one bits, a zero, and the `floor(log2 v)` bits of `v` below its leading 1.
- **zigzag** maps signed to unsigned:

  ```math
  \mathrm{zz}(v) = \begin{cases} 2v & v \ge 0 \\ -2v - 1 & v < 0 \end{cases}
  ```
- **ULEB128**: the usual 7-bits-per-byte length used by `.osr` strings.
- **Coded values** inside the arithmetic-coded body (§5) are binary decisions. "Raw" bits there are coded at
  probability 1/2. `gamma(v)` there is Elias gamma: `floor(log2 v)` as unary decisions, then the bits below the
  leading 1, most significant first.

## 3. Container

```
byte 0     magic 0xE6
byte 1..   header, a bit field padded to a byte boundary
then       the containers' data, back to back, in the order the header lists them
```

### 3.1 Header (in order)

| Field | Coding | Notes |
|---|---|---|
| version | `bucket(4, 8)` | 0. A reader rejects a larger version as "made by a newer version", before reading on. |
| crc | 32 bits | §3.4 |
| encode info | `bucket(8)` | §3.3 |
| seeded | 1 bit | the replay ended with a closing seed frame (§3.5) |
| seed | `length_bucket<3,18>`, if seeded | see below |
| frame count | `bucket(16)` | frames held by the frames stream, without the seed frame; at most 2^22 |
| standard containers | §3.2 | |
| custom containers | §3.2 | |

`length_bucket<3,18>`:

- a 3-bit code `min(bitlen − 18, 7)`, where `bitlen` is the value's bit length;
- code 7 escapes: 5 bits of `bitlen < 18 ? bitlen : bitlen − 7` follow;
- then the `bitlen − 1` bits below the leading 1.

### 3.2 Containers

The header lists the containers, each with its size (at most 2^28 bytes). Their data follows the header in the same
order, each exactly its size: the standard containers first, then the custom ones.

**Standard containers.** Each one is announced by a 1 bit, and a 0 bit ends the list. Then:

1. **tag**: the previous container's tag plus 1 (for the first container, 0 plus 1). If a 1 bit follows, a
   `bucket(4)` is added on top.
2. **size**: `bucket(16)`.

Tags are 8-bit and must rise by at least 1 from one container to the next. A tag can not stay the same, go down, or
overflow past 255 (it does not wrap around). A reader rejects a file whose tags break this. Tag 0 never occurs.

| Tag | Container | Present when |
|---|---|---|
| 1 | `frames`: the frames stream (§6, §7) | `replay_verbatim` is clear |
| 2 | `osr_meta_data`: the `.osr` bytes around the replay data, and the life bar (§8, §9) | `has_osr_header` |
| 3 | `osr_verbatim`: the original LZMA replay data (§10) | `replay_verbatim` is set |

So exactly one of `frames` and `osr_verbatim` is present. An empty container is not written. A reader ignores
standard tags it does not know.

**Custom containers** carry data outside the format, such as a tool's own metadata. Decoding the replay ignores them.

- 1 bit: whether there are any. If set, `bucket(4)` gives their count minus 1.
- Per custom container:
  1. **name**: its length in 5 bits (at most 31 characters), then each character in 7 bits (ASCII);
  2. **size**: `bucket(16)`.

### 3.3 Encode info

| Bits | Name | Meaning |
|---|---|---|
| 0–3 | mode | what the replay is, and so the form of its frames stream (below) |
| 4 | `has_osr_header` | the input was a full `.osr`, so the decoder rebuilds one |
| 5 | `trailing_comma` | the replay text ended with `,` |
| 6 | `replay_verbatim` | the replay data is stored as-is (§10) |
| 7 | `text_shortest` | floats print as shortest round-trip text, as in osu!lazer; otherwise as 7 significant digits, as in osu!stable (§4) |

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

Modes 0 to 3 are the `.osr`'s game mode numbers. Modes 8 to 15 are reserved for later versions: a reader rejects a
file that uses one.

Relax does not exist in osu!mania, and autopilot only in osu!. Relax selects the relax prior sets (§5.6) and is only
needed to decode the osu!standard form. The game mode is the `.osr`'s, or the one given with bare replay data, with
osu! when none is given; it is not needed to decode. osu!mania replays that do not fit the column form (keys set, x not
whole held columns) are stored verbatim (`replay_verbatim`). Autopilot replays the path model does not fit are stored as
mode 0 (the encoder decides from the frames).

### 3.4 CRC

CRC-32C (Castagnoli polynomial), starting from `0xFFFFFFFF` with no final XOR, over:

1. **Every decoded frame** as 16 little-endian bytes: `i32 delta`, `f32 x`, `f32 y`, `u32 keys`. This includes the
   closing seed frame `{−12345, +0, +0, seed}` when there is one. These are the frames as the decoder rebuilds them
   (§1), not the parsed input.
2. **`osr_meta_data` and `osr_verbatim`, in that order**, each when present and not empty: its `u32` size, then its
   bytes.
3. **The encode info**, as stored (§3.3), as a `u32`.

The CRC does not cover the magic, the header layout or the frames stream's own bytes, only what they decode to.

### 3.5 Seed frame

A replay text that ends with the frame `-12345|0|0|<seed>` stores `<seed>` in the header instead of as a frame. The
decoder appends `{−12345, +0, +0, seed}` after the decoded frames.

## 4. Replay text

The replay text is a list of frames `delta|x|y|keys` separated by `,`, with an optional trailing `,`. Each frame is
an `int` delta, `float` x and y, and `u32` keys. A file stores frames and rebuilds the text by printing them, so the
format requires the text to be exactly what printing would give.

Frames are joined by `,` with no trailing `,` (`trailing_comma` adds one). `delta` and `keys` print as plain integers, and x and
y print in one of two float styles:

- **7 significant digits** (`text_shortest` clear), as .NET Framework's default formatting:
  - the digits are the float's exact binary value rounded half away from zero, with trailing zeros removed;
  - both 0 and −0 print as `0`.
- **Shortest round-trip** (`text_shortest` set): the shortest digits that parse back to the same float; −0 prints as `-0`.

Both styles share one layout, where `e` is the decimal exponent of the leading digit:

- `e ≤ −5` or `e ≥ 7`: `d.ddddE±XX`, with at least two exponent digits;
- `e < 0`: `0.`, then `−e − 1` zeros, then the digits;
- otherwise: the digits with a decimal point after `e + 1` of them, zero-padded, and no point when there are no
  fractional digits.

`NaN` and `±Infinity` print as `NaN`, `Infinity` and `-Infinity`.

An encoder must store the replay verbatim (§10) instead of as frames when any of these holds:

- the replay data does not decompress to its declared size;
- the replay data decompresses to a size of 0
- neither float style reprints the text exactly;
- there are more than 2^22 frames;
- some `|x|` or `|y|` exceeds 2^17 (x may reach 2^24 in osu!mania), or a value is NaN.

## 5. The binary coder

Every frames stream is a short bit-field head, then one arithmetic-coded body. The body is a sequence of binary
decisions, each predicted by context mixing. Encoder and decoder must compute identical predictions, so all model
arithmetic is integer-only and normative.

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
decodes `y = (x ≤ xmid)`. It shifts input in the same way, reading 0 for any byte past the end of the stream.

The decoder reads exactly as many bytes as the encoder wrote, so a valid stream is never read past its end.

### 5.2 Counters

A counter holds a 16-bit probability `p` (starting at 32768) and a count `n` (starting at 0, at most 255). After a bit
`y`:

- move `p` toward the bit, by a rate that falls as `n` grows:

  ```math
  p \leftarrow p + \left\lfloor \frac{(65535\,y - p)\cdot \mathrm{RATE}[n]}{2^{16}} \right\rfloor,
  \qquad \mathrm{RATE}[n] = \left\lfloor \frac{131072}{2n + 3} \right\rfloor
  ```
- then `n` increases by 1 if it is below 255.

### 5.3 Models

Each kind of decision has a model:

- **a table of counters in lines**: `2^bits` lines of `width` counters each, where a line is found by
  `hash & (2^bits − 1)`, with no collision check;
- **a set of mixer weights** per selector, each starting at 19661 (0.3 in 16.16 fixed point);
- **an APM table** of 33 entries per context.

Binding a decision's `N` contexts (`N` ≤ 8) gives one line per input: input `i`'s line is
`hash(salt · 8 + i, context_i)`. The decision then uses the counter at its slot in each bound line. `hash(a, b)` works
on `u64`:

```
h = (a + 1) · 0x9E3779B97F4A7C15 ^ b · 0xD6E8FEB86659FD93
h ^= h >> 29;  h *= 0xBF58476D1CE4E5B9;  h ^= h >> 32
```

### 5.4 Mixing and the coded probability

`squash` maps a stretched value `d` to a 12-bit probability by interpolating a 33-entry table `T`. With
$`i = \lfloor d / 128 \rfloor + 16`$ and $`r = d - 128 \lfloor d / 128 \rfloor`$:

```math
\mathrm{squash}(d) =
\begin{cases}
1 & d < -2047 \\
4095 & d > 2047 \\
\left\lfloor \dfrac{T[i]\,(128 - r) + T[i+1]\,r + 64}{128} \right\rfloor & \text{otherwise}
\end{cases}
```

The table is:

```
T = {1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101, 1546, 2047, 2549, 2994, 3348, 3607, 3785,
     3901, 3975, 4022, 4050, 4068, 4079, 4085, 4089, 4092, 4093, 4094}
```

`stretch(p)` is the smallest `d` in −2047…2047 with `squash(d) ≥ p` (2047 if none).

For one decision with counters $`p_i`$ (one per input), weight set $`w`$ and APM table $`A`$ at context `a`:

1. **Mix** the stretched counters (`dot` in 64-bit):

   ```math
   x_i = \mathrm{stretch}\!\left(\left\lfloor \frac{p_i}{16} \right\rfloor\right), \qquad
   p_m = \mathrm{squash}\!\left(\mathrm{clamp}\!\left(\left\lfloor \frac{\sum_i x_i w_i}{2^{16}} \right\rfloor,\ -2047,\ 2047\right)\right)
   ```
2. **Refine** with the APM, interpolating between two of its entries:

   ```math
   s = \mathrm{stretch}(p_m) + 2048, \quad
   j = 33a + \left\lfloor \frac{s}{128} \right\rfloor, \quad
   t = s \bmod 128, \qquad
   p_a = \left\lfloor \frac{A[j]\,(128 - t) + A[j+1]\,t}{128} \right\rfloor
   ```
3. **Code** the bit with a quarter of the mixer and three quarters of the APM:

   ```math
   p = \mathrm{clamp}\!\left(\left\lfloor \frac{16\,p_m + 3\,p_a}{4} \right\rfloor,\ 32,\ 65504\right)
   ```

After coding `y`, in this order:

1. for each input, update its counter (§5.2), then its weight:

   ```math
   w_i \leftarrow w_i + \left\lfloor \frac{40\, x_i\, (4096\,y - p_m) + 2^{15}}{2^{16}} \right\rfloor
   ```
2. move both APM entries toward the bit, scaled to 16 bits as `τ = 65535 · y`:

   ```math
   A[j] \leftarrow A[j] + \left\lfloor \frac{5 \left\lfloor (\tau - A[j])\,(128 - t) / 128 \right\rfloor}{256} \right\rfloor, \qquad
   A[j+1] \leftarrow A[j+1] + \left\lfloor \frac{5 \left\lfloor (\tau - A[j+1])\, t / 128 \right\rfloor}{256} \right\rfloor
   ```

APM entries start at $`A[33c + k] = 16 \cdot \mathrm{squash}(128\,(k - 16))`$ for $`k = 0 \ldots 32`$.

### 5.5 Table sizes

A model's table size depends on the header's frame count, up to 16,384 frames (`b = bit_width(min(frame count,
16384))`), so a decoder must size every table exactly as the encoder did. The sizes are part of the format:

| Model | Inputs | Table bits | Width | Mixer sets | APM contexts |
|---|---|---|---|---|---|
| keys changed | 5 | clamp(b+4, 16, 22) | 1 | 64 | 64 |
| key bits | 4 | clamp(b+2, 14, 20) | 1 | 64 | 64 |
| high key bits | 1 | 8 | 1 | 1 | 1 |
| time | 4; 6 for relax (modes 4 and 6) and osu!taiko (modes 1 and 5) | clamp(b, 12, 16) | 64 | 576 | 192 |
| repeat | 4 | clamp(b+4, 16, 22) | 1 | 64 | 64 |
| move | 6 | clamp(b+3, 12, 17) | 32 | 384 | 1536 |
| move bits | 2 | clamp(b+2, 12, 20) | 1 | 32 | 256 |
| slack | 1 | clamp(b−2, 10, 16) | 16 | 64 | 64 |
| escape | 5 | clamp(b+2, 12, 20) | 1 | 48 | 48 |
| escape value | 2 | 10 | 64 | 1 | 1 |
| autopilot event | 5 | clamp(b, 10, 16) | 1 | 8 | 8 |
| autopilot value | 2 | 12 | 64 | 1 | 1 |
| catcher moved | 6 | clamp(b+3, 12, 20) | 1 | 64 | 64 |
| catcher direction | 4 | clamp(b+1, 10, 18) | 1 | 16 | 16 |
| catcher residual | 5 | clamp(b, 10, 18) | 64 | 64 | 64 |
| catcher residual bits | 2 | clamp(b+1, 10, 18) | 1 | 8 | 8 |

The first four serve every mode. The next eight are osu!standard only, and the last four osu!catch only. The time
model's two extra inputs (the older deltas, and the last three frames' repeat flags, or for osu!taiko whether the keys
stayed) are its last two, so its first four contexts are the same for every mode.

### 5.6 Priors

Before coding, the models start from a built-in prior: a starting state trained on real replays. The prior is chosen
by the replay's mode:

| Set | Replays |
|---|---|
| 0, 1, 2 | osu!standard whose device model (§6.3) is rational, mouse or absolute |
| 3, 4, 5 | the same for relax replays (modes 4 and 6) |
| 6 | relax without a device model |
| 7 | osu!standard without a device model (mostly autopilot) |
| 8, 9, 10 | osu!taiko, osu!catch, osu!mania |

The device is the higher `axis mode` value (§6.3) of the two axes' first segments. A text axis therefore counts as "no
device model".

A prior holds three things per model:

- **counters**, each given by the low 32 bits of its context hash, a slot, a probability and a confidence (the
  starting `n`);
- **the mixer weights**;
- **the APM table**.

Counters are written into the line their key selects under the replay's table size, and a later entry replaces an
earlier one that lands on the same counter. Weights and APM tables are copied whole.

The prior tables are normative data. Encoder and decoder need bit-identical copies, and changing them is a format
change.

## 6. The frames stream: osu!standard form

### 6.1 Head (bit field, in order)

1. **Delta table.**
   - `small<5>` entry count `n`, then the `n` distinct frame deltas as symbols:
     - symbol `v` for 0 ≤ v < 32;
     - otherwise symbol `32 + 2 · (bit_width(|v|) − 1) + (v < 0)`, followed by the bits of `|v|` below its leading 1,
       most significant first.
   - Each symbol is written as its rank in a fixed ordering row, coded as Elias gamma of `rank + 1`. Symbols below 32
     already in the table are skipped when ranking.
   - The first entry uses row 33, and each later entry uses row `min(previous symbol, 32)`. The 34 rows, each a
     permutation of the 96 symbols, are normative data.
2. **Time grid**: 3 bits choosing a period from `{none, 50/3, 25, 25/2, 100/3, 20}` ms (codes 0–5), or code 6 for a
   custom period of `num / den` ms, followed by `num` as `bucket(7)` and `den` as `bucket(3)` (other play rates, such as
   osu!lazer's rate mods: 1.4× is 70/3).
   A reader rejects code 7, and a custom period where either `num` or `den` is outside the range of 1 to 65535.
   It is only used as context.
3. **Device model** for x, then for y: §6.3.
4. **Autopilot**, for mode 7 only: the game speed (`small<2>`: 1.0, 1.5 or 0.75) and the screen width and height (§6.6).
5. **Native grid** for x, then y: 1 bit. If set, a 48-bit step and a 64-bit offset in 32.32 fixed point, with the sum
   wrapping as a signed 64-bit integer:

   ```math
   \mathrm{value}(j) = \left\lfloor \frac{\text{step} \cdot j + \text{offset}}{2^{32}} \right\rfloor
   ```

   It maps the coded integers of a single rational or absolute segment onto the device's own lattice.
6. **std_default_pos**: 1 bit; if set, **std_default_time**: 1 bit.
   - `std_default_pos`: frames 0 and 1 are at (256, −500), the position every replay starts at.
   - `std_default_time`: their deltas are also 0 and −1, and are not coded.

The coded body starts at the next byte.

### 6.2 Coded body

Two raw bits come first: whether axis x has escapes, then whether axis y does. Then, for each frame:

1. **Keys**, folded so that K1 and K2 imply M1 and M2 (`k ^ ((k >> 2) & 3)`):
   - a "keys changed" decision;
   - if they changed: one flip decision for each of the 5 low key bits, then any flips above them;
   - the bits above are 27 raw bits, preceded by a "high bits flipped" decision unless no low bit flipped.
2. **Delta**: the index into the delta table.
   - A walk down a fixed binary tree of 64 nodes, one decision per node from the root: its 65 leaves are the indexes
     0 to 63 and "64 or more". The tree (`TIME_TREE`, shaped by how often each index is used) is normative data, and
     each node's decisions use that node's counter slot.
   - "64 or more" continues as `gamma(index − 63)`.
   - An encoder never codes a leaf past the table. A reader takes one as the escape, or as the last entry when the
     table has no more than 64.
   - Nothing is coded for frames 0 and 1 when `std_default_time` is set.
3. **Repeat** (frames after the first): whether the position is bit-identical to the previous frame's.
4. **For a new position** (not a repeat):
   - an autopilot event, for mode 7 (§6.6);
   - then **each axis**, in the order the move predictor expects to move more first:
     1. the move residual: the coded integer's step minus the predicted step;
     2. an escape decision, if the axis has escapes;
     3. for a mouse axis that moved and was not escaped, the slack index (§6.3).

After all frames come the escape values: every escaped position on axis x in order, then axis y (§6.4).

The contexts of each decision are part of the format but are not restated here. They are the key rhythm, time history,
60 Hz tick phase, move history and so on, defined in the reference implementation (`orz_frames.h`).

### 6.3 Device models

Each axis carries a list of segments (a `bucket(4, 8, 16)` count). Each segment after the first starts with its
distance from the previous one, counted in new positions. A segment carries one axis model. When both axes have one
segment, y may instead be marked "same as x".

Device models are optional. This outline defines how a decoder turns a stored model into values, not how an encoder
picks one: finding the device a replay was recorded with is left to the encoder (§12). An encoder that does no device
detection can give each axis a single text segment, which fits any replay. The file is then still valid and decodes to
the same replay, only larger. Values a model does not reproduce are escaped (§6.4).

The models turn the coded integer `K` of each new position into a float:

| Mode | Value |
|---|---|
| rational | $`(a K + b) / D`$, in double, rounded to float |
| mouse | window pixels move by `counts · sensitivity` per input report, then map to the playfield |
| absolute | a tablet coordinate `L = K` in 0…65535 maps to the window, then to the playfield |
| text | `K` is the float's place in the order of printable values (§4) |

**rational**: integers `D`, `a`, `b`.

**mouse**: a sensitivity, a polling rate (125–8000 Hz) and the window.

- The step `ΔK` is split into `m` reports over the frame's `dt` ms:

  ```math
  m = \mathrm{clamp}\!\left(\mathrm{round}\!\left(\frac{dt \cdot \text{poll}}{1000}\right),\ 1,\ \min(|\Delta K|,\ 2^{16})\right)
  ```
- The pixel position is advanced in double and rounded to float after each report, and clamped to the window.
- Report splits and float rounding can leave several candidate floats. A slack index chooses one: first the
  neighbouring report counts, then values a few ulps away.
- The first position of each mouse segment is escaped, so its exact value seeds the position.

**absolute**: the tablet mapping (to the window, or to the desktop with an origin and a size), an area speed, and
whether the game computed in SSE single precision or x87 extended precision.

**The window** is either one of 24 common sizes, by index, or an explicit scale and offset.

**Window to playfield**: $`\text{field} = (\text{pixel} - \text{offset}) / \text{scale}`$, computed the way
osu!stable does. The rounding of each step, and an "unrounded subtraction" variant, are part of the model.

**text**:

- with 7-digit text, a nonzero value with 7 significant digits `m` (1 000 000 to 9 999 999) and exponent `e` is at
  the place below, signed as the value, and 0 is at `K = 0`:

  ```math
  K = \pm\big((e + 50) \cdot 9\,000\,000 + (m - 10^6) + 1\big)
  ```
- with shortest text, `K` is ± the float's bit pattern.

Rational, absolute and mouse axes **hold** after an escape: until the next move, frames keep the escaped value.

### 6.4 Escape values

An escape value is the true position where the model's value is wrong. It is coded against the model's prediction:

- **near**: the distance in 7-digit mantissas (or in ulps, with shortest text), when small;
- **otherwise**: 32 raw bits.

On an axis with a native grid, a value may instead be one of the two lattice values next to the grid value.

### 6.5 Rebuilding the frames

1. Each new position takes the next decoded value of each axis; repeated frames take the previous frame's position.
2. Deltas come from the delta table and keys are unfolded.
3. With `std_default_pos`, frames 0 and 1 are set to (256, −500).

### 6.6 Autopilot

Autopilot replays move the cursor along a path the game generates, so their positions are predicted from that path:

- points at notes;
- holds and straight moves;
- in-out cubic eased moves, sampled every `1000/60 · speed` ms;
- slider and spinner points.

**A path event** is coded when the path ends before a new position. It is the move's type, wait and duration, its
start (an offset from the last path point) and its target (an integer or an offset from a guess).

**A frame on the path** is predicted as the linear interpolation between the path points around its time. That value is
mapped field → screen → field with the stored screen size and printed as 7-digit text. Residuals are still coded.

## 7. The frames stream: osu!taiko, osu!catch and osu!mania form

**Head**: the delta table and time grid as in §6.1. The coded body starts at the next byte.

**Coded body**, in order:

1. **Escaped frames**: `gamma(count + 1) − 1`. Then, per escaped frame:
   - the gap to it, as `gamma(gap + 1) − 1`;
   - a bit for "at (256, −500)", or else its x and y as 32 raw bits each.
2. **osu!mania only**:
   - the column count, `gamma(columns)`, at most 18;
   - the scroll-speed changes: a count, then per change its frame (as a gap) and the speed as 32 raw bits.
3. **Per frame**:
   - keys, with the key coder of §6.2;
   - the delta;
   - for osu!catch, the catcher position.

| Mode | Coded keys | x | y | Escaped when |
|---|---|---|---|---|
| osu!taiko | the keys | 0, 320 or 640, from the keys | 9999 | x or y differs from that |
| osu!catch | the keys | catcher position, coded as a text value | 9999 | y ≠ 9999, or x negative or not finite |
| osu!mania | held columns (keys are always 0) | the columns as an integer | current scroll speed | the frame is at (256, −500) |

**The catcher** is coded as three decisions:

1. whether it moved;
2. whether in the same direction as last time;
3. the residual against a prediction of walking or dashing for the frame's delta, scaled by any hyperdash.

## 8. The `osr_meta_data` container

It holds the `.osr` bytes outside the replay data:

- **the header**: game mode through mods;
- **the life bar** (§9);
- **the timestamp**: 8 bytes;
- **the tail**: what follows the replay data, usually the online score id.

The container is the header, timestamp and tail in one of two forms, then the life bar to the end of the container. Both
forms end where their decoder stops reading (the fields form at the next byte boundary), so whatever bytes follow are
the life bar. When none follow, the `.osr` had no life bar.

Its first byte picks the form:

- **0, raw**: each of the three parts as a ULEB128 length and its bytes.
- **1, fields**: a bit field of:
  - mode;
  - client version, relative to 20200000;
  - player name;
  - beatmap MD5 as 16 bytes;
  - replay MD5, either as an index into 49 known formulas over the score fields or as 16 bytes;
  - hit counts, score, combo, perfect and mods;
  - the timestamp as Unix seconds, plus sub-second ticks only when present;
  - the online score id by kind (none, zero, 32-bit, 64-bit);
  - the rest of the tail.

An encoder only uses the fields form when decoding it gives back all three parts exactly and ends at the same byte.
Strings are rebuilt with the `0x0b` marker and a minimal ULEB128 length.

## 9. The life bar

The life bar follows the parts in the `osr_meta_data` container (§8). Its first byte picks the form:

- **0**: the life bar text as stored.
- **1, compact**: points of `time|value`, range coded.
  - Each time is coded as the frame it falls on (counted from a guess) or as an escaped difference.
  - Each value is coded in hundredths.
  - The decoder prints the canonical text: `0`, `1`, or `0.d` / `0.dd`.

An encoder only uses the compact form when it is smaller and decodes back to the same text. The range coder, its
frequency models and their priors are normative for this form.

## 10. Verbatim replay data

When the replay text cannot be stored as frames (§4), including replay data that decompresses to no text, the original
LZMA bytes are stored as the `osr_verbatim` container, with no `frames` container and a frame count of 0.

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
compresses the way osu! does, so replay data osu! wrote comes back byte for byte: the LZMA SDK's C# encoder with a
2 MiB dictionary, lc 3, lp 0, pb 2, 255 fast bytes, the BT4 match finder and no end mark. The SDK's C encoder makes
different choices with the same settings.

## 12. What the encoder chooses

A decoder only reads what the encoder chose. The format leaves these choices free:

- the device model of each axis, if any (text fits every replay, §6.3), its segments, the native grids and whether
  autopilot is on (mode 7);
- the order of the delta table;
- the time grid (the reference encoder picks one of codes 0–5, and does not write custom periods yet);
- whether to use the compact forms of §8 and §9.

The reference encoder refuses an `.osr` whose replay data length runs past the end of the file:
it writes no `.orz`, as the length could not be given back.

The reference encoder chooses by fitting candidates and comparing coded sizes. None of that search is part of the
format.

## 13. Known gaps in the reference implementation

Byte-exact round trips are expected. These edge cases currently break that, though no real client writes them:

- **A closing frame written as `-12345|-0|0|<seed>`** (shortest text) is taken as the seed frame and comes back as
  `-12345|0|0|<seed>`. The CRC does not catch it.
- **A life bar marker byte other than `0x00` or `0x0b`** comes back as an empty life bar (`0x0b 0x00`). A non-minimal
  ULEB128 length comes back minimal.
