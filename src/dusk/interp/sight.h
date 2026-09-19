#pragma once

#include "dusk/interp/samples.h"

namespace dusk::interp {

struct SightAnimation {
    f32 bck, brk, alpha, brkEnd;
};

inline void lerp(SightAnimation& out, const SightAnimation& a, const SightAnimation& b, f32 step) {
    out.bck = lerp(a.bck, b.bck < a.bck ? b.bck + 29.0f : b.bck, step);
    if (out.bck >= 50.0f) {
        out.bck -= 29.0f;
    }
    out.brk = lerp(a.brk, b.brk < a.brk ? b.brk + b.brkEnd : b.brk, step);
    if (out.brk >= b.brkEnd) {
        out.brk -= b.brkEnd;
    }
    out.alpha = lerp(a.alpha, b.alpha, step);
}

}  // namespace dusk::interp
