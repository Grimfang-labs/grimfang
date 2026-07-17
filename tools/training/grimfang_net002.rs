//! Grimfang net 001 — first perspective NNUE value net.
//!
//! Architecture: (768 -> 512)x2 -> 1, SCReLU, no buckets.
//! Adapted from bullet `examples/simple.rs` (commit cebc78a).
//!
//! Environment overrides (optional):
//!   GRIMFANG_DATA       path to shuffled ChessBoard data file
//!   GRIMFANG_OUTPUT_DIR checkpoint directory (default: checkpoints)
//!   GRIMFANG_END_SB      last superbatch (default: 35; use 1 for smoke)
//!   GRIMFANG_THREADS      loader threads (default: 4)
//!   GRIMFANG_BATCH_QUEUE  batch queue size (default: 64)

use bullet_lib::{
    game::inputs::Chess768,
    nn::optimiser::AdamW,
    trainer::{
        save::SavedFormat,
        schedule::{TrainingSchedule, TrainingSteps, lr, wdl},
        settings::LocalSettings,
    },
    value::{ValueTrainerBuilder, loader::DirectSequentialDataLoader},
};

const HIDDEN_SIZE: usize = 512;
const SCALE: i32 = 400;
const QA: i16 = 255;
const QB: i16 = 64;
const BATCH_SIZE: usize = 16_384;
const BATCHES_PER_SUPERBATCH: usize = 6104;
const DEFAULT_SUPERBATCHES: usize = 35;
const WDL_BLEND: f32 = 0.3;
const LR_DROP_SUPERBATCH: usize = 22; // ~63% of 35-sb run

fn env_usize(key: &str, default: usize) -> usize {
    std::env::var(key)
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(default)
}

fn env_string(key: &str, default: &str) -> String {
    std::env::var(key).unwrap_or_else(|_| default.to_string())
}

fn main() {
    let data_path = env_string(
        "GRIMFANG_DATA",
        "../grimfang/tools/data/train_shuffled.bulletdata",
    );
    let output_dir = env_string("GRIMFANG_OUTPUT_DIR", "checkpoints");
    let end_superbatch = env_usize("GRIMFANG_END_SB", DEFAULT_SUPERBATCHES);
    let threads = env_usize("GRIMFANG_THREADS", 4);
    let batch_queue = env_usize("GRIMFANG_BATCH_QUEUE", 64);

    let mut trainer = ValueTrainerBuilder::default()
        .dual_perspective()
        .optimiser(AdamW)
        .inputs(Chess768)
        .save_format(&[
            SavedFormat::id("l0w").round().quantise::<i16>(QA),
            SavedFormat::id("l0b").round().quantise::<i16>(QA),
            SavedFormat::id("l1w").round().quantise::<i16>(QB),
            SavedFormat::id("l1b").round().quantise::<i16>(QA * QB),
        ])
        .loss_fn(|output, target| output.sigmoid().squared_error(target))
        .build(|builder, stm_inputs, ntm_inputs| {
            let l0 = builder.new_affine("l0", 768, HIDDEN_SIZE);
            let l1 = builder.new_affine("l1", 2 * HIDDEN_SIZE, 1);

            let stm_hidden = l0.forward(stm_inputs).screlu();
            let ntm_hidden = l0.forward(ntm_inputs).screlu();
            let hidden_layer = stm_hidden.concat(ntm_hidden);
            l1.forward(hidden_layer)
        });

    let schedule = TrainingSchedule {
        net_id: "grimfang-net-002".to_string(),
        eval_scale: SCALE as f32,
        steps: TrainingSteps {
            batch_size: BATCH_SIZE,
            batches_per_superbatch: BATCHES_PER_SUPERBATCH,
            start_superbatch: 1,
            end_superbatch,
        },
        wdl_scheduler: wdl::ConstantWDL { value: WDL_BLEND },
        lr_scheduler: lr::DropLR {
            start: 0.001,
            gamma: 0.1,
            drop: LR_DROP_SUPERBATCH,
        },
        save_rate: 5,
    };

    let settings = LocalSettings {
        threads,
        test_set: None,
        output_directory: &output_dir,
        batch_queue_size: batch_queue,
    };

    println!("Grimfang net 001 training");
    println!("  data: {data_path}");
    println!("  output: {output_dir}");
    println!("  superbatches: 1..={end_superbatch}");
    schedule.display();

    let data_loader = DirectSequentialDataLoader::new(&[&data_path]);
    trainer.run(&schedule, &settings, &data_loader);
}

// ---------------------------------------------------------------------------
// Quantised inference layout (for engine integration — next prompt).
// Matches bullet `examples/simple.rs` with HIDDEN_SIZE = 512.
// ---------------------------------------------------------------------------

#[repr(C, align(64))]
pub struct Accumulator {
    pub vals: [i16; HIDDEN_SIZE],
}

#[repr(C)]
pub struct Network {
    /// Column-major HIDDEN_SIZE x 768, quantised QA.
    pub feature_weights: [Accumulator; 768],
    /// HIDDEN_SIZE bias vector, quantised QA.
    pub feature_bias: Accumulator,
    /// Column-major 1 x (2 * HIDDEN_SIZE), quantised QB.
    pub output_weights: [i16; 2 * HIDDEN_SIZE],
    /// Scalar bias, quantised QA * QB.
    pub output_bias: i16,
}

const _: () = assert!(std::mem::size_of::<Network>() % 64 == 0);
