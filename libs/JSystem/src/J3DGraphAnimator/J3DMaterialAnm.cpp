#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/J3DGraphAnimator/J3DMaterialAnm.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"

#if TARGET_PC
#include "dusk/interp/material.h"
#endif

void J3DMaterialAnm::initialize() {
    for (int i = 0; i < ARRAY_SIZE(mMatColorAnm); i++) {
        mMatColorAnm[i].setAnmFlag(false);
    }

    for (int i = 0; i < ARRAY_SIZE(mTexNoAnm); i++) {
        mTexNoAnm[i].setAnmFlag(false);
    }

    for (int i = 0; i < ARRAY_SIZE(mTevColorAnm); i++) {
        mTevColorAnm[i].setAnmFlag(false);
    }

    for (int i = 0; i < ARRAY_SIZE(mTevKColorAnm); i++) {
        mTevKColorAnm[i].setAnmFlag(false);
    }

    for (int i = 0; i < ARRAY_SIZE(mTexMtxAnm); i++) {
        mTexMtxAnm[i].setAnmFlag(false);
    }
}

void J3DMaterialAnm::calc(J3DMaterial* pMaterial) const {
    J3D_ASSERT_NULLPTR(54, pMaterial != NULL);

    for (u32 i = 0; i < ARRAY_SIZE(mMatColorAnm); i++) {
        if (mMatColorAnm[i].getAnmFlag()) {
            GXColor* color = pMaterial->mColorBlock->getMatColor(i);
            mMatColorAnm[i].calc(color);
        }
    }

    u16 tmp;
    for (u32 i = 0; i < ARRAY_SIZE(mTexNoAnm); i++) {
        if (mTexNoAnm[i].getAnmFlag()) {
            mTexNoAnm[i].calc(&tmp);
            pMaterial->mTevBlock->setTexNo(i, tmp);
        }
    }

    for (u32 i = 0; i < 3; i++) {
        if (mTevColorAnm[i].getAnmFlag()) {
            GXColorS10* color = pMaterial->mTevBlock->getTevColor(i);
            mTevColorAnm[i].calc(color);
        }
    }

    for (u32 i = 0; i < ARRAY_SIZE(mTevKColorAnm); i++) {
        if (mTevKColorAnm[i].getAnmFlag()) {
            GXColor* color = pMaterial->mTevBlock->getTevKColor(i);
            mTevKColorAnm[i].calc(color);
        }
    }

    for (u32 i = 0; i < ARRAY_SIZE(mTexMtxAnm); i++) {
        if (mTexMtxAnm[i].getAnmFlag()) {
            J3DTextureSRTInfo* pSRT = &pMaterial->mTexGenBlock->getTexMtx(i)->getTexMtxInfo().mSRT;
            mTexMtxAnm[i].calc(pSRT);
        }
    }
}

void J3DMaterialAnm::setMatColorAnm(int idx, J3DMatColorAnm* pMatColorAnm) {
    J3D_ASSERT_RANGE(106, idx >= 0 && idx < 2);

    if (pMatColorAnm == NULL) {
        mMatColorAnm[idx].setAnmFlag(false);
    } else {
        mMatColorAnm[idx] = *pMatColorAnm;
    }
}

void J3DMaterialAnm::setTexMtxAnm(int idx, J3DTexMtxAnm* pTexMtxAnm) {
    J3D_ASSERT_RANGE(117, idx >= 0 && idx < 8);

    if (pTexMtxAnm == NULL) {
        mTexMtxAnm[idx].setAnmFlag(false);
    } else {
        mTexMtxAnm[idx] = *pTexMtxAnm;
    }
}

void J3DMaterialAnm::setTexNoAnm(int idx, J3DTexNoAnm* pTexNoAnm) {
    J3D_ASSERT_RANGE(128, idx >= 0 && idx < 8);

    if (pTexNoAnm == NULL) {
        mTexNoAnm[idx].setAnmFlag(false);
    } else {
        mTexNoAnm[idx] = *pTexNoAnm;
    }
}

void J3DMaterialAnm::setTevColorAnm(int idx, J3DTevColorAnm* pTevColorAnm) {
    J3D_ASSERT_RANGE(139, idx >= 0 && idx < 4);

    if (pTevColorAnm == NULL) {
        mTevColorAnm[idx].setAnmFlag(false);
    } else {
        mTevColorAnm[idx] = *pTevColorAnm;
    }
}

void J3DMaterialAnm::setTevKColorAnm(int idx, J3DTevKColorAnm* pTevKColorAnm) {
    J3D_ASSERT_RANGE(150, idx >= 0 && idx < 4);

    if (pTevKColorAnm == NULL) {
        mTevKColorAnm[idx].setAnmFlag(false);
    } else {
        mTevKColorAnm[idx] = *pTevKColorAnm;
    }
}

#if TARGET_PC
bool J3DMaterialAnm::hasMaterialAnimation() const {
    for (const auto& track : mMatColorAnm) {
        if (track.getAnmFlag()) return true;
    }
    for (const auto& track : mTexMtxAnm) {
        if (track.getAnmFlag()) return true;
    }
    for (const auto& track : mTevColorAnm) {
        if (track.getAnmFlag()) return true;
    }
    for (const auto& track : mTevKColorAnm) {
        if (track.getAnmFlag()) return true;
    }
    return false;
}

void J3DMatColorAnm::calc(GXColor* pColor) const {
    J3D_ASSERT_NULLPTR(507, pColor != NULL);
    dusk::interp::material::Sample materialSample(mAnmColor);
    mAnmColor->getColor(field_0x0, pColor);
}

void J3DTexMtxAnm::calc(J3DTextureSRTInfo* pSRTInfo) const {
    J3D_ASSERT_NULLPTR(519, pSRTInfo != NULL);
    dusk::interp::material::sample_texture(mAnmTransform, field_0x0, pSRTInfo);
}

void J3DTevColorAnm::calc(GXColorS10* pColor) const {
    J3D_ASSERT_NULLPTR(545, pColor != NULL);
    dusk::interp::material::Sample materialSample(mAnmTevReg);
    mAnmTevReg->getTevColorReg(field_0x0, pColor);
}

void J3DTevKColorAnm::calc(GXColor* pColor) const {
    J3D_ASSERT_NULLPTR(558, pColor != NULL);
    dusk::interp::material::Sample materialSample(mAnmTevReg);
    mAnmTevReg->getTevKonstReg(field_0x0, pColor);
}
#endif
