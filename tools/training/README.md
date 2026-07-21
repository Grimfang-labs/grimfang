# Grimfang NNUE Training (bullet)

First net: **768 -> 512 x2 -> 1**, SCReLU, perspective, no buckets.

Superseded nets are archived outside the repo; regenerate them from the training
configs and datasets if needed.

## Prerequisites

1. **bullet** cloned as sibling: `C:\Users\shywolf91\Dev\Grimfang\bullet` (pin: `cebc78a` at setup time)
2. **Converted data**: `tools/data/train.bulletdata` (324,170,158 ChessBoard records, 32 bytes each)
3. **CUDA Toolkit** on Windows:
   - `winget install Nvidia.CUDA` (13.3 as of setup)
   - Set `CUDA_PATH` to e.g. `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3`
   - Driver 596.36 / RTX 4070 SUPER — do not train on CPU

## Workflow

```powershell
# 1. Register example in bullet
./tools/training/setup.ps1

# 2. Build bullet-utils and shuffle (REQUIRED — loader does not shuffle)
./tools/training/shuffle_data.ps1

# 3. Smoke test (1 superbatch, ~100M positions)
./tools/training/run_smoke.ps1

# 4. Full run (human launches when ready)
./tools/training/run_full.ps1   # prints exact command
```

## Training spec (grimfang_net001.rs)

| Parameter | Value |
|-----------|-------|
| Architecture | `(768 -> 512)x2 -> 1`, SCReLU |
| Batch size | 16384 |
| Superbatch | 6104 batches (~100M positions) |
| Full run | 35 superbatches (~10.8 dataset passes) |
| WDL blend | 0.3 (30% game result / 70% eval sigmoid) |
| Eval scale | 400 cp |
| LR | 0.001 until superbatch 22, then 0.0001 |
| Quantisation | QA=255, QB=64; output scale 400 |

## Output format

Checkpoints under `bullet/checkpoints/grimfang-net-001-<N>/`:

- `quantised.bin` — **use this in the engine** (i16, column-major, padded to 64-byte multiple)
- `raw.bin` — f32 weights (resume/debug)
- `optimiser_state/` — AdamW state

Quantised layout for HIDDEN=512 (from `grimfang_net001.rs`):

```
feature_weights: [Accumulator; 768]   // each Accumulator = align(64) [i16; 512], QA
feature_bias:    Accumulator          // [i16; 512], QA
output_weights:  [i16; 1024]          // QB
output_bias:     i16                  // QA * QB
```

Eval: `output * 400 / (QA * QB)` centipawns after SCReLU accumulator dot products.
