#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "arm.h"
#include "arm_mem.h"

#include "dma.h"
#include "io.h"
#include "sdl.h"
#include "sound.h"
#include "timer.h"

#define LINES_VISIBLE  160
#define LINES_TOTAL    228

#define CYC_LINE_TOTAL  1232
#define CYC_LINE_HBLK0  1006
#define CYC_LINE_HBLK1  (CYC_LINE_TOTAL - CYC_LINE_HBLK0)

void* screen;

// -----------------------------------------------------------------------------
// gdkGBA의 SDL_PIXELFORMAT_RGBA8888 (0xBBGGRRAA) 포맷에 맞춘 색상 변환 함수
// 비트 구조: [31:24] Blue, [23:16] Green, [15:8] Red, [7:0] Alpha(0xFF)
// -----------------------------------------------------------------------------
static inline uint32_t to_sdl_color(uint16_t c) {
    uint8_t r = (c >> 0) & 0x1F;
    uint8_t g = (c >> 5) & 0x1F;
    uint8_t b = (c >> 10) & 0x1F;

    // 5비트(0~31)를 8비트(0~255)로 확장
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);

    return ((uint32_t)b << 24) | ((uint32_t)g << 16) | ((uint32_t)r << 8) | 0xFF;
}

static const uint8_t x_tiles_lut[16] = { 1, 2, 4, 8, 2, 4, 4, 8, 1, 1, 2, 4, 0, 0, 0, 0 };
static const uint8_t y_tiles_lut[16] = { 1, 2, 4, 8, 1, 1, 2, 4, 2, 4, 4, 8, 0, 0, 0, 0 };

// -----------------------------------------------------------------------------
// 두 개의 gdkGBA 색상(0xBBGGRRAA)을 GBA BLDALPHA(EVA, EVB)로 합성
// -----------------------------------------------------------------------------
static inline uint32_t blend_color(uint32_t top_c, uint32_t bot_c) {
    uint32_t eva = bld_alpha.b.b0 & 0x1f;
    uint32_t evb = bld_alpha.b.b1 & 0x1f;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;

    // 0xBBGGRRAA 포맷에서 R, G, B 추출
    uint32_t r1 = (top_c >> 8) & 0xFF;
    uint32_t g1 = (top_c >> 16) & 0xFF;
    uint32_t b1 = (top_c >> 24) & 0xFF;

    uint32_t r2 = (bot_c >> 8) & 0xFF;
    uint32_t g2 = (bot_c >> 16) & 0xFF;
    uint32_t b2 = (bot_c >> 24) & 0xFF;

    uint32_t r = (r1 * eva + r2 * evb) >> 4;
    uint32_t g = (g1 * eva + g2 * evb) >> 4;
    uint32_t b = (b1 * eva + b2 * evb) >> 4;

    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;

    // 다시 0xBBGGRRAA 포맷으로 조합 (하위 8비트 Alpha = 0xFF 유지)
    return (b << 24) | (g << 16) | (r << 8) | 0xFF;
}

// -----------------------------------------------------------------------------
// gdkGBA 색상(0xBBGGRRAA)의 밝기(BLDY) 조절 (Fade to White / Black)
// -----------------------------------------------------------------------------
static inline uint32_t brighten_color(uint32_t c, int mode) {
    uint32_t evy = bld_bright.b.b0 & 0x1f;
    if (evy > 16) evy = 16;

    // 0xBBGGRRAA 포맷에서 R, G, B 추출
    uint32_t r = (c >> 8) & 0xFF;
    uint32_t g = (c >> 16) & 0xFF;
    uint32_t b = (c >> 24) & 0xFF;

    if (mode == 2) { // 밝게 (Fade to White)
        r += ((255 - r) * evy) >> 4;
        g += ((255 - g) * evy) >> 4;
        b += ((255 - b) * evy) >> 4;
    }
    else if (mode == 3) { // 어둡게 (Fade to Black)
        r -= (r * evy) >> 4;
        g -= (g * evy) >> 4;
        b -= (b * evy) >> 4;
    }

    // 다시 0xBBGGRRAA 포맷으로 조합
    return (b << 24) | (g << 16) | (r << 8) | 0xFF;
}

// 현재 스캔라인(240픽셀)에서 OBJ Window(마스크) 영역을 표시하는 버퍼
static bool obj_win_mask[240];

// 1단계: 해당 라인의 모든 OBJ Window(obj_mode == 2) 마스크를 먼저 스캔하는 함수
static void build_obj_win_mask(void) {
    memset(obj_win_mask, 0, sizeof(obj_win_mask));
    if (!(disp_cnt.w & OBJ_ENB) || !(disp_cnt.w & 0x8000)) return;

    uint32_t offset = 0x3f8;
    for (uint8_t obj_index = 0; obj_index < 128; obj_index++) {
        uint16_t attr0 = oam[offset + 0] | (oam[offset + 1] << 8);
        uint16_t attr1 = oam[offset + 2] | (oam[offset + 3] << 8);
        uint16_t attr2 = oam[offset + 4] | (oam[offset + 5] << 8);
        offset -= 8;

        bool    affine = (attr0 >> 8) & 0x1;
        bool    hidden = (attr0 >> 9) & 0x1;
        uint8_t obj_mode = (attr0 >> 10) & 0x3;

        if ((!affine && hidden) || obj_mode != 2) continue;

        int16_t obj_y = (attr0 >> 0) & 0xff;
        bool    dbl_size = (attr0 >> 9) & 0x1;
        uint8_t obj_shp = (attr0 >> 14) & 0x3;
        uint8_t affine_p = (attr1 >> 9) & 0x1f;
        uint8_t obj_size = (attr1 >> 14) & 0x3;

        int16_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            uint32_t p_base = affine_p * 32;
            pa = oam[p_base + 0x06] | (oam[p_base + 0x07] << 8);
            pb = oam[p_base + 0x0e] | (oam[p_base + 0x0f] << 8);
            pc = oam[p_base + 0x16] | (oam[p_base + 0x17] << 8);
            pd = oam[p_base + 0x1e] | (oam[p_base + 0x1f] << 8);
        }

        uint8_t lut_idx = obj_size | (obj_shp << 2);
        uint8_t x_tiles = x_tiles_lut[lut_idx];
        uint8_t y_tiles = y_tiles_lut[lut_idx];
        int32_t rcx = x_tiles * 4;
        int32_t rcy = y_tiles * 4;
        if (affine && dbl_size) { rcx *= 2; rcy *= 2; }
        if (obj_y + rcy * 2 > 0xff) obj_y -= 0x100;

        if (obj_y <= (int32_t)v_count.w && (obj_y + rcy * 2 > v_count.w)) {
            bool     is_256 = (attr0 >> 13) & 0x1;
            int16_t  obj_x = (attr1 >> 0) & 0x1ff;
            bool     flip_x = (attr1 >> 12) & 0x1;
            bool     flip_y = (attr1 >> 13) & 0x1;
            uint16_t chr_numb = (attr2 >> 0) & 0x3ff;
            uint32_t chr_base = 0x10000 | chr_numb * 32;

            obj_x <<= 7; obj_x >>= 7;
            int32_t x, y = v_count.w - obj_y;
            if (!affine && flip_y) y ^= (y_tiles * 8) - 1;

            uint8_t tsz = is_256 ? 64 : 32;
            uint8_t lsz = is_256 ? 8 : 4;
            int32_t ox = pa * -rcx + pb * (y - rcy) + (x_tiles << 10);
            int32_t oy = pc * -rcx + pd * (y - rcy) + (y_tiles << 10);
            if (!affine && flip_x) { ox = (x_tiles * 8 - 1) << 8; pa = -0x100; }
            uint32_t tys = (disp_cnt.w & MAP_1D_FLAG) ? x_tiles * tsz : 1024;

            for (x = 0; x < rcx * 2; x++, ox += pa, oy += pc) {
                int32_t screen_x = obj_x + x;
                if (screen_x < 0) continue;
                if (screen_x >= 240) break;

                uint16_t tile_x = ox >> 11;
                uint16_t tile_y = oy >> 11;
                if (ox < 0 || tile_x >= x_tiles || oy < 0 || tile_y >= y_tiles) continue;

                uint16_t chr_x = (ox >> 8) & 7;
                uint16_t chr_y = (oy >> 8) & 7;
                uint32_t chr_addr = chr_base + tile_y * tys + chr_y * lsz;
                uint32_t pal_idx;

                if (is_256) {
                    pal_idx = vram[chr_addr + tile_x * 64 + chr_x];
                }
                else {
                    pal_idx = (vram[chr_addr + tile_x * 32 + (chr_x >> 1)] >> ((chr_x & 1) * 4)) & 0xf;
                }

                if (pal_idx) obj_win_mask[screen_x] = true;
            }
        }
    }
}

// 2단계: 실제 스프라이트 렌더링 함수
static void render_obj(uint8_t prio) {
    if (!(disp_cnt.w & OBJ_ENB)) return;

    uint8_t obj_index;
    uint32_t offset = 0x3f8;
    uint32_t surf_addr = v_count.w * 240 * 4;

    uint8_t bld_mode = (bld_cnt.w >> 6) & 0x3;
    bool    obj_is_1st = (bld_cnt.w >> 4) & 0x1;
    bool    has_2nd = (bld_cnt.w >> 8) & 0x3f;
    bool    objwin_enb = (disp_cnt.w & 0x8000) != 0;

    for (obj_index = 0; obj_index < 128; obj_index++) {
        uint16_t attr0 = oam[offset + 0] | (oam[offset + 1] << 8);
        uint16_t attr1 = oam[offset + 2] | (oam[offset + 3] << 8);
        uint16_t attr2 = oam[offset + 4] | (oam[offset + 5] << 8);

        offset -= 8;

        int16_t  obj_y = (attr0 >> 0) & 0xff;
        bool     affine = (attr0 >> 8) & 0x1;
        bool     dbl_size = (attr0 >> 9) & 0x1;
        bool     hidden = (attr0 >> 9) & 0x1;
        uint8_t  obj_mode = (attr0 >> 10) & 0x3;
        uint8_t  obj_shp = (attr0 >> 14) & 0x3;
        uint8_t  affine_p = (attr1 >> 9) & 0x1f;
        uint8_t  obj_size = (attr1 >> 14) & 0x3;
        uint8_t  chr_prio = (attr2 >> 10) & 0x3;

        if (chr_prio != prio || (!affine && hidden)) continue;
        if (obj_mode >= 2) continue;

        int16_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            uint32_t p_base = affine_p * 32;
            pa = oam[p_base + 0x06] | (oam[p_base + 0x07] << 8);
            pb = oam[p_base + 0x0e] | (oam[p_base + 0x0f] << 8);
            pc = oam[p_base + 0x16] | (oam[p_base + 0x17] << 8);
            pd = oam[p_base + 0x1e] | (oam[p_base + 0x1f] << 8);
        }

        uint8_t lut_idx = obj_size | (obj_shp << 2);
        uint8_t x_tiles = x_tiles_lut[lut_idx];
        uint8_t y_tiles = y_tiles_lut[lut_idx];
        int32_t rcx = x_tiles * 4;
        int32_t rcy = y_tiles * 4;

        if (affine && dbl_size) { rcx *= 2; rcy *= 2; }
        if (obj_y + rcy * 2 > 0xff) obj_y -= 0x100;

        if (obj_y <= (int32_t)v_count.w && (obj_y + rcy * 2 > v_count.w)) {
            bool     is_256 = (attr0 >> 13) & 0x1;
            int16_t  obj_x = (attr1 >> 0) & 0x1ff;
            bool     flip_x = (attr1 >> 12) & 0x1;
            bool     flip_y = (attr1 >> 13) & 0x1;
            uint16_t chr_numb = (attr2 >> 0) & 0x3ff;
            uint8_t  chr_pal = (attr2 >> 12) & 0xf;

            uint32_t chr_base = 0x10000 | chr_numb * 32;
            obj_x <<= 7; obj_x >>= 7;

            int32_t x, y = v_count.w - obj_y;
            if (!affine && flip_y) y ^= (y_tiles * 8) - 1;

            uint8_t tsz = is_256 ? 64 : 32;
            uint8_t lsz = is_256 ? 8 : 4;

            int32_t ox = pa * -rcx + pb * (y - rcy) + (x_tiles << 10);
            int32_t oy = pc * -rcx + pd * (y - rcy) + (y_tiles << 10);

            if (!affine && flip_x) { ox = (x_tiles * 8 - 1) << 8; pa = -0x100; }

            uint32_t tys = (disp_cnt.w & MAP_1D_FLAG) ? x_tiles * tsz : 1024;
            uint32_t address = surf_addr + obj_x * 4;

            for (x = 0; x < rcx * 2; x++, ox += pa, oy += pc, address += 4) {
                int32_t screen_x = obj_x + x;
                if (screen_x < 0) continue;
                if (screen_x >= 240) break;

                if (objwin_enb) {
                    uint8_t win_flags = obj_win_mask[screen_x] ? win_out.b.b1 : win_out.b.b0;
                    if (!(win_flags & 0x10)) continue;
                }

                uint16_t tile_x = ox >> 11;
                uint16_t tile_y = oy >> 11;
                if (ox < 0 || tile_x >= x_tiles || oy < 0 || tile_y >= y_tiles) continue;

                uint16_t chr_x = (ox >> 8) & 7;
                uint16_t chr_y = (oy >> 8) & 7;
                uint32_t chr_addr = chr_base + tile_y * tys + chr_y * lsz;
                uint32_t pal_idx;

                if (is_256) {
                    pal_idx = vram[chr_addr + tile_x * 64 + chr_x];
                }
                else {
                    pal_idx = (vram[chr_addr + tile_x * 32 + (chr_x >> 1)] >> ((chr_x & 1) * 4)) & 0xf;
                }

                if (pal_idx) {
                    uint32_t pal_addr = 0x100 | pal_idx | (!is_256 ? chr_pal * 16 : 0);
                    uint32_t obj_col = palette[pal_addr];
                    uint32_t* dst_ptr = (uint32_t*)((uint8_t*)screen + address);

                    if (obj_mode == 1 && has_2nd) {
                        *dst_ptr = blend_color(obj_col, *dst_ptr);
                    }
                    else if (obj_is_1st && bld_mode == 1 && has_2nd) {
                        *dst_ptr = blend_color(obj_col, *dst_ptr);
                    }
                    else if (obj_is_1st && (bld_mode == 2 || bld_mode == 3)) {
                        *dst_ptr = brighten_color(obj_col, bld_mode);
                    }
                    else {
                        *dst_ptr = obj_col;
                    }
                }
            }
        }
    }
}

static const uint8_t bg_enb[3] = { 0xf, 0x7, 0xc };

static void render_bg(void) {
    uint8_t mode = disp_cnt.w & 7;

    uint32_t surf_addr = v_count.w * 240 * 4;

    switch (mode) {
    case 0:
    case 1:
    case 2: {
        uint8_t enb = (disp_cnt.w >> 8) & bg_enb[mode];
        uint8_t bld_mode = (bld_cnt.w >> 6) & 0x3;

        int8_t prio, bg_idx;

        for (prio = 3; prio >= 0; prio--) {
            for (bg_idx = 3; bg_idx >= 0; bg_idx--) {
                if (!(enb & (1 << bg_idx))) continue;
                if ((bg[bg_idx].ctrl.w & 3) != prio) continue;

                bool bg_is_1st = (bld_cnt.w >> bg_idx) & 0x1;
                bool has_2nd = (bld_cnt.w >> 8) & 0x3f;

                uint32_t chr_base = ((bg[bg_idx].ctrl.w >> 2) & 0x3) << 14;
                uint16_t scrn_base = ((bg[bg_idx].ctrl.w >> 8) & 0x1f) << 11;
                bool     aff_wrap = (bg[bg_idx].ctrl.w >> 13) & 0x1;
                uint16_t scrn_size = (bg[bg_idx].ctrl.w >> 14);

                bool affine = mode == 2 || (mode == 1 && bg_idx == 2);

                uint32_t address = surf_addr;

                if (affine) {
                    int16_t pa = bg_pa[bg_idx].w;
                    int16_t pb = bg_pb[bg_idx].w;
                    int16_t pc = bg_pc[bg_idx].w;
                    int16_t pd = bg_pd[bg_idx].w;

                    int32_t ox = ((int32_t)bg_refxi[bg_idx].w << 4) >> 4;
                    int32_t oy = ((int32_t)bg_refyi[bg_idx].w << 4) >> 4;

                    bg_refxi[bg_idx].w += pb;
                    bg_refyi[bg_idx].w += pd;

                    uint8_t tms = 16 << scrn_size;
                    uint8_t tmsk = tms - 1;

                    uint8_t x;

                    for (x = 0; x < 240;
                        x++,
                        ox += pa,
                        oy += pc,
                        address += 4) {
                        int16_t tmx = ox >> 11;
                        int16_t tmy = oy >> 11;

                        if (aff_wrap) {
                            tmx &= tmsk;
                            tmy &= tmsk;
                        }
                        else {
                            if (tmx < 0 || tmx >= tms) continue;
                            if (tmy < 0 || tmy >= tms) continue;
                        }

                        uint16_t chr_x = (ox >> 8) & 7;
                        uint16_t chr_y = (oy >> 8) & 7;

                        uint32_t map_addr = scrn_base + tmy * tms + tmx;
                        uint32_t vram_addr = chr_base + vram[map_addr] * 64 + chr_y * 8 + chr_x;
                        uint16_t pal_idx = vram[vram_addr];

                        if (pal_idx) {
                            uint32_t bg_col = palette[pal_idx];
                            uint32_t* dst_ptr = (uint32_t*)((uint8_t*)screen + address);

                            if (bg_is_1st && bld_mode == 1 && has_2nd) {
                                *dst_ptr = blend_color(bg_col, *dst_ptr);
                            }
                            else if (bg_is_1st && (bld_mode == 2 || bld_mode == 3)) {
                                *dst_ptr = brighten_color(bg_col, bld_mode);
                            }
                            else {
                                *dst_ptr = bg_col;
                            }
                        }
                    }
                }
                else {
                    uint16_t oy = v_count.w + bg[bg_idx].yofs.w;
                    uint16_t tmy = oy >> 3;
                    uint16_t scrn_y = (tmy >> 5) & 1;

                    uint8_t x;

                    for (x = 0; x < 240; x++) {
                        uint16_t ox = x + bg[bg_idx].xofs.w;
                        uint16_t tmx = ox >> 3;
                        uint16_t scrn_x = (tmx >> 5) & 1;

                        uint16_t chr_x = ox & 7;
                        uint16_t chr_y = oy & 7;

                        uint16_t pal_idx;
                        uint16_t pal_base = 0;

                        uint32_t map_addr = scrn_base + (tmy & 0x1f) * 32 * 2 + (tmx & 0x1f) * 2;

                        switch (scrn_size) {
                        case 1: map_addr += scrn_x * 2048; break;
                        case 2: map_addr += scrn_y * 2048; break;
                        case 3: map_addr += scrn_x * 2048 + scrn_y * 4096; break;
                        }

                        uint16_t tile = vram[map_addr + 0] | (vram[map_addr + 1] << 8);

                        uint16_t chr_numb = (tile >> 0) & 0x3ff;
                        bool     flip_x = (tile >> 10) & 0x1;
                        bool     flip_y = (tile >> 11) & 0x1;
                        uint8_t  chr_pal = (tile >> 12) & 0xf;
                        bool     is_256 = (bg[bg_idx].ctrl.w >> 7) & 0x1;

                        if (!is_256) pal_base = chr_pal * 16;

                        if (flip_x) chr_x ^= 7;
                        if (flip_y) chr_y ^= 7;

                        uint32_t vram_addr;

                        if (is_256) {
                            vram_addr = chr_base + chr_numb * 64 + chr_y * 8 + chr_x;
                            pal_idx = vram[vram_addr];
                        }
                        else {
                            vram_addr = chr_base + chr_numb * 32 + chr_y * 4 + (chr_x >> 1);
                            pal_idx = (vram[vram_addr] >> (chr_x & 1) * 4) & 0xf;
                        }

                        uint32_t pal_addr = pal_idx | pal_base;

                        if (pal_idx) {
                            uint32_t bg_col = palette[pal_addr];
                            uint32_t* dst_ptr = (uint32_t*)((uint8_t*)screen + address);

                            uint32_t backdrop_col = palette[0];
                            // Backdrop이 밝기 조절된 상태일 수도 있으므로 두 경우 모두 체크
                            uint8_t bld_m = (bld_cnt.w >> 6) & 0x3;
                            bool bd_is_1st = (bld_cnt.w >> 5) & 0x1;
                            if (bd_is_1st && (bld_m == 2 || bld_m == 3)) {
                                backdrop_col = brighten_color(backdrop_col, bld_m);
                            }

                            bool is_on_backdrop = (*dst_ptr == backdrop_col);
                            bool bd_is_2nd = (bld_cnt.w >> 13) & 0x1;

                            if (bg_is_1st && bld_mode == 1 && has_2nd) {
                                if (!is_on_backdrop || bd_is_2nd) {
                                    *dst_ptr = blend_color(bg_col, *dst_ptr);
                                }
                            }
                            else if (bg_is_1st && (bld_mode == 2 || bld_mode == 3)) {
                                *dst_ptr = brighten_color(bg_col, bld_mode);
                            }
                            else {
                                *dst_ptr = bg_col;
                            }
                        }

                        address += 4;
                    }
                }
            }

            render_obj(prio);
        }
    }
          break;

    case 3: {
        uint8_t x;
        uint32_t frm_addr = v_count.w * 480;

        for (x = 0; x < 240; x++) {
            uint16_t pixel = vram[frm_addr + 0] | (vram[frm_addr + 1] << 8);

            // gdkGBA의 SDL_PIXELFORMAT_RGBA8888 (0xBBGGRRAA) 포맷으로 변환
            *(uint32_t*)((uint8_t*)screen + surf_addr) = to_sdl_color(pixel);

            surf_addr += 4;
            frm_addr += 2;
        }
    }
          break;

    case 4: {
        uint8_t x, frame = (disp_cnt.w >> 4) & 1;
        uint32_t frm_addr = 0xa000 * frame + v_count.w * 240;

        for (x = 0; x < 240; x++) {
            uint8_t pal_idx = vram[frm_addr++];

            *(uint32_t*)((uint8_t*)screen + surf_addr) = palette[pal_idx];

            surf_addr += 4;
        }
    }
          break;
    }
}

static void render_line(void) {
    uint32_t addr;

    uint32_t addr_s = v_count.w * 240 * 4;
    uint32_t addr_e = addr_s + 240 * 4;

    uint32_t bg_color = palette[0];

    // [추가] Backdrop(바탕화면, 비트 5)이 BLDCNT의 1st Target으로 설정되어
    // 화면 전체가 페이드 인/아웃(모드 2, 3) 될 때 바탕색에도 밝기 조절 적용!
    uint8_t bld_mode = (bld_cnt.w >> 6) & 0x3;
    bool bd_is_1st = (bld_cnt.w >> 5) & 0x1;
    if (bd_is_1st && (bld_mode == 2 || bld_mode == 3)) {
        bg_color = brighten_color(bg_color, bld_mode);
    }

    for (addr = addr_s; addr < addr_e; addr += 0x10) {
        *(uint32_t*)((uint8_t*)screen + (addr | 0x0)) = bg_color;
        *(uint32_t*)((uint8_t*)screen + (addr | 0x4)) = bg_color;
        *(uint32_t*)((uint8_t*)screen + (addr | 0x8)) = bg_color;
        *(uint32_t*)((uint8_t*)screen + (addr | 0xc)) = bg_color;
    }

    // 배경과 스프라이트를 그리기 전에 현재 줄의 OBJ Window 마스크를 먼저 생성
    build_obj_win_mask();

    if ((disp_cnt.w & 7) > 2) {
        render_bg();
        render_obj(0);
        render_obj(1);
        render_obj(2);
        render_obj(3);
    }
    else {
        render_bg();
    }
}


static void vblank_start() {
    if (disp_stat.w & VBLK_IRQ) trigger_irq(VBLK_FLAG);

    disp_stat.w |= VBLK_FLAG;
}

static void hblank_start() {
    if (disp_stat.w & HBLK_IRQ) trigger_irq(HBLK_FLAG);

    disp_stat.w |= HBLK_FLAG;
}

static void vcount_match() {
    if (disp_stat.w & VCNT_IRQ) trigger_irq(VCNT_FLAG);

    disp_stat.w |= VCNT_FLAG;
}

void run_frame() {
    disp_stat.w &= ~VBLK_FLAG;

    SDL_LockTexture(texture, NULL, &screen, &tex_pitch);

    for (v_count.w = 0; v_count.w < LINES_TOTAL; v_count.w++) {
        disp_stat.w &= ~(HBLK_FLAG | VCNT_FLAG);

        //V-Count match and V-Blank start
        if (v_count.w == disp_stat.b.b1) vcount_match();

        if (v_count.w == LINES_VISIBLE) {
            bg_refxi[2].w = bg_refxe[2].w;
            bg_refyi[2].w = bg_refye[2].w;

            bg_refxi[3].w = bg_refxe[3].w;
            bg_refyi[3].w = bg_refye[3].w;

            vblank_start();
            dma_transfer(VBLANK);
        }

        arm_exec(CYC_LINE_HBLK0);

        //H-Blank start
        if (v_count.w < LINES_VISIBLE) {
            render_line();
            dma_transfer(HBLANK);
        }

        hblank_start();

        arm_exec(CYC_LINE_HBLK1);

        sound_clock(CYC_LINE_TOTAL);
    }

    SDL_UnlockTexture(texture);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);

    sound_buffer_wrap();
}