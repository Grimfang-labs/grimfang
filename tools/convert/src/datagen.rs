//! Grimfang datagen shard record layout (feat/datagen, version 1).

pub const RECORD_SIZE: usize = 74;
pub const VERSION: u8 = 1;

/// Packed on-disk layout from `src/datagen.hpp` (little-endian, no alignment padding).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DatagenRecord {
    pub version: u8,
    pub piece: [u8; 64],
    pub stm: u8,
    pub castling: u8,
    pub ep_square: i8,
    pub score: i16,
    pub result: i8,
    pub reserved: [u8; 3],
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SkipReason {
    BadVersion,
    IllegalPosition,
}

impl DatagenRecord {
    pub fn from_bytes(bytes: &[u8]) -> anyhow::Result<Self> {
        anyhow::ensure!(
            bytes.len() == RECORD_SIZE,
            "record slice must be exactly {RECORD_SIZE} bytes, got {}",
            bytes.len()
        );

        let mut piece = [0u8; 64];
        piece.copy_from_slice(&bytes[1..65]);

        Ok(Self {
            version: bytes[0],
            piece,
            stm: bytes[65],
            castling: bytes[66],
            ep_square: bytes[67] as i8,
            score: i16::from_le_bytes([bytes[68], bytes[69]]),
            result: bytes[70] as i8,
            reserved: [bytes[71], bytes[72], bytes[73]],
        })
    }

    /// Belt-and-suspenders validation before conversion.
    pub fn validate(&self) -> Result<(), SkipReason> {
        if self.version != VERSION {
            return Err(SkipReason::BadVersion);
        }
        if !is_legal_position(&self.piece) {
            return Err(SkipReason::IllegalPosition);
        }
        Ok(())
    }
}

fn is_legal_position(piece: &[u8; 64]) -> bool {
    let mut white_kings = 0u8;
    let mut black_kings = 0u8;

    for &p in piece {
        match p {
            0 => {}
            1..=6 => {
                if p == 6 {
                    white_kings += 1;
                }
            }
            7..=12 => {
                if p == 12 {
                    black_kings += 1;
                }
            }
            _ => return false,
        }
    }

    white_kings == 1 && black_kings == 1
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn record_size_constant() {
        assert_eq!(RECORD_SIZE, 74);
    }
}
