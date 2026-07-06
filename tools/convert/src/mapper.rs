//! Map `DatagenRecord` fields into `bulletformat::ChessBoard` via the verified API.

use anyhow::{bail, Context, Result};
use bulletformat::ChessBoard;

use crate::datagen::DatagenRecord;

/// Build bullet bitboards from datagen square mailboxes (a1=0 .. h8=63, LERF).
///
/// Order required by `ChessBoard::from_raw`: White, Black, Pawn, Knight, Bishop,
/// Rook, Queen, King.
pub fn build_bitboards(piece: &[u8; 64]) -> Result<[u64; 8]> {
    let mut bbs = [0u64; 8];

    for (sq, &code) in piece.iter().enumerate() {
        if code == 0 {
            continue;
        }

        let (is_white, pt) = match code {
            1..=6 => (true, code - 1),
            7..=12 => (false, code - 7),
            _ => bail!("invalid datagen piece code {code} at square {sq}"),
        };

        let bit = 1u64 << sq;
        if is_white {
            bbs[0] |= bit;
        } else {
            bbs[1] |= bit;
        }
        bbs[2 + pt as usize] |= bit;
    }

    Ok(bbs)
}

/// Convert datagen STM-relative score/result into the white-absolute inputs that
/// `ChessBoard::from_raw` documents:
///   - score: white-relative centipawns
///   - result: 0.0 = black win, 0.5 = draw, 1.0 = white win
///
/// `from_raw` then flips both when `stm == 1` to store STM-relative values
/// internally (matching `ChessBoard::from_str`).
pub fn bullet_inputs(record: &DatagenRecord) -> Result<(i16, f32)> {
    let score_white = if record.stm == 0 {
        record.score
    } else {
        -record.score
    };

    let result = match (record.stm, record.result) {
        (0, 0) => 0.0,
        (0, 1) => 0.5,
        (0, 2) => 1.0,
        (1, 0) => 1.0,
        (1, 1) => 0.5,
        (1, 2) => 0.0,
        (stm, r) => bail!("invalid stm/result pair: stm={stm}, result={r}"),
    };

    Ok((score_white, result))
}

/// Convert one datagen record into a `ChessBoard` using `bulletformat`'s
/// public constructor (no hand-rolled layout).
pub fn to_chess_board(record: &DatagenRecord) -> Result<ChessBoard> {
    let bbs = build_bitboards(&record.piece)?;
    let (score_white, result_abs) = bullet_inputs(record)?;
    let stm = record.stm as usize;

    ChessBoard::from_raw(bbs, stm, score_white, result_abs)
        .map_err(|e| anyhow::anyhow!(e))
        .context("ChessBoard::from_raw rejected position")
}

#[cfg(test)]
mod tests {
    use super::*;
    use bulletformat::BulletFormat;

    #[test]
    fn stm_relative_round_trip_via_from_raw() {
        // White to move, white winning.
        let mut rec = DatagenRecord {
            version: 1,
            piece: [0; 64],
            stm: 0,
            castling: 0,
            ep_square: -1,
            score: 120,
            result: 2,
            reserved: [0; 3],
        };
        rec.piece[4] = 6; // white king e1
        rec.piece[60] = 12; // black king e8

        let board = to_chess_board(&rec).unwrap();
        assert_eq!(board.score(), 120);
        assert!((board.result() - 1.0).abs() < f32::EPSILON);

        // Black to move, black winning (STM-relative +cp, result=2).
        rec.stm = 1;
        rec.score = 200;
        rec.result = 2;
        let board = to_chess_board(&rec).unwrap();
        assert_eq!(board.score(), 200);
        assert!((board.result() - 1.0).abs() < f32::EPSILON);
    }
}
