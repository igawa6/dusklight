#pragma once

#include "dusk/game_clock.h"

#include "JSystem/J2DGraph/J2DAnimation.h"
#include "JSystem/J2DGraph/J2DPane.h"
#include "SSystem/SComponent/c_lib.h"
#include "SSystem/SComponent/c_math.h"

#include <dolphin/types.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace dusk::vdt {

class FrameAnimation {
public:
    void start(f32 from, f32 target, f32 speed = 1.0f, f32 period = 0.0f) {
        mFrame = from;
        mTarget = target;
        mSpeed = speed;
        mPeriod = period;
        mTime = game_clock::sample_time();
        mEpoch = game_clock::g_frameTiming.presentationEpoch;
    }

    void approach(f32 from, f32 target, f32 speed = 1.0f) {
        if (mTarget != target || std::abs(mSpeed - speed) > 0.000001f || mPeriod != 0.0f ||
            mEpoch != game_clock::g_frameTiming.presentationEpoch)
        {
            start(from, target, speed);
        }
    }

    void finish(f32 frame) { start(frame, frame, 0.0f); }

    f32 advance(f32 logicalFrame) {
        const f32 frames = game_clock::consume_interval(mTime) / game_clock::kSimPeriod;
        if (!game_clock::g_frameTiming.interpolating ||
            mEpoch != game_clock::g_frameTiming.presentationEpoch)
        {
            mFrame = logicalFrame;
            mEpoch = game_clock::g_frameTiming.presentationEpoch;
        } else if (mPeriod > 0.0f) {
            mFrame = std::fmod(mFrame + mSpeed * frames, mPeriod);
        } else {
            const f32 step = mSpeed * frames;
            mFrame = mFrame < mTarget ? std::min(mFrame + step, mTarget)
                                      : std::max(mFrame - step, mTarget);
        }
        return mFrame;
    }

    f32 value() const { return mFrame; }

private:
    f32 mFrame = 0.0f;
    f32 mTarget = 0.0f;
    f32 mSpeed = 0.0f;
    f32 mPeriod = 0.0f;
    double mTime = 0.0;
    u64 mEpoch = 0;
};

inline f32 clamped_fraction(f32 timer, f32 duration) {
    if (duration <= 0.0f) {
        return 1.0f;
    }
    return std::clamp(timer / duration, 0.0f, 1.0f);
}

inline void advance_looping_frame(f32& frame, f32 speed, f32 max) {
    if (max <= 0.0f) {
        return;
    }
    frame += speed * game_clock::original_frames();
    if (frame >= max) {
        frame = fmodf(frame, max);
    }
}

inline void advance_toward_frame(f32& frame, f32 target, f32 speed) {
    if (frame == target) {
        return;
    }
    const f32 step = speed * game_clock::original_frames();
    frame = frame < target ? std::min(frame + step, target) : std::max(frame - step, target);
}

template <typename Animation>
bool request_animation(Animation& animation, f32& frame, s16 time,
                       decltype(animation.start) from, decltype(animation.end) to, u8 curve) {
    if (frame < time) {
        animation = {true, time, curve, from, to};
        return false;
    }
    animation.active = false;
    frame = time;
    return true;
}

template <typename Animation, typename Pane, typename Apply>
void present_animation(Animation& animation, f32& frame, Pane& pane, Apply apply) {
    if (!animation.active) {
        return;
    }
    advance_toward_frame(frame, animation.duration, 1.0f);
    apply(pane.rateCalc(animation.duration, frame, animation.curve));
    if (frame >= animation.duration) {
        animation.active = false;
    }
}

inline void present_looping(f32& frame, J2DAnmBase* anm, f32 speed) {
    if (anm == nullptr) {
        return;
    }
    advance_looping_frame(frame, speed, anm->getFrameMax());
    anm->setFrame(frame);
}

inline void present_toward(f32& frame, f32 target, J2DAnmTransform* anm, J2DPane* pane = nullptr) {
    if (anm == nullptr) {
        return;
    }
    if (pane != nullptr && pane->mTransform != anm) {
        return;
    }
    advance_toward_frame(frame, target, 2.0f);
    anm->setFrame(frame);
    if (pane != nullptr) {
        pane->animationTransform();
    }
}

inline void present_selected(f32& frame, J2DPane* pane, J2DAnmTransform* first, f32 firstTarget,
                             J2DAnmTransform* second, f32 secondTarget)
{
    if (pane != nullptr) {
        if (pane->mTransform == first) {
            present_toward(frame, firstTarget, first, pane);
        } else if (pane->mTransform == second) {
            present_toward(frame, secondTarget, second, pane);
        }
    }
}

inline void present_shared(f32& frame, f32 target, J2DAnmTransform* anm,
                           std::initializer_list<J2DPane*> panes)
{
    if (frame == target) {
        return;
    }
    bool advanced = false;
    for (J2DPane* pane : panes) {
        if (pane != nullptr && pane->mTransform == anm) {
            if (!advanced) {
                present_toward(frame, target, anm);
                advanced = true;
            }
            pane->animationTransform();
        }
    }
}

inline void present_addCalc(f32* value, f32 target, f32 scale, f32 maxStep, f32 minStep) {
    const f32 frames = game_clock::original_frames();
    if (*value == target || frames <= 0.0f) {
        return;
    }
    const f32 timedScale = frames == 1.0f ? scale : 1.0f - std::pow(1.0f - scale, frames);
    cLib_addCalc(value, target, timedScale, maxStep * frames, minStep * frames);
}

inline void present_addCalc2(f32* value, f32 target, f32 scale, f32 maxStep, f32 snap) {
    const f32 frames = game_clock::original_frames();
    if (*value == target || frames <= 0.0f) {
        return;
    }
    const f32 timedScale = frames == 1.0f ? scale : 1.0f - std::pow(1.0f - scale, frames);
    cLib_addCalc2(value, target, timedScale, maxStep * frames);
    if (fabsf(*value - target) < snap) {
        *value = target;
    }
}

inline f32 present_sine_ease(f32 max, f32 value) {
    if (max <= 0.0f) {
        return 1.0f;
    }
    value = std::clamp(value, 0.0f, max);
    f32 rate = value / max;
    rate = cM_ssin((int)(0.5f * (32768.0f * rate)));
    return rate * rate;
}

inline bool present_chase(f32* value, f32 target, f32 scale, f32 maxStep, f32 snap) {
    if (*value == target) {
        return false;
    }
    present_addCalc2(value, target, scale, maxStep, snap);
    return true;
}

}  // namespace dusk::vdt
