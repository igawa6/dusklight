#ifndef D_PANE_D_PANE_CLASS_H
#define D_PANE_D_PANE_CLASS_H

#include "JSystem/JUtility/TColor.h"
#include "d/d_pane_class_alpha.h"

#if TARGET_PC
#include <utility>
#endif

class JKRHeap;

void dPaneClass_showNullPane(J2DScreen*);
void dPaneClass_showNullPane(J2DPane*);
bool dPaneClass_setPriority(void**, JKRHeap*, J2DScreen*, char const*, u32, JKRArchive*);

class CPaneMgr : public CPaneMgrAlpha {
public:
    virtual ~CPaneMgr();
    virtual void setAlpha(u8);

    CPaneMgr(J2DScreen* i_scrn, u64 i_tag, u8 i_flags, JKRExpHeap* i_heap);
    CPaneMgr();
    void reinit();
    void initiate(J2DPane*, JKRExpHeap*);
    void childPaneGetSize(J2DPane*);
    void childPaneSetSize(J2DPane*, f32, f32);
    f32 getGlobalPosX();
    f32 getGlobalPosY();
    void setBlackWhite(JUtility::TColor, JUtility::TColor);
    void paneTrans(f32, f32);
    void paneScale(f32, f32);
#if TARGET_PC
    void presentAnime();
#endif
    bool scaleAnime(s16, f32, f32, u8);
    bool colorAnime(s16, JUtility::TColor, JUtility::TColor, JUtility::TColor,
                                   JUtility::TColor, u8);
    Vec getGlobalVtx(J2DPane*, f32 (*)[3][4], u8, bool, s16);
    Vec getGlobalVtxCenter(J2DPane*, bool, s16);
    JGeometry::TBox2<f32>* getBounds(J2DPane*);

    Vec getGlobalVtx(Mtx* param_0, u8 param_1, bool param_2, s16 param_3) {
        return getGlobalVtx(mPane, param_0, param_1, param_2, param_3);
    }

    Vec getGlobalVtxCenter(bool param_0, s16 param_1) {
        return getGlobalVtxCenter(getPanePtr(), param_0, param_1);
    }

    f32 getGlobalCenterPosY() {
        return getGlobalPosY() + mPane->getHeight() / 2;
    }

    f32 getCenterPosX() {
        return mPane->getBounds().i.x + mPane->getWidth() / 2;
    }

    void translate(f32 x, f32 y) { getPanePtr()->translate(x, y); }
    void scale(f32 h, f32 v) { getPanePtr()->scale(h, v); }
    void resize(f32 x, f32 y) { getPanePtr()->resize(x, y); }
    void move(f32 x, f32 y) { getPanePtr()->move(x, y); }

    void scaleAnimeStart(DUSK_IF_ELSE(f32, s16) v) {
        mScaleAnime = v;
        IF_DUSK(mScaleAnimation.active = false;)
    }
    void colorAnimeStart(DUSK_IF_ELSE(f32, s16) start) {
        mColorAnime = start;
        IF_DUSK(mColorAnimation.active = false;)
    }

    f32 getPosX() { return getPanePtr()->getBounds().i.x; }
    f32 getPosY() { return getPanePtr()->getBounds().i.y; }

    f32 getSizeX() { return mPane->getWidth(); }
    f32 getSizeY() { return mPane->getHeight(); }

    f32 getRotateZ() { return getPanePtr()->getRotateZ(); }

    f32 getInitCenterPosX() { return mInitPos.x + mInitSize.x * 0.5f; }

    f32 getInitCenterPosY() { return mInitPos.y + mInitSize.y * 0.5f; }

    f32 getInitPosX() { return mInitPos.x; }
    f32 getInitPosY() { return mInitPos.y; }

    f32 getInitGlobalPosX() { return mGlobalPos.x; }
    f32 getInitGlobalPosY() { return mGlobalPos.y; }

    f32 getInitGlobalCenterPosX() { return mGlobalPos.x + mInitSize.x * 0.5f; }
    f32 getInitGlobalCenterPosY() { return mGlobalPos.y + mInitSize.y * 0.5f; }

    f32 getInitSizeX() { return mInitSize.x; }
    f32 getInitSizeY() { return mInitSize.y; }

    f32 getInitScaleX() { return mInitScale.x; }
    f32 getInitScaleY() { return mInitScale.y; }

    f32 getScaleX() { return mPane->getScaleX(); }
    f32 getScaleY() { return mPane->getScaleY(); }

    f32 getTranslateX() { return mPane->getTranslateX(); }
    f32 getTranslateY() { return mPane->getTranslateY(); }

    JUtility::TColor getInitBlack() { return mInitBlack; }
    JUtility::TColor getInitWhite() { return mInitWhite; }

    /* 0x1C */ void* mpFirstStackSize;
    /* 0x20 */ s16* field_0x20;
    /* 0x24 */ JGeometry::TVec2<f32> mInitPos;
    /* 0x2C */ JGeometry::TVec2<f32> mGlobalPos;
    /* 0x34 */ JGeometry::TVec2<f32> mInitSize;
    /* 0x3C */ JGeometry::TVec2<f32> mInitScale;
    /* 0x44 */ JGeometry::TVec2<f32> mInitTrans;
    /* 0x4C */ f32 mRotateZ;
    /* 0x50 */ JGeometry::TVec2<f32> mRotateOffset;
    /* 0x58 */ JUtility::TColor mInitWhite;
    /* 0x5C */ JUtility::TColor mInitBlack;
    /* 0x60 */ s16 field_0x60;
    /* 0x62 */ s16 field_0x62;
    /* 0x64 */ DUSK_IF_ELSE(f32, s16) mScaleAnime;
    /* 0x66 */ s16 field_0x66;
    /* 0x68 */ s16 field_0x68;
    /* 0x6A */ DUSK_IF_ELSE(f32, s16) mColorAnime;
#if TARGET_PC
    Animation<f32> mScaleAnimation;
    Animation<std::pair<JUtility::TColor, JUtility::TColor>> mColorAnimation;
#endif
};

#endif /* D_PANE_D_PANE_CLASS_H */
