//! Integration tests for datagen -> bullet conversion.

use std::fs::File;
use std::io::Read;
use std::path::PathBuf;

use bulletformat::{BulletFormat, ChessBoard, DataLoader};
use convert::{
    collect_input_shards, convert_shards, datagen::DatagenRecord, mapper::to_chess_board,
    read_chess_boards, CHESSBOARD_SIZE, RECORD_SIZE,
};

fn repo_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("..")
}

fn shard0_path() -> PathBuf {
    repo_root().join("tools").join("data").join("shard_0.bin")
}

fn read_sample_records(path: &PathBuf, count: usize) -> Vec<DatagenRecord> {
    let mut file = File::open(path).expect("open shard_0.bin");
    let mut out = Vec::with_capacity(count);
    let mut buf = [0u8; RECORD_SIZE];
    for _ in 0..count {
        file.read_exact(&mut buf).expect("read record");
        out.push(DatagenRecord::from_bytes(&buf).expect("parse record"));
    }
    out
}

#[test]
fn chessboard_size_matches_bulletformat() {
    assert_eq!(CHESSBOARD_SIZE, 32);
}

#[test]
fn round_trip_sample_records_from_shard() {
    let shard = shard0_path();
    if !shard.exists() {
        eprintln!("skip round_trip_sample_records_from_shard: {}", shard.display());
        return;
    }

    let records = read_sample_records(&shard, 10);
    for (i, record) in records.iter().enumerate() {
        record.validate().expect("sample should be valid");
        let board = to_chess_board(record).expect("convert sample");

        // Serialize and read back exactly as bullet's loader would.
        let bytes = bullet_bytes(&[board]);
        assert_eq!(bytes.len(), CHESSBOARD_SIZE);
        let loaded: ChessBoard =
            unsafe { std::ptr::read_unaligned(bytes.as_ptr().cast()) };

        assert_eq!(loaded.occ(), board.occ(), "record {i} occ mismatch");
        assert_eq!(loaded.score(), board.score(), "record {i} score mismatch");
        assert_eq!(loaded.result(), board.result(), "record {i} result mismatch");
        assert_eq!(loaded.our_ksq(), board.our_ksq(), "record {i} ksq mismatch");
        assert_eq!(loaded.opp_ksq(), board.opp_ksq(), "record {i} opp_ksq mismatch");
    }
}

#[test]
fn determinism_on_first_shard_prefix() {
    let shard = shard0_path();
    if !shard.exists() {
        eprintln!("skip determinism_on_first_shard_prefix");
        return;
    }

    let dir = tempfile::tempdir().unwrap();
    let out_a = dir.path().join("a.bulletdata");
    let out_b = dir.path().join("b.bulletdata");

    // Convert only the first 4096 records by copying a prefix file.
    let prefix = dir.path().join("prefix.bin");
    {
        let mut src = File::open(&shard).unwrap();
        let mut buf = vec![0u8; 4096 * RECORD_SIZE];
        src.read_exact(&mut buf).unwrap();
        std::fs::write(&prefix, buf).unwrap();
    }

    convert_shards(&[prefix.clone()], &out_a, 0).unwrap();
    convert_shards(&[prefix], &out_b, 0).unwrap();

    let a = std::fs::read(&out_a).unwrap();
    let b = std::fs::read(&out_b).unwrap();
    assert_eq!(a, b);
}

#[test]
fn bullet_dataloader_accepts_converted_prefix() {
    let shard = shard0_path();
    if !shard.exists() {
        eprintln!("skip bullet_dataloader_accepts_converted_prefix");
        return;
    }

    let dir = tempfile::tempdir().unwrap();
    let prefix = dir.path().join("prefix.bin");
    let out = dir.path().join("train.bulletdata");

    {
        let mut src = File::open(&shard).unwrap();
        let mut buf = vec![0u8; 1024 * RECORD_SIZE];
        src.read_exact(&mut buf).unwrap();
        std::fs::write(&prefix, buf).unwrap();
    }

    convert_shards(&[prefix], &out, 0).unwrap();

    let loader = DataLoader::<ChessBoard>::new(&out, 1).expect("DataLoader::new");
    assert_eq!(loader.len(), 1024);

    let mut seen = 0usize;
    loader.map_positions(|pos| {
        let _ = pos.score();
        let _ = pos.result();
        seen += 1;
    });
    assert_eq!(seen, 1024);

    let boards = read_chess_boards(&out).unwrap();
    assert_eq!(boards.len(), 1024);
}

fn bullet_bytes(boards: &[ChessBoard]) -> Vec<u8> {
    let mut out = Vec::with_capacity(boards.len() * CHESSBOARD_SIZE);
    for board in boards {
        let bytes = std::slice::from_ref(board);
        let raw: &[u8] = ChessBoard::as_bytes_slice(bytes);
        out.extend_from_slice(raw);
    }
    out
}

#[test]
fn collect_shards_from_tools_data() {
    let data_dir = repo_root().join("tools").join("data");
    if !data_dir.exists() {
        eprintln!("skip collect_shards_from_tools_data");
        return;
    }
    let shards = collect_input_shards(&data_dir).unwrap();
    assert!(shards.len() >= 1);
    assert!(shards[0].file_name().unwrap().to_str().unwrap().starts_with("shard_"));
}
