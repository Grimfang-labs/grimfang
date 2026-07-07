#include "eval.hpp"

#include "nnue.hpp"
#include "position.hpp"

Value Eval::evaluate(const Position& pos) {
    return nnue::evaluate(pos);
}
