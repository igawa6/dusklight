/**
 * JUTFader.cpp
 * JUtility - Color Fader
 */

#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/JUtility/JUTFader.h"
#include "JSystem/J2DGraph/J2DOrthoGraph.h"

#if TARGET_PC
#include "dusk/game_clock.h"

#include <algorithm>
#endif

JUTFader::JUTFader(int x, int y, int width, int height, JUtility::TColor pColor)
    : mColor(pColor), mBox(x, y, x + width, y + height) {
    mStatus = None;
    mDuration = 0;
    mTimer = 0;
    mNextStatus = 0;
    mStatusTimer = -1;
}

void JUTFader::advance() {
    if (0 <= mStatusTimer && mStatusTimer-- == 0) {
        mStatus = mNextStatus;
    }

    if (mStatus == Wait) {
        return;
    }

    switch (mStatus) {
    case None:
        mColor.a = 0xFF;
        break;
    case FadeIn:
#if AVOID_UB
        if (mDuration == 0) {
            IF_DUSK(mColor.a = 0);
            mStatus = Wait;
            break;
        }
#endif
        mColor.a = 0xFF - ((++mTimer * 0xFF) / mDuration);

        if (mTimer >= mDuration) {
            mStatus = Wait;
        }

        break;
    case FadeOut:
#if AVOID_UB
        if (mDuration == 0) {
            IF_DUSK(mColor.a = 0xFF);
            mStatus = None;
            break;
        }
#endif
        mColor.a = ((++mTimer * 0xFF) / mDuration);

        if (mTimer >= mDuration) {
            mStatus = None;
        }

        break;
    }
}

void JUTFader::control() {
    advance();
    IF_DUSK_BLOCK(getStatus() != Wait)
    draw();
    IF_DUSK_BLOCK_END
}

void JUTFader::draw() {
#if TARGET_PC
    JUtility::TColor color = mColor;
    if (dusk::game_clock::g_frameTiming.separatePresentation && mDuration != 0 &&
        (mStatus == FadeIn || mStatus == FadeOut))
    {
        const f32 timer = std::min(mTimer + dusk::game_clock::sample_interpolation_step(),
                                   static_cast<f32>(mDuration));
        const u8 alpha = static_cast<u8>((timer * 0xFF) / mDuration);
        color.a = mStatus == FadeIn ? 0xFF - alpha : alpha;
    }
#endif
    if (DUSK_IF_ELSE(color, mColor).a != 0) {
        J2DOrthoGraph orthograph;
        orthograph.setColor(DUSK_IF_ELSE(color, mColor));
        orthograph.fillBox(mBox);
    }
}

bool JUTFader::startFadeIn(int duration) {
    bool statusCheck = mStatus == 0;

    if (statusCheck) {
        mStatus = FadeIn;
        mTimer = 0;
        mDuration = duration;
    }

    return statusCheck;
}

bool JUTFader::startFadeOut(int duration) {
    bool statusCheck = mStatus == 1;

    if (statusCheck) {
        mStatus = FadeOut;
        mTimer = 0;
        mDuration = duration;
    }

    return statusCheck;
}

void JUTFader::setStatus(JUTFader::EStatus i_status, int timer) {
    switch (i_status) {
    case None: 
        if (timer != 0) {
            mNextStatus = None;
            mStatusTimer = timer;
            break;
        }

        mStatus = None;
        mNextStatus = None;
        mStatusTimer = 0;
        break;
    case Wait:
        if (timer != 0) {
            mNextStatus = Wait;
            mStatusTimer = timer;
            break;
        }

        mStatus = Wait;
        mNextStatus = Wait;
        mStatusTimer = 0;
        break;
    }
}

JUTFader::~JUTFader() {}
