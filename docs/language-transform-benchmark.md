# Language-transform benchmark results

## Setup and comparison rules

- Build: CMake `Release`, GNU C++ 13.3.0, Zstandard 1.5.5.
- Existing baseline: `encodeLosslessFrame` with Zstandard level 1, including its predictor selection and raw fallback.
- Inputs: deterministic synthetic single-component frames at 8, 10, 12, 16, 24, and 32 significant bits (10/12-bit samples stored in 16-bit words); flat, gradient, edge, low-amplitude texture, and deterministic random patterns.
- Temporal inputs are intentionally sparse-change: the current frame is the previous frame with one sample changed per 997 samples. They are a targeted predictor test, not a representative motion-video corpus.
- The candidate must beat the existing packet payload to be selected in the reported best-of size. Two bytes for sample-width and byte-order side information are charged to transformed packets. The common outer record overhead is excluded from both.
- All baseline and candidate decodes were checked against the exact source bytes. The complete project suite passed: **7/7 CTest tests**, including integration.

## Results at 1280×720

Each case ran once for elapsed-time sampling; sizes are deterministic for these inputs, but timings are less statistically robust than the 320×180 run with three repetitions.

| Candidate | Best-of selected | Median best-of size change | Median candidate encode time vs existing | Median selected decode time vs existing |
|---|---:|---:|---:|---:|
| Integer Paeth, spatial | 4/30 | 0% | 1.51× | 1.00× |
| Integer Paeth, temporal | 28/30 | −89.6% | 2.41× | 5.78× |
| Integer temporal delta | 5/30 | 0% | 0.74× | 1.00× |

Spatial integer Paeth produced useful wins in selected high-precision synthetic cases: 16-bit gradient **2,662 → 1,571 bytes (−41.0%)**; 24-bit gradient **3,121 → 1,633 (−47.7%)**; 24-bit low-amplitude texture **1,393,144 → 1,046,923 (−24.9%)**; 32-bit texture **1,458,967 → 1,193,811 (−18.2%)**. It did not win on most other spatial cases, especially 8–12-bit inputs.

For the deliberately sparse temporal sequence, integer Paeth reduced the best-of packet size by median **77.8–92.0%**, depending on bit depth, and was selected in 28 of 30 cases. Example: 16-bit random-underlying frame **2,673 → 213 bytes**; 32-bit random-underlying frame **1,793 → 391 bytes**. The decode work is appreciably higher: for the 16-bit example, candidate encode was **21.6 ms** versus **8.4 ms** baseline encode, and candidate decode was **34.9 ms**. The savings are specific to the sparse-change setup and must not be generalized to ordinary moving footage.

The simpler integer temporal-delta transform was cheaper to decode but rarely improved size over the existing codec. Its selected wins were small (up to about **1.7%** in this set), so it is a useful low-cost alternative to retain for future adaptive evaluation, not a demonstrated general compression win.

## Small-frame cross-check at 320×180

With three repetitions per case, spatial integer Paeth was selected in 3/30 cases and had a 0% median best-of size change. Temporal integer Paeth was selected in 28/30 cases (median **−65.5%**) on the same sparse-change construction; its median candidate encode ratio was **2.75×** and median selected decode ratio **8.68×**. Integer temporal delta was selected in 7/30 cases, with 0% median size change.

## Engineering decision

Keep both transforms as explicit, testable language-layer candidates, but **do not route them into VFS1–VFS5 or change the production encoder yet**. The spatial model has narrow content-dependent wins; the temporal Paeth model can save many bytes on sparse changes but currently spends substantially more compute to do so. A production promotion should first add a versioned stream signal for transform kind, component width, byte order, and temporal seed; optimize candidate gating and Paeth decode throughput; and benchmark real 8/10/12/16-bit clips with static, panning, scene-cut, and high-motion segments. No assembly was added: the current profiling evidence does not yet show that hand-written assembly is justified over further algorithm and C++ optimization.

Raw per-case output is retained as `benchmarks/results/language-transform-320x180-r3.csv` and `benchmarks/results/language-transform-1280x720-r1.csv` so the measurements can be re-aggregated without rerunning the test.
