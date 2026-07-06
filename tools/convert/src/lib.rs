pub mod datagen;
pub mod mapper;

use std::{
    fs::File,
    io::{BufReader, BufWriter, Read, Write},
    path::{Path, PathBuf},
};

use anyhow::{bail, Context, Result};
use bulletformat::{BulletFormat, ChessBoard};

pub use datagen::{DatagenRecord, RECORD_SIZE};
use datagen::SkipReason;
use mapper::to_chess_board;

pub const CHESSBOARD_SIZE: usize = std::mem::size_of::<ChessBoard>();
pub const PROGRESS_INTERVAL: u64 = 10_000_000;

#[derive(Debug, Default, Clone, Copy)]
pub struct SkipCounts {
    pub bad_version: u64,
    pub illegal_position: u64,
    pub conversion_error: u64,
}

impl SkipCounts {
    pub fn total(&self) -> u64 {
        self.bad_version + self.illegal_position + self.conversion_error
    }
}

#[derive(Debug, Default)]
pub struct ConvertStats {
    pub records_read: u64,
    pub records_written: u64,
    pub skips: SkipCounts,
}

pub fn collect_input_shards(input: &Path) -> Result<Vec<PathBuf>> {
    if input.is_file() {
        return Ok(vec![input.to_path_buf()]);
    }

    if !input.is_dir() {
        bail!("input path does not exist: {}", input.display());
    }

    let mut shards: Vec<PathBuf> = std::fs::read_dir(input)?
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| {
            p.is_file()
                && p.file_name()
                    .and_then(|n| n.to_str())
                    .is_some_and(|n| n.starts_with("shard_") && n.ends_with(".bin"))
        })
        .collect();

    shards.sort();
    if shards.is_empty() {
        bail!("no shard_*.bin files found in {}", input.display());
    }
    Ok(shards)
}

pub fn convert_shards(inputs: &[PathBuf], output: &Path, progress_interval: u64) -> Result<ConvertStats> {
    if let Some(parent) = output.parent() {
        if !parent.as_os_str().is_empty() {
            std::fs::create_dir_all(parent)?;
        }
    }

    let mut stats = ConvertStats::default();
    let mut writer = BufWriter::new(
        File::create(output).with_context(|| format!("create {}", output.display()))?,
    );

    let mut write_buf: Vec<ChessBoard> = Vec::with_capacity(16_384);

    for shard in inputs {
        convert_one_shard(shard, &mut writer, &mut write_buf, &mut stats, progress_interval)?;
    }

    if !write_buf.is_empty() {
        flush_boards(&mut writer, &write_buf)?;
        stats.records_written += write_buf.len() as u64;
        write_buf.clear();
    }

    writer.flush()?;
    Ok(stats)
}

fn convert_one_shard(
    path: &Path,
    writer: &mut BufWriter<File>,
    write_buf: &mut Vec<ChessBoard>,
    stats: &mut ConvertStats,
    progress_interval: u64,
) -> Result<()> {
    let file = File::open(path).with_context(|| format!("open {}", path.display()))?;
    let len = file.metadata()?.len();
    if len % RECORD_SIZE as u64 != 0 {
        bail!(
            "shard {} size {} is not a multiple of record size {}",
            path.display(),
            len,
            RECORD_SIZE
        );
    }

    let expected = len / RECORD_SIZE as u64;
    eprintln!("reading {} ({expected} records)", path.display());

    let mut reader = BufReader::with_capacity(8 * 1024 * 1024, file);
    let mut raw = [0u8; RECORD_SIZE];

    for _ in 0..expected {
        reader.read_exact(&mut raw)?;
        stats.records_read += 1;

        let record = DatagenRecord::from_bytes(&raw)?;
        if let Err(reason) = record.validate() {
            bump_skip(&mut stats.skips, reason);
            continue;
        }

        match to_chess_board(&record) {
            Ok(board) => {
                write_buf.push(board);
                if write_buf.len() >= 16_384 {
                    stats.records_written += write_buf.len() as u64;
                    flush_boards(writer, write_buf)?;
                    write_buf.clear();
                }
            }
            Err(_) => {
                stats.skips.conversion_error += 1;
            }
        }

        if progress_interval > 0 && stats.records_read % progress_interval == 0 {
            eprintln!(
                "progress: read {} written {} skipped {}",
                stats.records_read,
                stats.records_written + write_buf.len() as u64,
                stats.skips.total()
            );
        }
    }

    Ok(())
}

fn bump_skip(counts: &mut SkipCounts, reason: SkipReason) {
    match reason {
        SkipReason::BadVersion => counts.bad_version += 1,
        SkipReason::IllegalPosition => counts.illegal_position += 1,
    }
}

fn flush_boards(writer: &mut BufWriter<File>, boards: &[ChessBoard]) -> Result<()> {
    ChessBoard::write_to_bin(writer, boards).context("write ChessBoard batch")
}

/// Read `ChessBoard` records back from a bullet training file (no header).
pub fn read_chess_boards(path: &Path) -> Result<Vec<ChessBoard>> {
    let data = std::fs::read(path)?;
    if data.len() % CHESSBOARD_SIZE != 0 {
        bail!(
            "bullet data size {} is not a multiple of ChessBoard size {}",
            data.len(),
            CHESSBOARD_SIZE
        );
    }
    let mut out = Vec::with_capacity(data.len() / CHESSBOARD_SIZE);
    for chunk in data.chunks_exact(CHESSBOARD_SIZE) {
        let board: ChessBoard = unsafe { std::ptr::read_unaligned(chunk.as_ptr().cast()) };
        out.push(board);
    }
    Ok(out)
}
