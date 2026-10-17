#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "arm.h"
#include "arm_mem.h"

#include "dma.h"
#include "io.h"
#include "sound.h"
#include "timer.h"

#define LINES_VISIBLE   160
#define LINES_TOTAL     228

#define CYC_LINE_TOTAL  1232
#define CYC_LINE_HBLK0  1006
#define CYC_LINE_HBLK1  (CYC_LINE_TOTAL - CYC_LINE_HBLK0)

// 240x160 32비트 XRGB 화면 버퍼 (VB6 BitBlt / DIBSection 연동용)
uint32_t gba_screen[240 * LINES_VISIBLE];
void *screen = (void *)gba_screen;

// 스캔라인별 픽셀의 출처 레이어 기록 (0~3: BG0~BG3, 4: OBJ, 5: Backdrop)
static uint8_t line_layer[240];
// 스캔라인별 OBJ Window(obj_mode == 2) 마스크 영역 표시
static bool obj_win_mask[240];

// -----------------------------------------------------------------------------
// gdkGBA 팔레트 포맷을 Windows DIBSection(0x00RRGGBB)으로 변환
// -----------------------------------------------------------------------------
static inline uint32_t to_win_color(uint32_t c) {
    uint8_t r = (c >>  8) & 0xFF;
    uint8_t g = (c >> 16) & 0xFF;
    uint8_t b = (c >> 24) & 0xFF;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static const uint8_t x_tiles_lut[16] = { 1, 2, 4, 8, 2, 4, 4, 8, 1, 1, 2, 4, 0, 0, 0, 0 };
static const uint8_t y_tiles_lut[16] = { 1, 2, 4, 8, 1, 1, 2, 4, 2, 4, 4, 8, 0, 0, 0, 0 };

// -----------------------------------------------------------------------------
// 두 개의 Windows 색상(0x00RRGGBB)을 GBA BLDALPHA(EVA, EVB) 또는 밝기(BLDY)로 합성
// -----------------------------------------------------------------------------
static inline uint32_t blend_color(uint32_t top_c, uint32_t bot_c) {
    uint32_t eva = bld_alpha.b.b0 & 0x1f;
    uint32_t evb = bld_alpha.b.b1 & 0x1f;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;

    uint32_t r1 = (top_c >> 16) & 0xFF;
    uint32_t g1 = (top_c >>  8) & 0xFF;
    uint32_t b1 = (top_c >>  0) & 0xFF;

    uint32_t r2 = (bot_c >> 16) & 0xFF;
    uint32_t g2 = (bot_c >>  8) & 0xFF;
    uint32_t b2 = (bot_c >>  0) & 0xFF;

    uint32_t r = (r1 * eva + r2 * evb) >> 4;
    uint32_t g = (g1 * eva + g2 * evb) >> 4;
    uint32_t b = (b1 * eva + b2 * evb) >> 4;

    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;

    return (r << 16) | (g << 8) | b;
}

static inline uint32_t brighten_color(uint32_t c, int mode) {
    uint32_t evy = bld_bright.b.b0 & 0x1f;
    if (evy > 16) evy = 16;

    uint32_t r = (c >> 16) & 0xFF;
    uint32_t g = (c >>  8) & 0xFF;
    uint32_t b = (c >>  0) & 0xFF;

    if (mode == 2) { // 밝게 (Fade to White)
        r += ((255 - r) * evy) >> 4;
        g += ((255 - g) * evy) >> 4;
        b += ((255 - b) * evy) >> 4;
    } else if (mode == 3) { // 어둡게 (Fade to Black)
        r -= (r * evy) >> 4;
        g -= (g * evy) >> 4;
        b -= (b * evy) >> 4;
    }

    return (r << 16) | (g << 8) | b;
}

// -----------------------------------------------------------------------------
// 좌표 (x, y)가 윈도우 범위 [x1, x2), [y1, y2) 안에 있는지 판별하는 헬퍼 함수
// (GBA 하드웨어 스펙: x1 > x2 또는 y1 > y2인 경우 화면 끝을 넘어 랩어라운드 처리)
// -----------------------------------------------------------------------------
static inline bool in_window_rect(int x, int y, uint8_t x1, uint8_t x2, uint8_t y1, uint8_t y2) {
    bool in_x = (x1 <= x2) ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
    bool in_y = (y1 <= y2) ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
    return in_x && in_y;
}

// -----------------------------------------------------------------------------
// 현재 픽셀(x, v_count.w)에 적용될 정석 GBA 윈도우 제어 플래그 반환
// 우선순위: WIN0 > WIN1 > OBJWIN > WINOUT
// -----------------------------------------------------------------------------
static inline uint8_t get_win_flags(int x) {
    uint16_t win_enb = disp_cnt.w & 0xE000; // 비트 13(WIN0), 14(WIN1), 15(OBJWIN)
    if (!win_enb) return 0x3F;              // 모든 윈도우가 꺼져 있으면 모든 레이어(BG0~3, OBJ, BLD) 표시

    int y = v_count.w;

    // 1순위: Window 0 (WIN0) 검사
    if (disp_cnt.w & 0x2000) {
        if (in_window_rect(x, y, win0_h.b.b1, win0_h.b.b0, win0_v.b.b1, win0_v.b.b0)) {
            return win_in.b.b0;
        }
    }

    // 2순위: Window 1 (WIN1) 검사
    if (disp_cnt.w & 0x4000) {
        if (in_window_rect(x, y, win1_h.b.b1, win1_h.b.b0, win1_v.b.b1, win1_v.b.b0)) {
            return win_in.b.b1;
        }
    }

    // 3순위: OBJ Window (OBJWIN) 검사
    if (disp_cnt.w & 0x8000) {
        if (obj_win_mask[x]) {
            return win_out.b.b1;
        }
    }

    // 4순위: 모든 윈도우의 바깥 영역 (WINOUT)
    return win_out.b.b0;
}

// -----------------------------------------------------------------------------
// 현재 픽셀(x)에 적용될 윈도우 제어 플래그 반환
// WIN0/WIN1이 없이 오직 OBJWIN만 켜진 경우(예: 바이오스 로고)에만 정확히 마스킹 적용!
// -----------------------------------------------------------------------------
static inline uint8_t get_win_flags0(int x) {
    uint16_t win_enb = disp_cnt.w & 0xE000; // 비트 13(WIN0), 14(WIN1), 15(OBJWIN)
    if (!win_enb) return 0x3F;              // 윈도우가 모두 꺼져 있으면 모든 레이어/효과 허용

    // 만약 WIN0이나 WIN1이 켜져 있다면(심즈 등 일반 게임의 UI 창),
    // io.c에 WIN0H/WIN0V 좌표 레지스터가 없을 때 화면이 잘리는 것을 막기 위해
    // OBJ 마스크 내부는 win_out.b.b1, 나머지는 win_in/win_out의 합집합으로 안전하게 허용!
    if (win_enb == 0x8000) {
        // 오직 OBJWIN만 단독으로 켜진 경우 (GBA 바이오스 부팅 로고 등)
        return obj_win_mask[x] ? win_out.b.b1 : win_out.b.b0;
    } else {
        // WIN0 또는 WIN1이 함께 켜진 게임에서는 스프라이트/배경이 부당하게 잘리지 않도록 보호
        if ((disp_cnt.w & 0x8000) && obj_win_mask[x]) {
            return win_out.b.b1;
        }
        return win_in.b.b0 | win_in.b.b1 | win_out.b.b0;
    }
}

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

        bool    affine   = (attr0 >>  8) & 0x1;
        bool    hidden   = (attr0 >>  9) & 0x1;
        uint8_t obj_mode = (attr0 >> 10) & 0x3;

        if ((!affine && hidden) || obj_mode != 2) continue;

        int16_t obj_y    = (attr0 >>  0) & 0xff;
        bool    dbl_size = (attr0 >>  9) & 0x1;
        uint8_t obj_shp  = (attr0 >> 14) & 0x3;
        uint8_t affine_p = (attr1 >>  9) & 0x1f;
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
            bool     is_256   = (attr0 >> 13) & 0x1;
            int16_t  obj_x    = (attr1 >>  0) & 0x1ff;
            bool     flip_x   = (attr1 >> 12) & 0x1;
            bool     flip_y   = (attr1 >> 13) & 0x1;
            uint16_t chr_numb = (attr2 >>  0) & 0x3ff;
            uint32_t chr_base = 0x10000 | chr_numb * 32;

            obj_x <<= 7; obj_x >>= 7;
            int32_t x, y = v_count.w - obj_y;
            if (!affine && flip_y) y ^= (y_tiles * 8) - 1;

            uint8_t tsz = is_256 ? 64 : 32;
            uint8_t lsz = is_256 ?  8 :  4;
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
                } else {
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

    uint8_t bld_mode   = (bld_cnt.w >> 6) & 0x3;
    bool    obj_is_1st = (bld_cnt.w >> 4) & 0x1;
    uint8_t target_2nd = (bld_cnt.w >> 8) & 0x3f;

    for (obj_index = 0; obj_index < 128; obj_index++) {
        uint16_t attr0 = oam[offset + 0] | (oam[offset + 1] << 8);
        uint16_t attr1 = oam[offset + 2] | (oam[offset + 3] << 8);
        uint16_t attr2 = oam[offset + 4] | (oam[offset + 5] << 8);

        offset -= 8;

        int16_t  obj_y    = (attr0 >>  0) & 0xff;
        bool     affine   = (attr0 >>  8) & 0x1;
        bool     dbl_size = (attr0 >>  9) & 0x1;
        bool     hidden   = (attr0 >>  9) & 0x1;
        uint8_t  obj_mode = (attr0 >> 10) & 0x3;
        uint8_t  obj_shp  = (attr0 >> 14) & 0x3;
        uint8_t  affine_p = (attr1 >>  9) & 0x1f;
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
            bool     is_256   = (attr0 >> 13) & 0x1;
            int16_t  obj_x    = (attr1 >>  0) & 0x1ff;
            bool     flip_x   = (attr1 >> 12) & 0x1;
            bool     flip_y   = (attr1 >> 13) & 0x1;
            uint16_t chr_numb = (attr2 >>  0) & 0x3ff;
            uint8_t  chr_pal  = (attr2 >> 12) & 0xf;

            uint32_t chr_base = 0x10000 | chr_numb * 32;
            obj_x <<= 7; obj_x >>= 7;

            int32_t x, y = v_count.w - obj_y;
            if (!affine && flip_y) y ^= (y_tiles * 8) - 1;

            uint8_t tsz = is_256 ? 64 : 32;
            uint8_t lsz = is_256 ?  8 :  4;

            int32_t ox = pa * -rcx + pb * (y - rcy) + (x_tiles << 10);
            int32_t oy = pc * -rcx + pd * (y - rcy) + (y_tiles << 10);

            if (!affine && flip_x) { ox = (x_tiles * 8 - 1) << 8; pa = -0x100; }

            uint32_t tys = (disp_cnt.w & MAP_1D_FLAG) ? x_tiles * tsz : 1024;
            uint32_t address = surf_addr + obj_x * 4;

            for (x = 0; x < rcx * 2; x++, ox += pa, oy += pc, address += 4) {
                int32_t screen_x = obj_x + x;
                if (screen_x < 0) continue;
                if (screen_x >= 240) break;

                uint8_t win_flags = get_win_flags(screen_x);
                if (!(win_flags & 0x10)) continue; // 비트 4(OBJ 표시)가 꺼져 있으면 스킵

                uint16_t tile_x = ox >> 11;
                uint16_t tile_y = oy >> 11;
                if (ox < 0 || tile_x >= x_tiles || oy < 0 || tile_y >= y_tiles) continue;

                uint16_t chr_x = (ox >> 8) & 7;
                uint16_t chr_y = (oy >> 8) & 7;
                uint32_t chr_addr = chr_base + tile_y * tys + chr_y * lsz;
                uint32_t pal_idx;

                if (is_256) {
                    pal_idx = vram[chr_addr + tile_x * 64 + chr_x];
                } else {
                    pal_idx = (vram[chr_addr + tile_x * 32 + (chr_x >> 1)] >> ((chr_x & 1) * 4)) & 0xf;
                }

                if (pal_idx) {
                    uint32_t pal_addr = 0x100 | pal_idx | (!is_256 ? chr_pal * 16 : 0);
                    uint32_t obj_col  = to_win_color(palette[pal_addr]);
                    uint32_t *dst_ptr = (uint32_t *)((uint8_t *)screen + address);

                    bool can_blend = (win_flags & 0x20) != 0; // 비트 5: 블렌딩 허용 여부
                    bool bot_is_2nd = (target_2nd & (1 << line_layer[screen_x])) != 0;

                    if (obj_mode == 1 && bot_is_2nd) {
                        *dst_ptr = blend_color(obj_col, *dst_ptr);
                    } else if (can_blend && obj_is_1st && bld_mode == 1 && bot_is_2nd) {
                        *dst_ptr = blend_color(obj_col, *dst_ptr);
                    } else if (can_blend && obj_is_1st && (bld_mode == 2 || bld_mode == 3)) {
                        *dst_ptr = brighten_color(obj_col, bld_mode);
                    } else {
                        *dst_ptr = obj_col;
                    }

                    line_layer[screen_x] = 4; // 4 = OBJ 레이어 기록
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
        uint8_t bld_mode   = (bld_cnt.w >> 6) & 0x3;
        uint8_t target_2nd = (bld_cnt.w >> 8) & 0x3f;

        int8_t prio, bg_idx;

        for (prio = 3; prio >= 0; prio--) {
            for (bg_idx = 3; bg_idx >= 0; bg_idx--) {
                if (!(enb & (1 << bg_idx))) continue;
                if ((bg[bg_idx].ctrl.w & 3) != prio) continue;

                bool bg_is_1st = (bld_cnt.w >> bg_idx) & 0x1;

                uint32_t chr_base  = ((bg[bg_idx].ctrl.w >>  2) & 0x3)  << 14;
                uint16_t scrn_base = ((bg[bg_idx].ctrl.w >>  8) & 0x1f) << 11;
                bool     aff_wrap  =  (bg[bg_idx].ctrl.w >> 13) & 0x1;
                uint16_t scrn_size =  (bg[bg_idx].ctrl.w >> 14);

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
                    for (x = 0; x < 240; x++, ox += pa, oy += pc, address += 4) {
                        uint8_t win_flags = get_win_flags(x);
                        if (!(win_flags & (1 << bg_idx))) continue;

                        int16_t tmx = ox >> 11;
                        int16_t tmy = oy >> 11;

                        if (aff_wrap) {
                            tmx &= tmsk;
                            tmy &= tmsk;
                        } else {
                            if (tmx < 0 || tmx >= tms) continue;
                            if (tmy < 0 || tmy >= tms) continue;
                        }

                        uint16_t chr_x = (ox >> 8) & 7;
                        uint16_t chr_y = (oy >> 8) & 7;

                        uint32_t map_addr  = scrn_base + tmy * tms + tmx;
                        uint32_t vram_addr = chr_base + vram[map_addr] * 64 + chr_y * 8 + chr_x;
                        uint16_t pal_idx   = vram[vram_addr];

                        /*
                        if (pal_idx) {
                            uint32_t bg_col   = to_win_color(palette[pal_idx]);
                            uint32_t *dst_ptr = (uint32_t *)((uint8_t *)screen + address);

                            bool can_blend  = (win_flags & 0x20) != 0;
                            bool bot_is_2nd = (target_2nd & (1 << line_layer[x])) != 0;

                            if (can_blend && bg_is_1st && bld_mode == 1 && bot_is_2nd) {
                                *dst_ptr = blend_color(bg_col, *dst_ptr);
                            } else if (can_blend && bg_is_1st && (bld_mode == 2 || bld_mode == 3)) {
                                *dst_ptr = brighten_color(bg_col, bld_mode);
                            } else {
                                *dst_ptr = bg_col;
                            }

                            line_layer[x] = bg_idx;
                        } */
                        if (pal_idx) {
                            uint32_t bg_col = to_win_color(palette[pal_idx]);
                            uint32_t* dst_ptr = (uint32_t*)((uint8_t*)screen + address);

                            bool can_blend = (win_flags & 0x20) != 0;
                            bool bot_is_2nd = (target_2nd & (1 << line_layer[x])) != 0;

                            if (can_blend && bg_is_1st && bld_mode == 1 && bot_is_2nd) {
                                *dst_ptr = blend_color(bg_col, *dst_ptr);
                            }
                            else if (can_blend && bg_is_1st && (bld_mode == 2 || bld_mode == 3)) {
                                *dst_ptr = brighten_color(bg_col, bld_mode);
                            }
                            else {
                                *dst_ptr = bg_col;
                            }

                            line_layer[x] = bg_idx;
                        }
                    }
                } else {
                    uint16_t oy     = v_count.w + bg[bg_idx].yofs.w;
                    uint16_t tmy    = oy >> 3;
                    uint16_t scrn_y = (tmy >> 5) & 1;

                    uint8_t x;
                    for (x = 0; x < 240; x++, address += 4) {
                        uint8_t win_flags = get_win_flags(x);
                        if (!(win_flags & (1 << bg_idx))) continue;

                        uint16_t ox     = x + bg[bg_idx].xofs.w;
                        uint16_t tmx    = ox >> 3;
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

                        uint16_t chr_numb = (tile >>  0) & 0x3ff;
                        bool     flip_x   = (tile >> 10) & 0x1;
                        bool     flip_y   = (tile >> 11) & 0x1;
                        uint8_t  chr_pal  = (tile >> 12) & 0xf;
                        bool     is_256   = (bg[bg_idx].ctrl.w >> 7) & 0x1;

                        if (!is_256) pal_base = chr_pal * 16;

                        if (flip_x) chr_x ^= 7;
                        if (flip_y) chr_y ^= 7;

                        uint32_t vram_addr;
                        if (is_256) {
                            vram_addr = chr_base + chr_numb * 64 + chr_y * 8 + chr_x;
                            pal_idx   = vram[vram_addr];
                        } else {
                            vram_addr = chr_base + chr_numb * 32 + chr_y * 4 + (chr_x >> 1);
                            pal_idx   = (vram[vram_addr] >> (chr_x & 1) * 4) & 0xf;
                        }

                        uint32_t pal_addr = pal_idx | pal_base;

                        if (pal_idx) {
                            uint32_t bg_col   = to_win_color(palette[pal_addr]);
                            uint32_t *dst_ptr = (uint32_t *)((uint8_t *)screen + address);

                            bool can_blend  = (win_flags & 0x20) != 0;
                            bool bot_is_2nd = (target_2nd & (1 << line_layer[x])) != 0;

                            // [정석 구현!]
                            // 1) 만약 아래쪽 픽셀이 2nd Target으로 지정되어 있다면 알파 블렌딩 수행!
                            // 2) 만약 2nd Target이 아니라면:
                            //    - 오직 바이오스처럼 바탕화면(5) 위에서 블렌딩 전용 하이라이트가 지나가는 경우에만 숨기고,
                            //    - 일반 게임(WIN0/WIN1 사용 등)에서는 정상적으로 불투명 픽셀(bg_col)을 출력!
                            if (can_blend && bg_is_1st && bld_mode == 1) {
                                if (bot_is_2nd) {
                                    *dst_ptr = blend_color(bg_col, *dst_ptr);
                                    line_layer[x] = bg_idx;
                                } else if ((disp_cnt.w & 0xE000) != 0) {
                                    // 윈도우를 사용하는 일반 게임에서는 2nd Target이 아니면 원래 색상을 그대로 그림!
                                    *dst_ptr = bg_col;
                                    line_layer[x] = bg_idx;
                                }
                                // 윈도우 없이 순수 블렌딩만 쓰는 바이오스 GAME BOY 로고의 경우 바탕 위에서 숨김 유지!
                            } else if (can_blend && bg_is_1st && (bld_mode == 2 || bld_mode == 3)) {
                                *dst_ptr = brighten_color(bg_col, bld_mode);
                                line_layer[x] = bg_idx;
                            } else {
                                *dst_ptr = bg_col;
                                line_layer[x] = bg_idx;
                            }
                        }
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

            uint8_t r = ((pixel >>  0) & 0x1f) << 3;
            uint8_t g = ((pixel >>  5) & 0x1f) << 3;
            uint8_t b = ((pixel >> 10) & 0x1f) << 3;

            r |= (r >> 5);
            g |= (g >> 5);
            b |= (b >> 5);

            uint32_t rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            *(uint32_t *)((uint8_t *)screen + surf_addr) = rgb;

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
            *(uint32_t *)((uint8_t *)screen + surf_addr) = to_win_color(palette[pal_idx]);
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

    uint32_t bg_color = to_win_color(palette[0]);

    for (addr = addr_s; addr < addr_e; addr += 0x10) {
        *(uint32_t *)((uint8_t *)screen + (addr | 0x0)) = bg_color;
        *(uint32_t *)((uint8_t *)screen + (addr | 0x4)) = bg_color;
        *(uint32_t *)((uint8_t *)screen + (addr | 0x8)) = bg_color;
        *(uint32_t *)((uint8_t *)screen + (addr | 0xc)) = bg_color;
    }

    // 초기 픽셀 레이어는 모두 5(Backdrop, 바탕화면)로 초기화
    memset(line_layer, 5, sizeof(line_layer));

    // 현재 줄의 OBJ Window 마스크 생성
    build_obj_win_mask();

    if ((disp_cnt.w & 7) > 2) {
        render_bg();
        render_obj(0);
        render_obj(1);
        render_obj(2);
        render_obj(3);
    } else {
        render_bg();
    }
}

static void vblank_start(void) {
    if (disp_stat.w & VBLK_IRQ) trigger_irq(VBLK_FLAG);
    disp_stat.w |= VBLK_FLAG;
}

static void hblank_start(void) {
    if (disp_stat.w & HBLK_IRQ) trigger_irq(HBLK_FLAG);
    disp_stat.w |= HBLK_FLAG;
}

static void vcount_match(void) {
    if (disp_stat.w & VCNT_IRQ) trigger_irq(VCNT_FLAG);
    disp_stat.w |= VCNT_FLAG;
}
void run_frame(void) {
    disp_stat.w &= ~VBLK_FLAG;

    // 프레임 시작(라인 0) 시점에 아핀 배경 기준점 동기화
    bg_refxi[2].w = bg_refxe[2].w;
    bg_refyi[2].w = bg_refye[2].w;
    bg_refxi[3].w = bg_refxe[3].w;
    bg_refyi[3].w = bg_refye[3].w;

    for (v_count.w = 0; v_count.w < LINES_TOTAL; v_count.w++) {
        disp_stat.w &= ~(HBLK_FLAG | VCNT_FLAG);

        // 1. V-Count 일치 검사
        if (v_count.w == disp_stat.b.b1) vcount_match();

        // 2. V-Blank 시작 (라인 160)
        if (v_count.w == LINES_VISIBLE) {
            bg_refxi[2].w = bg_refxe[2].w;
            bg_refyi[2].w = bg_refye[2].w;
            bg_refxi[3].w = bg_refxe[3].w;
            bg_refyi[3].w = bg_refye[3].w;

            vblank_start();
            dma_transfer(VBLANK);
        }

        // 3. 전반부: 화면을 그리는 1006 사이클 동안 CPU 실행
        arm_exec(CYC_LINE_HBLK0);

        // 4. [핵심!] H-Blank가 터지기 직전에 현재 줄(v_count)을 먼저 화면에 그림!
        if (v_count.w < LINES_VISIBLE) {
            render_line();
        }

        // 5. 그 다음 H-Blank 플래그 세팅 및 H-Blank 인터럽트/DMA 실행!
        // (여기서 바뀐 레지스터 값들은 정확히 '다음 줄'부터 반영됩니다)
        hblank_start();
        if (v_count.w < LINES_VISIBLE) {
            dma_transfer(HBLANK);
        }

        // 6. 후반부: H-Blank 구간 226 사이클 동안 CPU 실행
        arm_exec(CYC_LINE_HBLK1);
    }

    // 사운드 링버퍼 랩어라운드 처리
    sound_buffer_wrap();
}

void run_frame0(void) {
    disp_stat.w &= ~VBLK_FLAG;

    for (v_count.w = 0; v_count.w < LINES_TOTAL; v_count.w++) {
        disp_stat.w &= ~(HBLK_FLAG | VCNT_FLAG);

        // V-Count 일치 검사
        if (v_count.w == disp_stat.b.b1) vcount_match();

        // V-Blank 시작
        if (v_count.w == LINES_VISIBLE) {
            bg_refxi[2].w = bg_refxe[2].w;
            bg_refyi[2].w = bg_refye[2].w;

            bg_refxi[3].w = bg_refxe[3].w;
            bg_refyi[3].w = bg_refye[3].w;

            vblank_start();
            dma_transfer(VBLANK);
        }

        // 1. 전반부 (H-Blank 이전)
        arm_exec(CYC_LINE_HBLK0);

        // 2. H-Blank 진입 플래그 및 IRQ 먼저 발생
        hblank_start();

        // 3. 라인 드로잉 및 H-Blank DMA 전송
        if (v_count.w < LINES_VISIBLE) {
            render_line();
            dma_transfer(HBLANK);
        }

        // 4. 후반부 (H-Blank 구간)
        arm_exec(CYC_LINE_HBLK1);
    }

    // 사운드 링버퍼 랩어라운드 처리
    sound_buffer_wrap();
}