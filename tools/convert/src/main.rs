use std::path::PathBuf;

use anyhow::Context;
use clap::Parser;

use convert::{collect_input_shards, convert_shards, ConvertStats, PROGRESS_INTERVAL};

#[derive(Parser, Debug)]
#[command(
    name = "convert",
    about = "Convert Grimfang datagen shards to bullet ChessBoard training data"
)]
struct Args {
    /// Input directory containing shard_*.bin files, or a single shard file.
    #[arg(long = "in")]
    input: PathBuf,

    /// Output bullet training file (raw ChessBoard sequence, 32 bytes/position).
    #[arg(long)]
    out: PathBuf,

    /// Print progress every N positions read (0 disables).
    #[arg(long, default_value_t = PROGRESS_INTERVAL)]
    progress_every: u64,
}

fn main() -> anyhow::Result<()> {
    let args = Args::parse();
    let shards = collect_input_shards(&args.input)?;
    eprintln!(
        "converting {} shard(s) -> {}",
        shards.len(),
        args.out.display()
    );

    let stats = convert_shards(&shards, &args.out, args.progress_every)?;
    print_summary(&stats, &args.out)?;
    Ok(())
}

fn print_summary(stats: &ConvertStats, output: &std::path::Path) -> anyhow::Result<()> {
    let out_size = std::fs::metadata(output)
        .with_context(|| format!("stat {}", output.display()))?
        .len();

    println!("convert complete");
    println!("  output: {}", output.display());
    println!("  output bytes: {out_size}");
    println!("  records read: {}", stats.records_read);
    println!("  records written: {}", stats.records_written);
    println!("  skipped bad version: {}", stats.skips.bad_version);
    println!("  skipped illegal position: {}", stats.skips.illegal_position);
    println!("  skipped conversion error: {}", stats.skips.conversion_error);
    println!(
        "  accounting: read = written + skipped ({})",
        stats.records_written + stats.skips.total()
    );

    if stats.records_read != stats.records_written + stats.skips.total() {
        anyhow::bail!("record accounting mismatch");
    }

    Ok(())
}
