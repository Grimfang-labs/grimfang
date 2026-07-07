use std::{
    fs,
    mem::MaybeUninit,
    mem::{align_of, size_of},
};

const HIDDEN_SIZE: usize = 512;
const QA: i32 = 255;
const QB: i32 = 64;
const SCALE: i32 = 400;
const NETWORK_BYTES: usize = 789_568;

#[repr(C, align(64))]
#[derive(Clone, Copy)]
struct Accumulator {
    vals: [i16; HIDDEN_SIZE],
}

#[repr(C)]
struct Network {
    feature_weights: [Accumulator; 768],
    feature_bias: Accumulator,
    output_weights: [i16; 2 * HIDDEN_SIZE],
    output_bias: i16,
}

#[derive(Clone, Copy)]
struct PieceOnSquare {
    color: usize,
    piece_type: usize,
    square: usize,
}

struct Position {
    pieces: Vec<PieceOnSquare>,
    stm: usize,
}

const FENS: [&str; 10] = [
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "8/8/8/4k3/8/3K4/4P3/8 b - - 0 1",
    "rnbqkbnr/pp1ppppp/8/2pP4/8/8/PPP1PPPP/RNBQKBNR w KQkq c6 0 3",
    "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
    "4k3/8/8/8/8/8/4Q3/4K3 w - - 0 1",
    "4k3/8/8/8/8/8/4Q3/4K3 b - - 0 1",
    "4k3/1P6/8/8/8/8/8/4K3 w - - 0 1",
];

fn feature_index(perspective: usize, piece: PieceOnSquare) -> usize {
    let color_base = if piece.color == perspective { 0 } else { 384 };
    let square = if perspective == 0 {
        piece.square
    } else {
        piece.square ^ 56
    };
    color_base + piece.piece_type * 64 + square
}

fn parse_fen(fen: &str) -> Position {
    let mut parts = fen.split_whitespace();
    let placement = parts.next().expect("placement");
    let stm = match parts.next().expect("side to move") {
        "w" => 0,
        "b" => 1,
        other => panic!("bad stm: {other}"),
    };

    let mut pieces = Vec::new();
    let mut rank = 7usize;
    let mut file = 0usize;
    for ch in placement.chars() {
        match ch {
            '/' => {
                rank -= 1;
                file = 0;
            }
            '1'..='8' => file += (ch as u8 - b'0') as usize,
            _ => {
                let color = if ch.is_ascii_uppercase() { 0 } else { 1 };
                let piece_type = match ch.to_ascii_lowercase() {
                    'p' => 0,
                    'n' => 1,
                    'b' => 2,
                    'r' => 3,
                    'q' => 4,
                    'k' => 5,
                    other => panic!("bad piece: {other}"),
                };
                let square = rank * 8 + file;
                pieces.push(PieceOnSquare {
                    color,
                    piece_type,
                    square,
                });
                file += 1;
            }
        }
    }

    Position { pieces, stm }
}

fn evaluate(network: &Network, pos: &Position) -> i32 {
    let mut acc = [[0i16; HIDDEN_SIZE]; 2];
    for perspective in 0..2 {
        acc[perspective].copy_from_slice(&network.feature_bias.vals);
    }

    for &piece in &pos.pieces {
        for perspective in 0..2 {
            let idx = feature_index(perspective, piece);
            for i in 0..HIDDEN_SIZE {
                acc[perspective][i] += network.feature_weights[idx].vals[i];
            }
        }
    }

    let mut sum = 0i32;
    let us = &acc[pos.stm];
    let them = &acc[pos.stm ^ 1];
    for i in 0..HIDDEN_SIZE {
        let a = us[i].clamp(0, QA as i16) as i32;
        let b = them[i].clamp(0, QA as i16) as i32;
        sum += a * a * network.output_weights[i] as i32;
        sum += b * b * network.output_weights[HIDDEN_SIZE + i] as i32;
    }

    (sum / QA + network.output_bias as i32) * SCALE / (QA * QB)
}

fn main() {
    assert_eq!(size_of::<Accumulator>(), 1024);
    assert_eq!(align_of::<Accumulator>(), 64);
    assert_eq!(size_of::<Network>(), NETWORK_BYTES);

    let bytes = fs::read("../stockwolf-net-001.bin").expect("read ../stockwolf-net-001.bin");
    assert_eq!(bytes.len(), NETWORK_BYTES, "net byte size mismatch");

    let mut network = Box::<MaybeUninit<Network>>::new(MaybeUninit::uninit());
    unsafe {
        std::ptr::copy_nonoverlapping(
            bytes.as_ptr(),
            network.as_mut_ptr() as *mut u8,
            NETWORK_BYTES,
        );
    }
    let network = unsafe { network.assume_init() };

    for fen in FENS {
        let pos = parse_fen(fen);
        let eval = evaluate(&network, &pos);
        println!("{fen} -> {eval}");
    }
}
