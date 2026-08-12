# Grimfang test results

Standard conditions unless noted: `nodes=1000000`/move, `Hash=64`,
`8moves_v3.epd`, 1 thread.

**SPRT results are DECISIONS, not measurements.** SPRT stops when evidence
looks favourable, so the stopping rule correlates with upward noise and the
point estimate is inflated. Never sum them; measure with a fixed-length match.
Demonstrated 2026-07-29: v01 (+33.06) + v10 (+20.35) = +53.4, but the
fixed-length 10,000-game measurement of the same change gave +17.04 (~3x).

| date | tag | kind | change | Elo | games | verdict |
|---|---|---|---|---|---|---|
| 2026-08-12 | v08_lmrmin2 | SPRT | dev:[LmrMinDepth=2] vs base:[defaults] | -0.00 +/- 0.00 | 6000 | no verdict (rounds exhausted) |
