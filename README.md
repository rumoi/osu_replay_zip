# ORZ format

.orz is a lossless compression format to store osu replay frames. [SPEC_OUTLINE.md](SPEC_OUTLINE.md) outlines the format.

**The format is specified, but V0 is not pinned quite yet.**\
**The priors are still being trained, once they are finished the format is ready.**

Choose the compression priority with the two-bit field in `orz_state::options`:

| Value | Encoder option | Frame prediction |
|---|---|---|
| 0 (default) | `ENCODE_COMPRESSION_PRIORITY_SIZE` | All contexts and mixer; no adaptive probability refinement |
| 1 | `ENCODE_COMPRESSION_PRIORITY_BALANCED` | First context when confident, otherwise fallback; direct probability |
| 2 | `ENCODE_COMPRESSION_PRIORITY_SPEED` | Fallback counter only; direct probability |

```cpp
orz::orz_state encoder;
encoder.options = (encoder.options & ~orz::ENCODE_COMPRESSION_PRIORITY_MASK)
    | orz::ENCODE_COMPRESSION_PRIORITY_BALANCED;
orz::osr_to_orz(input, packed, encoder);
```

`replay_data_to_orz` accepts the same options. Value 3 is reserved: encoding returns false with an empty output.
These are alternative field values, so clear the mask before changing priority. The encoder writes the priority
into bits 8–9 of the `.orz` decode flags (`encode_info`); `orz_decode_frames` and `orz_to_osr` select it automatically.
All three priorities use the current unreleased V0 format. Verbatim fallbacks clear the priority field.
Compatibility with earlier experimental builds is not supported.

SIZE and BALANCED share one counter-prior bank; BALANCED adds no prior data. SIZE also uses the existing mixer
weights. SPEED uses a separate 5,420-byte array payload. There is no APM decoder or APM prior bank.
The offline generator's `--balanced` mode produces a candidate counter bank for experiments; the current
BALANCED format continues to use the shared bank.

The prior banks are part of the format. Encoder and decoder must use identical prior values.

To skip cursor device-model detection during encoding, also set `ENCODE_NO_DEVICE`:

```cpp
encoder.options |= orz::ENCODE_NO_DEVICE;
```

This skips device hints and device-model search; cursor positions can use the text representation instead.
Autopilot detection still runs. Skipping device search does not guarantee faster encoding: the chosen position
representation also affects encoding cost. No decode flag is needed because the file already describes that representation.

For repeated frame decoding, retain the table storage across calls to avoid repeated allocations:

```cpp
orz::_table_memory tables;
orz::_decoded_orz decoded;
for (const auto& packed : replays)
    orz::orz_decode_frames(packed, decoded, tables);
```

The decoder resets the adaptive model state for each replay. Both examples omit error handling.

## Dataset compression

[Replay dataset](https://drive.google.com/file/d/1i2oe929loEzhvCfRYG-DeZZQ_YouaBYD/view): 6,555 replays.

Measured on Windows on 2026-11-12 using the default **SIZE** priority, built-in priors and device detection.
Inputs are the bare LZMA replay payloads; output sizes include the complete `.orz` envelope. All sizes below
are totals in bytes, and percentages are output bytes divided by input bytes. The dataset overlaps prior training.

| Mode | Replays | Input bytes | ORZ bytes (SIZE) | % of original |
|---|---:|---:|---:|---:|
| osu! | 4,163 | 226,602,499 | 61,893,067 | 27.313% |
| taiko | 457 | 2,794,463 | 1,258,302 | 45.028% |
| mania | 1,562 | 14,573,760 | 6,744,120 | 46.276% |
| catch | 373 | 10,419,674 | 4,665,215 | 44.773% |
| **Total** | **6,555** | **254,390,396** | **74,560,704** | **29.310%** |

All 6,555 outputs passed replay-text equality and default CRC verification.
