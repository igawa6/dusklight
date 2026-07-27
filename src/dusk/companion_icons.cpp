// Companion dashboard: icon caches. Item icons are BTI resources read
// through the game's own loaders into static buffers (no game-heap
// allocations, so stage transitions can't invalidate them): the two-layer
// item icons via dMeter2Info_readItemTexture, raw ItemIcon-archive
// resources by index, and the collect-archive category art.

#include "dusk/companion.h"
#include "dusk/companion_internal.h"

#include "JSystem/J2DGraph/J2DPicture.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_meter2_info.h"
#include "res/Layout/clctres.h"


namespace dusk::companion {
namespace {

// Item icons are 0xC00-byte BTI resources read from the item icon archive.
// Cache slots: 24 inventory + B/X/Y buttons + fixed UI icons. All static —
// no game-heap allocations, so stage transitions can't invalidate us.
alignas(32) u8 s_iconBuf[ICON_SLOT_COUNT][ICON_BUF_SIZE];
u8 s_iconItem[ICON_SLOT_COUNT];
bool s_iconValid[ICON_SLOT_COUNT];

// Item icons are two-layer composites (base + tinted content overlay, e.g.
// bottle fill) loaded through the game's own dMeter2Info_readItemTexture,
// which also applies the per-type tint colors to the pictures.
alignas(32) u8 s_iconBuf2[ICON_SLOT_COUNT][ICON_BUF_SIZE];
J2DPicture* s_itemPic[ICON_SLOT_COUNT][2];
int s_iconLayers[ICON_SLOT_COUNT];

int updateItemPics(int slot, u8 itemNo) {
    if (itemNo == dItemNo_NONE_e || dComIfGp_getItemIconArchive() == NULL) {
        return 0;
    }
    // The lantern icon swaps to its "off" art when the oil runs out; fold
    // that state into the cache key.
    u8 cacheKey = itemNo;
    if (itemNo == dItemNo_KANTERA_e && dComIfGs_getOil() == 0) {
        cacheKey = dItemNo_KANTERA2_e;
    }
    if (s_iconValid[slot] && s_iconItem[slot] == cacheKey) {
        return s_iconLayers[slot];
    }
    if (s_itemPic[slot][0] == NULL) {
        // Seed the pictures with any valid TIMG; readItemTexture swaps them.
        const s16 texIdx = dItem_data::getTexture(itemNo);
        if (texIdx < 0 ||
            JKRReadIdxResource(s_iconBuf[slot], ICON_BUF_SIZE, texIdx,
                dComIfGp_getItemIconArchive()) == 0)
        {
            return 0;
        }
        s_itemPic[slot][0] = createPicture((ResTIMG*)s_iconBuf[slot]);
        s_itemPic[slot][1] = createPicture((ResTIMG*)s_iconBuf[slot]);
        if (s_itemPic[slot][0] == NULL || s_itemPic[slot][1] == NULL) {
            return 0;
        }
    }
    const int layers = dMeter2Info_readItemTexture(itemNo, s_iconBuf[slot],
        s_itemPic[slot][0], s_iconBuf2[slot], s_itemPic[slot][1], NULL, NULL, NULL, NULL, -1);
    s_iconItem[slot] = cacheKey;
    s_iconValid[slot] = true;
    s_iconLayers[slot] = layers;
    return layers;
}

// Collection-menu category art read straight from the collect archive. The
// archive handle is mounted at boot and never unmounted (only its loaded
// resources are evicted while the pause menu closes), so the BTI blobs can
// be copied into our buffers on the first frame — no pause-menu visit
// needed. Slots: 0 fish journal, 1 skills scroll, 2 letter, 3-7 scents
// (medicine/children/fish/iria/poe), 8 heart-piece base, 9-12 the four
// cumulative wedge overlays.
alignas(32) u8 s_collectIconBuf[CLCT_ICON_COUNT][0x5000];
bool s_collectIconValid[CLCT_ICON_COUNT];

// Raw ItemIcon-archive textures (see RAWICON_* in companion_internal.h).
alignas(32) u8 s_rawIconBuf[1][0x2800];
int s_rawIconIdx[1] = {-1};

}  // namespace

void drawItemIcon(int slot, u8 itemNo, f32 x, f32 y, f32 size, u8 alpha) {
    const int layers = updateItemPics(slot, itemNo);
    if (layers <= 0) {
        return;
    }
    const ResTIMG* timg = s_itemPic[slot][0]->getTexture(0) != NULL
        ? s_itemPic[slot][0]->getTexture(0)->getTexInfo() : NULL;
    if (timg == NULL || timg->width == 0 || timg->height == 0) {
        return;
    }
    f32 w = size;
    f32 h = size * (f32)timg->height / (f32)timg->width;
    if (h > size) {
        h = size;
        w = size * (f32)timg->width / (f32)timg->height;
    }
    const f32 dx = x + (size - w) * 0.5f;
    const f32 dy = y + (size - h) * 0.5f;
    for (int i = 0; i < layers && i < 2; i++) {
        s_itemPic[slot][i]->setAlpha(mulDrawAlpha(alpha));
        s_itemPic[slot][i]->draw(dx, dy, w, h, false, false, false);
    }
    // The map texture cache tracks s_iconPic only; these draws do not touch it.
    dComIfGp_getCurrentGrafPort()->setup2D();
}

// Flat-color silhouette of an item icon — drawn slightly enlarged behind
// the real icon it forms a colored border/glow. Preserves each layer's own
// tint colors around the draw.
void drawItemIconSilhouette(int slot, u8 itemNo, f32 x, f32 y, f32 size, u32 rgba) {
    const int layers = updateItemPics(slot, itemNo);
    if (layers <= 0) {
        return;
    }
    const ResTIMG* timg = s_itemPic[slot][0]->getTexture(0) != NULL
        ? s_itemPic[slot][0]->getTexture(0)->getTexInfo() : NULL;
    if (timg == NULL || timg->width == 0 || timg->height == 0) {
        return;
    }
    f32 w = size;
    f32 h = size * (f32)timg->height / (f32)timg->width;
    if (h > size) {
        h = size;
        w = size * (f32)timg->width / (f32)timg->height;
    }
    const f32 dx = x + (size - w) * 0.5f;
    const f32 dy = y + (size - h) * 0.5f;
    for (int i = 0; i < layers && i < 2; i++) {
        J2DPicture* pic = s_itemPic[slot][i];
        const JUtility::TColor black = pic->getBlack();
        const JUtility::TColor white = pic->getWhite();
        // Alpha must follow the texture (black a=0, white a=FF), or the
        // flat tint fills the whole quad instead of the icon's shape.
        pic->setBlackWhite(JUtility::TColor(rgba & 0xFFFFFF00u), JUtility::TColor(rgba | 0xFFu));
        pic->setAlpha(mulDrawAlpha(0xFF));
        pic->draw(dx, dy, w, h, false, false, false);
        pic->setAlpha(0xFF);
        pic->setBlackWhite(black, white);
    }
    dComIfGp_getCurrentGrafPort()->setup2D();
}

// i_xy: X/Y button index for equipped items (bombs read their own bag via
// dComIfGp_getSelectItemNum, like the game HUD); i_invSlot: inventory slot
// for the ITEMS grid (bomb bags live in SLOT_15..SLOT_17).
int ammoForItem(u8 itemNo, int i_xy, int i_invSlot) {
    switch (itemNo) {
    case dItemNo_BOW_e:
    case dItemNo_HAWK_ARROW_e:
    case dItemNo_LIGHT_ARROW_e:
        return dComIfGs_getArrowNum();
    case dItemNo_BOMB_ARROW_e: {
        const int bombs = i_xy >= 0 ? dComIfGp_getSelectItemNum(i_xy) : 0;
        const int arrows = dComIfGs_getArrowNum();
        return bombs < arrows ? bombs : arrows;
    }
    case dItemNo_NORMAL_BOMB_e:
    case dItemNo_WATER_BOMB_e:
    case dItemNo_POKE_BOMB_e:
        if (i_xy >= 0) {
            return dComIfGp_getSelectItemNum(i_xy);
        }
        if (i_invSlot >= SLOT_15 && i_invSlot <= SLOT_17) {
            return dComIfGs_getBombNum(i_invSlot - SLOT_15);
        }
        return dComIfGs_getBombNum(0);
    case dItemNo_BOMB_BAG_LV1_e:
    case dItemNo_BOMB_BAG_LV2_e:
        return dComIfGs_getBombNum(0);
    case dItemNo_PACHINKO_e:
        return dComIfGs_getPachinkoNum();
    default:
        return -1;
    }
}

#include "companion_logo.inc"

// Pause-menu decoration art (see DECO_* in companion_internal.h). Sized for
// the largest piece, TT_BLOCK128 (128x128).
alignas(32) u8 s_decoBuf[DECO_COUNT][0x9000];
bool s_decoValid[DECO_COUNT];

void loadCollectIcons() {
    static bool s_loaded = false;
    if (s_loaded) {
        return;
    }
    JKRArchive* arc = dComIfGp_getCollectResArchive();
    if (arc == NULL) {
        return;
    }
    static const u16 l_res[CLCT_ICON_COUNT] = {
        dRes_INDEX_CLCTRES_BTI_NI_ITEM_ICON_FISH_e,
        dRes_INDEX_CLCTRES_BTI_NI_ITEM_ICON_MAKIMONO_e,
        dRes_INDEX_CLCTRES_BTI_NI_ITEM_ICON_LETTER_e,
        dRes_INDEX_CLCTRES_BTI_NI_NIOI_MEDICIN_e,
        dRes_INDEX_CLCTRES_BTI_NI_NIOI_CHILD_e,
        dRes_INDEX_CLCTRES_BTI_NI_NIOI_FISH_e,
        dRes_INDEX_CLCTRES_BTI_NI_NIOI_IRIA_e,
        dRes_INDEX_CLCTRES_BTI_NI_NIOI_POU_e,
        dRes_INDEX_CLCTRES_BTI_ZELDA_HEART_PARTS_IWASAWA_VER2_BASE_00_e,
        dRes_INDEX_CLCTRES_BTI_ZELDA_HEART_PARTS_IWASAWA_VER2_PARTS1_e,
        dRes_INDEX_CLCTRES_BTI_ZELDA_HEART_PARTS_IWASAWA_VER2_PARTS2_e,
        dRes_INDEX_CLCTRES_BTI_ZELDA_HEART_PARTS_IWASAWA_VER2_PARTS3_e,
        dRes_INDEX_CLCTRES_BTI_ZELDA_HEART_PARTS_IWASAWA_VER2_PARTS4_e,
    };
    for (int i = 0; i < CLCT_ICON_COUNT; i++) {
        const u32 size = JKRReadIdxResource(s_collectIconBuf[i], sizeof(s_collectIconBuf[0]),
            l_res[i], arc);
        s_collectIconValid[i] = size != 0 && size <= sizeof(s_collectIconBuf[0]);
        if (s_collectIconValid[i]) {
            // Force bilinear filtering — the companion draws these well
            // above their native size (e.g. the big heart).
            ResTIMG* t = (ResTIMG*)s_collectIconBuf[i];
            t->minFilter = 1;
            t->magFilter = 1;
        }
    }
    static const u16 l_deco[DECO_COUNT] = {
        dRes_INDEX_CLCTRES_BTI_TT_BLOCK128_00_e,
        dRes_INDEX_CLCTRES_BTI_IM_DUNGEON_MAP_FLOOR_PARTS_10_e,  // wi_save plate
        dRes_INDEX_CLCTRES_BTI_TT_LINE2_e,
        dRes_INDEX_CLCTRES_BTI_TT_YAKUSHIMA_e,
        dRes_INDEX_CLCTRES_BTI_TT_SPOT_SQUARE3_e,
        dRes_INDEX_CLCTRES_BTI_TT_KAZARI_2ND_OKAN_64_e,
    };
    for (int i = 0; i < DECO_COUNT; i++) {
        const u32 size = JKRReadIdxResource(s_decoBuf[i], sizeof(s_decoBuf[0]), l_deco[i], arc);
        s_decoValid[i] = size != 0 && size <= sizeof(s_decoBuf[0]);
    }
    s_loaded = true;
}

const ResTIMG* dusklightLogoTimg() {
    return (const ResTIMG*)l_dusklightLogo;
}


const ResTIMG* decoTimg(int slot) {
    if (slot < 0 || slot >= DECO_COUNT || !s_decoValid[slot]) {
        return NULL;
    }
    return (const ResTIMG*)s_decoBuf[slot];
}

const ResTIMG* collectIconTimg(int slot) {
    if (slot < 0 || slot >= CLCT_ICON_COUNT || !s_collectIconValid[slot]) {
        return NULL;
    }
    return (const ResTIMG*)s_collectIconBuf[slot];
}

const ResTIMG* rawArchiveIcon(int slot, int resIdx) {
    if (dComIfGp_getItemIconArchive() == NULL) {
        return NULL;
    }
    if (s_rawIconIdx[slot] != resIdx) {
        if (JKRReadIdxResource(s_rawIconBuf[slot], sizeof(s_rawIconBuf[0]), resIdx,
                dComIfGp_getItemIconArchive()) == 0)
        {
            return NULL;
        }
        s_rawIconIdx[slot] = resIdx;
    }
    return (const ResTIMG*)s_rawIconBuf[slot];
}

}  // namespace dusk::companion
