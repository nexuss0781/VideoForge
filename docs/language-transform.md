# Reversible integer-sample language transform, version 1

## Scope

This module is confined to the frame-byte language/transform layer. It adds a mathematical residual transform to the existing codec library, but does not change `encodeLosslessFrame`, VFS records, demux/mux, archives, audio, or UI. Existing VFS1–VFS5 packets and prediction identifiers are unchanged. The new representation is not written into a persisted stream yet: a future wire-format change must explicitly advertise the transform version, sample width, byte order, and temporal-seed requirement before any stream writer selects it. Do not label these residuals as an existing Paeth mode.

## Representation

A plane is traversed in row-major order. Within each pixel, its packed components are traversed in byte-order-sized groups of `w` bytes, where `1 <= w <= 4`. The bytes in a component are interpreted as an unsigned integer `X_i` in the explicitly selected little- or big-endian order. The storage domain is

\[
\mathbb{Z}_M, \qquad M = 2^{8w}.
\]

The significant bit depth may be lower than the storage width (for example, 10- or 12-bit samples stored in 16 bits). This transform does not infer or validate a pixel format or significant bit depth.

For a key frame, let `D_i = X_i`. With a supplied previous decoded frame `P`, let

\[
D_i = (X_i - P_i) \bmod M.
\]

For each sample, `a`, `b`, and `c` are respectively the already available left, above, and upper-left `D` values for the same component lane. A missing neighbor is zero. Define `q = a + b - c`; choose from `(a, b, c)` whichever minimizes absolute distance to `q`, breaking ties in the order left, above, upper-left. The stored residual is

\[
R_i = (D_i - \operatorname{Paeth}(a,b,c)) \bmod M.
\]

The inverse walks the same traversal order, computes

\[
D_i = (R_i + \operatorname{Paeth}(a,b,c)) \bmod M,
\]

then emits `X_i = D_i` for a key frame or `X_i = (D_i + P_i) mod M` for a temporal frame. The implementation supports up to 32-bit samples so predictor arithmetic is safely represented in signed 64-bit integers. Plane/pixel/component alignment is validated before processing.

This is bijective for every accepted byte string and matching layout/parameters/seed; the proof is induction in traversal order because each decoded residual has a unique modular inverse once its earlier neighbors are known. The residual buffer has the **same length** as the input. The transform itself does not compress data or guarantee fewer bytes.

The module also exposes a distinct, simpler **integer temporal-delta** transform for a required previous frame:

\[
R_i = (X_i - P_i) \bmod M, \qquad X_i = (R_i + P_i) \bmod M.
\]

It has no spatial dependencies; every component can be decoded independently. It is intentionally benchmarked separately from temporal Paeth, not silently substituted for it.

## Compression and fallback

The benchmark applies Zstandard level 1 to the residuals and selects the transformed candidate only when `compressed residual bytes + 2 bytes of modeled sample-width/byte-order side information < raw bytes`; otherwise that candidate falls back to raw. It reports the candidate separately from an adaptive best-of comparison, which selects the smaller of the existing packet and the new candidate. The existing codec is benchmarked through `encodeLosslessFrame` with Zstandard level 1, so its current raw fallback and adaptive byte/temporal predictor selection are included. The common outer frame-record overhead is omitted from both totals because it is equal; the new representation's two extra parameter bytes are included when selected. Best-of encode time includes both encoders because the candidate must be evaluated; a real implementation should gate or optimize this work before production use.

Lossless compression cannot shrink every possible input. For arbitrary byte strings, a reversible code must sometimes emit at least as many bits as the input; the raw fallback is essential. No compression ratio is promised for random, already-compressed, or high-entropy content.

## Build, test, and benchmark

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target frameforge_language_transform_test frameforge_language_transform_benchmark -j
ctest --test-dir build -R language_transform --output-on-failure
build/frameforge_language_transform_benchmark --width 320 --height 180 --repeats 3
```

The benchmark covers 8-, 10-, 12-, 16-, 24-, and 32-bit unsigned samples; flat, gradient, edge, low-amplitude texture, and deterministic random frames; key-frame spatial prediction and temporal prediction from an identical shared seed. It compares integer Paeth on both frame types and integer temporal delta on temporal frames. It checks exact decoded bytes for each candidate before reporting candidate/best-of sizes and median encode/decode times. Results are empirical for the build machine and must not be generalized beyond these cases.
