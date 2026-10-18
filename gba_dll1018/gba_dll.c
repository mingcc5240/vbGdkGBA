#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "arm.h"
#include "arm_mem.h"
#include "io.h"
#include "sound.h"
#include "video.h"
#include "timer.h"

// xxd 또는 스크립트로 생성한 내장 바이오스 헤더 포함
#include "gba_bios.h"

#define GBA_WIDTH       240
#define GBA_HEIGHT      160
#define MAX_ROM_SIZE    (32 * 1024 * 1024)
#define STATE_MAGIC 0x54534247
#define STATE_VER   1

// video.c에서 렌더링하는 32비트 프레임 버퍼 참조
extern uint32_t gba_screen[GBA_WIDTH * GBA_HEIGHT];

extern uint32_t gba_screen_2x[480 * 320];
extern uint32_t* screen_2x_ptr;

extern uint32_t gba_screen_3x[720 * 480];
extern uint32_t* screen_3x_ptr;
extern int video_filter_mode;
extern uint32_t snd_cur_play;
extern uint32_t snd_cur_write;
//extern void apply_scale2x(void);
extern void apply_scale3x(void);

// VB6 메뉴에서 호출할 비디오 필터 변경 함수 (mode: 0 = 원본, 1 = Scale2x)
__declspec(dllexport) void __stdcall GBA_SetVideoFilter(int mode) {
    video_filter_mode = mode;
    apply_scale3x(); // 일시정지 상태에서도 즉시 화면에 반영되도록 1회 호출
}

// 방법 A: VB6가 만든 CreateDIBSection의 메모리 주소(pDIBBits)를 DLL에 등록!
// (이 함수를 한 번만 호출해 두면 RtlMoveMemory조차 필요 없이 자동으로 DIB에 그려집니다!)
__declspec(dllexport) void __stdcall GBA_SetScreenBuffer3X(void* dib_bits_ptr) {
    if (dib_bits_ptr != NULL) {
        screen_3x_ptr = (uint32_t*)dib_bits_ptr;
    }
    else {
        screen_3x_ptr = gba_screen_3x;
    }
}

extern uint32_t eeprom_addr;

// GBA 하드웨어 메모리 규격 크기 정의 (gdkGBA의 arm_mem.c 할당 크기 기준)
#define SZ_WRAM   0x40000 // 256 KB (EWRAM)
#define SZ_IWRAM  0x08000 // 32 KB  (IWRAM)
#define SZ_PRAM   0x00400 // 1 KB   (Palette RAM)
#define SZ_VRAM   0x18000 // 96 KB  (VRAM - 만약 arm_mem.c에서 0x20000으로 할당했다면 0x20000으로 수정)
#define SZ_OAM    0x00400 // 1 KB   (OAM)
#define SZ_SRAM   0x10000 // 64 KB  (SRAM)
#define SZ_FLASH  0x20000 // 128 KB (Flash)
#define SZ_EEPROM 0x02000 // 8 KB   (EEPROM)

// -----------------------------------------------------------------------------
// 1. 현재 게임 상태 저장하기 (Save State) - 성공 시 1, 실패 시 0 반환
// -----------------------------------------------------------------------------
__declspec(dllexport) int __stdcall GBA_SaveState(const char* state_path) {
    if (!state_path || !state_path[0]) return 0;

    FILE* fp = fopen(state_path, "wb");
    if (!fp) return 0;

    uint32_t magic = STATE_MAGIC;
    uint32_t ver = STATE_VER + 1; // 포맷 변경으로 인한 버전 구분
    fwrite(&magic, sizeof(uint32_t), 1, fp);
    fwrite(&ver, sizeof(uint32_t), 1, fp);

    // [1] CPU 상태 (레지스터, 파이프라인, 사이클, BIOS 보호 상태)
    fwrite(&arm_r, sizeof(arm_r), 1, fp);
    fwrite(arm_pipe, sizeof(arm_pipe), 1, fp);
    fwrite(&arm_op, sizeof(arm_op), 1, fp);
    fwrite(&arm_cycles, sizeof(arm_cycles), 1, fp);
    fwrite(&pipe_reload, sizeof(pipe_reload), 1, fp);
    fwrite(&bios_op, sizeof(bios_op), 1, fp);

    // [2] 메모리 영역 (포인터이므로 sizeof 대신 실제 바이트 크기를 직접 지정해야 함!)
    if (wram)  fwrite(wram, SZ_WRAM, 1, fp);
    if (iwram) fwrite(iwram, SZ_IWRAM, 1, fp);
    if (pram)  fwrite(pram, SZ_PRAM, 1, fp); // 누락되었던 PRAM 추가!
    if (vram)  fwrite(vram, SZ_VRAM, 1, fp);
    if (oam)   fwrite(oam, SZ_OAM, 1, fp);
    fwrite(palette, sizeof(palette), 1, fp);   // palette는 uint32_t[0x200] 배열이므로 sizeof 정상 동작

    // [3] 카트리지 세이브 메모리 및 Flash/EEPROM 컨트롤 상태
    if (sram)   fwrite(sram, SZ_SRAM, 1, fp);
    if (flash)  fwrite(flash, SZ_FLASH, 1, fp);
    if (eeprom) fwrite(eeprom, SZ_EEPROM, 1, fp);
    fwrite(&eeprom_idx, sizeof(eeprom_idx), 1, fp);
    fwrite(&eeprom_addr, sizeof(eeprom_addr), 1, fp);

    // [4] PPU / IO 레지스터 상태
    fwrite(&disp_cnt, sizeof(disp_cnt), 1, fp);
    fwrite(&green_inv, sizeof(green_inv), 1, fp);
    fwrite(&disp_stat, sizeof(disp_stat), 1, fp);
    fwrite(&v_count, sizeof(v_count), 1, fp);
    fwrite(bg, sizeof(bg), 1, fp);
    fwrite(bg_pa, sizeof(bg_pa), 1, fp);
    fwrite(bg_pb, sizeof(bg_pb), 1, fp);
    fwrite(bg_pc, sizeof(bg_pc), 1, fp);
    fwrite(bg_pd, sizeof(bg_pd), 1, fp);
    fwrite(bg_refxe, sizeof(bg_refxe), 1, fp);
    fwrite(bg_refye, sizeof(bg_refye), 1, fp);
    fwrite(bg_refxi, sizeof(bg_refxi), 1, fp);
    fwrite(bg_refyi, sizeof(bg_refyi), 1, fp);
    fwrite(&win0_h, sizeof(win0_h), 1, fp);
    fwrite(&win1_h, sizeof(win1_h), 1, fp);
    fwrite(&win0_v, sizeof(win0_v), 1, fp);
    fwrite(&win1_v, sizeof(win1_v), 1, fp);
    fwrite(&win_in, sizeof(win_in), 1, fp);
    fwrite(&win_out, sizeof(win_out), 1, fp);
    fwrite(&bld_cnt, sizeof(bld_cnt), 1, fp);
    fwrite(&bld_alpha, sizeof(bld_alpha), 1, fp);
    fwrite(&bld_bright, sizeof(bld_bright), 1, fp);

    // [5] 인터럽트 및 시스템 제어 레지스터
    fwrite(&int_enb, sizeof(int_enb), 1, fp);
    fwrite(&int_ack, sizeof(int_ack), 1, fp);
    fwrite(&wait_cnt, sizeof(wait_cnt), 1, fp);
    fwrite(&int_enb_m, sizeof(int_enb_m), 1, fp);
    fwrite(&post_boot, sizeof(post_boot), 1, fp);
    fwrite(&int_halt, sizeof(int_halt), 1, fp);

    // [6] DMA 및 타이머 상태
    fwrite(dma_ch, sizeof(dma_ch), 1, fp);
    // (참고) 만약 dma_dst_addr, dma_src_addr, dma_count가 extern 접근 가능하다면 주석을 꼭 해제하세요!
    // fwrite(dma_dst_addr, sizeof(dma_dst_addr), 1, fp);
    // fwrite(dma_src_addr, sizeof(dma_src_addr), 1, fp);
    // fwrite(dma_count,    sizeof(dma_count),    1, fp);
    fwrite(tmr, sizeof(tmr), 1, fp);
    fwrite(tmr_icnt, sizeof(tmr_icnt), 1, fp);
    fwrite(&tmr_enb, sizeof(tmr_enb), 1, fp);

    // [7] 사운드 채널 및 FIFO 레지스터 상태
    fwrite(sqr_ch, sizeof(sqr_ch), 1, fp);
    fwrite(&wave_ch, sizeof(wave_ch), 1, fp);
    fwrite(&noise_ch, sizeof(noise_ch), 1, fp);
    fwrite(wave_ram, sizeof(wave_ram), 1, fp);
    fwrite(&snd_psg_vol, sizeof(snd_psg_vol), 1, fp);
    fwrite(&snd_pcm_vol, sizeof(snd_pcm_vol), 1, fp);
    fwrite(&snd_psg_enb, sizeof(snd_psg_enb), 1, fp);
    fwrite(&snd_bias, sizeof(snd_bias), 1, fp);
    fwrite(snd_ch_state, sizeof(snd_ch_state), 1, fp);

    // [8] 현재 그려진 화면 프레임버퍼도 함께 저장
    // 주의: gba_screen도 포인터라면 sizeof(gba_screen) 대신 (240 * 160 * sizeof(uint32_t))를 써야 합니다!
    fwrite(gba_screen, 240 * 160 * sizeof(uint32_t), 1, fp);

    fclose(fp);
    return 1;
}

// -----------------------------------------------------------------------------
// 2. 저장된 게임 상태 그대로 불러오기 (Load State) - 성공 시 1, 실패 시 0 반환
// -----------------------------------------------------------------------------
__declspec(dllexport) int __stdcall GBA_LoadState(const char* state_path) {
    if (!state_path || !state_path[0]) return 0;

    FILE* fp = fopen(state_path, "rb");
    if (!fp) return 0;

    uint32_t magic = 0, ver = 0;
    fread(&magic, sizeof(uint32_t), 1, fp);
    fread(&ver, sizeof(uint32_t), 1, fp);

    // 파일 포맷 검증
    if (magic != STATE_MAGIC || ver != (STATE_VER + 1)) {
        fclose(fp);
        return 0;
    }

    // [1] CPU 상태 복원
    fread(&arm_r, sizeof(arm_r), 1, fp);
    fread(arm_pipe, sizeof(arm_pipe), 1, fp);
    fread(&arm_op, sizeof(arm_op), 1, fp);
    fread(&arm_cycles, sizeof(arm_cycles), 1, fp);
    fread(&pipe_reload, sizeof(pipe_reload), 1, fp);
    fread(&bios_op, sizeof(bios_op), 1, fp);

    // [2] 메모리 영역 복원 (실제 바이트 크기만큼 정확히 읽어옴)
    if (wram)  fread(wram, SZ_WRAM, 1, fp);
    if (iwram) fread(iwram, SZ_IWRAM, 1, fp);
    if (pram)  fread(pram, SZ_PRAM, 1, fp);
    if (vram)  fread(vram, SZ_VRAM, 1, fp);
    if (oam)   fread(oam, SZ_OAM, 1, fp);
    fread(palette, sizeof(palette), 1, fp);

    // [3] 카트리지 세이브 메모리 및 상태 복원
    if (sram)   fread(sram, SZ_SRAM, 1, fp);
    if (flash)  fread(flash, SZ_FLASH, 1, fp);
    if (eeprom) fread(eeprom, SZ_EEPROM, 1, fp);
    fread(&eeprom_idx, sizeof(eeprom_idx), 1, fp);
    fread(&eeprom_addr, sizeof(eeprom_addr), 1, fp);

    // [4] PPU / IO 레지스터 복원
    fread(&disp_cnt, sizeof(disp_cnt), 1, fp);
    fread(&green_inv, sizeof(green_inv), 1, fp);
    fread(&disp_stat, sizeof(disp_stat), 1, fp);
    fread(&v_count, sizeof(v_count), 1, fp);
    fread(bg, sizeof(bg), 1, fp);
    fread(bg_pa, sizeof(bg_pa), 1, fp);
    fread(bg_pb, sizeof(bg_pb), 1, fp);
    fread(bg_pc, sizeof(bg_pc), 1, fp);
    fread(bg_pd, sizeof(bg_pd), 1, fp);
    fread(bg_refxe, sizeof(bg_refxe), 1, fp);
    fread(bg_refye, sizeof(bg_refye), 1, fp);
    fread(bg_refxi, sizeof(bg_refxi), 1, fp);
    fread(bg_refyi, sizeof(bg_refyi), 1, fp);
    fread(&win0_h, sizeof(win0_h), 1, fp);
    fread(&win1_h, sizeof(win1_h), 1, fp);
    fread(&win0_v, sizeof(win0_v), 1, fp);
    fread(&win1_v, sizeof(win1_v), 1, fp);
    fread(&win_in, sizeof(win_in), 1, fp);
    fread(&win_out, sizeof(win_out), 1, fp);
    fread(&bld_cnt, sizeof(bld_cnt), 1, fp);
    fread(&bld_alpha, sizeof(bld_alpha), 1, fp);
    fread(&bld_bright, sizeof(bld_bright), 1, fp);

    // [5] 인터럽트 및 시스템 제어 레지스터 복원
    fread(&int_enb, sizeof(int_enb), 1, fp);
    fread(&int_ack, sizeof(int_ack), 1, fp);
    fread(&wait_cnt, sizeof(wait_cnt), 1, fp);
    fread(&int_enb_m, sizeof(int_enb_m), 1, fp);
    fread(&post_boot, sizeof(post_boot), 1, fp);
    fread(&int_halt, sizeof(int_halt), 1, fp);

    // [6] DMA 및 타이머 복원
    fread(dma_ch, sizeof(dma_ch), 1, fp);
    // fwrite(dma_dst_addr, sizeof(dma_dst_addr), 1, fp);
    // fwrite(dma_src_addr, sizeof(dma_src_addr), 1, fp);
    // fwrite(dma_count,    sizeof(dma_count),    1, fp);
    fread(tmr, sizeof(tmr), 1, fp);
    fread(tmr_icnt, sizeof(tmr_icnt), 1, fp);
    fread(&tmr_enb, sizeof(tmr_enb), 1, fp);

    // [7] 사운드 채널 상태 복원
    fread(sqr_ch, sizeof(sqr_ch), 1, fp);
    fread(&wave_ch, sizeof(wave_ch), 1, fp);
    fread(&noise_ch, sizeof(noise_ch), 1, fp);
    fread(wave_ram, sizeof(wave_ram), 1, fp);
    fread(&snd_psg_vol, sizeof(snd_psg_vol), 1, fp);
    fread(&snd_pcm_vol, sizeof(snd_pcm_vol), 1, fp);
    fread(&snd_psg_enb, sizeof(snd_psg_enb), 1, fp);
    fread(&snd_bias, sizeof(snd_bias), 1, fp);
    fread(snd_ch_state, sizeof(snd_ch_state), 1, fp);

    // [8] 화면 버퍼 복원
    fread(gba_screen, 240 * 160 * sizeof(uint32_t), 1, fp);

    fclose(fp);

    // [9] 후처리: DMA 내부 주소가 저장되지 않았을 경우를 대비한 안전 복구!
    // (만약 위 [6]번에서 dma_src_addr 주석을 풀고 직접 저장/복원한다면 아래 for문은 빼셔도 됩니다.)
   // for (int i = 0; i < 4; i++) {
   //     dma_src_addr[i] = dma_ch[i].src.w;
   //     dma_dst_addr[i] = dma_ch[i].dst.w;
   //     dma_count[i] = dma_ch[i].count.w;
   // }

    // 메모리 웨이트스테이트 테이블 갱신, 사운드 큐 정리, 비디오 필터 즉시 반영
    update_ws();
    fifo_a_len = 0;
    fifo_b_len = 0;
    snd_cur_play = snd_cur_write; // 사운드 링버퍼 지연 초기화 (노이즈 방지)
    apply_scale3x();

    return 1;
}
/////////

// -----------------------------------------------------------------------------
// 1. 현재 게임 상태 통째로 저장 (Save State) - 성공 시 1, 실패 시 0 반환
// -----------------------------------------------------------------------------
__declspec(dllexport) int __stdcall GBA_SaveState0(const char* state_path) {
    if (!state_path || !state_path[0]) return 0;

    FILE* fp = fopen(state_path, "wb");
    if (!fp) return 0;

    uint32_t magic = STATE_MAGIC;
    uint32_t ver = STATE_VER;
    fwrite(&magic, sizeof(uint32_t), 1, fp);
    fwrite(&ver, sizeof(uint32_t), 1, fp);

    // [1] CPU 상태 (레지스터, 파이프라인, 사이클)
    fwrite(&arm_r, sizeof(arm_r), 1, fp);
    fwrite(arm_pipe, sizeof(arm_pipe), 1, fp);
    fwrite(&arm_op, sizeof(arm_op), 1, fp);
    fwrite(&arm_cycles, sizeof(arm_cycles), 1, fp);

    // [2] 메모리 영역 (WRAM 256KB, IWRAM 32KB, VRAM 96KB, OAM 1KB, Palette 1KB)
    fwrite(wram, sizeof(wram), 1, fp);
    fwrite(iwram, sizeof(iwram), 1, fp);
    fwrite(vram, sizeof(vram), 1, fp);
    fwrite(oam, sizeof(oam), 1, fp);
    fwrite(palette, sizeof(palette), 1, fp);
    fwrite(sram, sizeof(sram), 1, fp);

    // [3] 카트리지 세이브 메모리 및 Flash/EEPROM 컨트롤 상태
    fwrite(flash, sizeof(flash), 1, fp);
    fwrite(eeprom, sizeof(eeprom), 1, fp);
    fwrite(&eeprom_idx, sizeof(eeprom_idx), 1, fp);
    fwrite(&eeprom_addr, sizeof(eeprom_addr), 1, fp);
   
    // [4] PPU / IO 레지스터 상태
    fwrite(&disp_cnt, sizeof(disp_cnt), 1, fp);
    fwrite(&green_inv, sizeof(green_inv), 1, fp);
    fwrite(&disp_stat, sizeof(disp_stat), 1, fp);
    fwrite(&v_count, sizeof(v_count), 1, fp);
    fwrite(bg, sizeof(bg), 1, fp);
    fwrite(bg_pa, sizeof(bg_pa), 1, fp);
    fwrite(bg_pb, sizeof(bg_pb), 1, fp);
    fwrite(bg_pc, sizeof(bg_pc), 1, fp);
    fwrite(bg_pd, sizeof(bg_pd), 1, fp);
    fwrite(bg_refxe, sizeof(bg_refxe), 1, fp);
    fwrite(bg_refye, sizeof(bg_refye), 1, fp);
    fwrite(bg_refxi, sizeof(bg_refxi), 1, fp);
    fwrite(bg_refyi, sizeof(bg_refyi), 1, fp);
    fwrite(&win0_h, sizeof(win0_h), 1, fp);
    fwrite(&win1_h, sizeof(win1_h), 1, fp);
    fwrite(&win0_v, sizeof(win0_v), 1, fp);
    fwrite(&win1_v, sizeof(win1_v), 1, fp);
    fwrite(&win_in, sizeof(win_in), 1, fp);
    fwrite(&win_out, sizeof(win_out), 1, fp);
    fwrite(&bld_cnt, sizeof(bld_cnt), 1, fp);
    fwrite(&bld_alpha, sizeof(bld_alpha), 1, fp);
    fwrite(&bld_bright, sizeof(bld_bright), 1, fp);

    // [5] 인터럽트 및 시스템 제어 레지스터
    fwrite(&int_enb, sizeof(int_enb), 1, fp);
    fwrite(&int_ack, sizeof(int_ack), 1, fp);
    fwrite(&wait_cnt, sizeof(wait_cnt), 1, fp);
    fwrite(&int_enb_m, sizeof(int_enb_m), 1, fp);
    fwrite(&post_boot, sizeof(post_boot), 1, fp);
    fwrite(&int_halt, sizeof(int_halt), 1, fp);

    // [6] DMA 및 타이머 상태
    fwrite(dma_ch, sizeof(dma_ch), 1, fp);
    //fwrite(dma_dst_addr, sizeof(dma_dst_addr), 1, fp);
    //fwrite(dma_src_addr, sizeof(dma_src_addr), 1, fp);
    //fwrite(dma_count, sizeof(dma_count), 1, fp);
    fwrite(tmr, sizeof(tmr), 1, fp);
    fwrite(tmr_icnt, sizeof(tmr_icnt), 1, fp);
    fwrite(&tmr_enb, sizeof(tmr_enb), 1, fp);

    // [7] 사운드 채널 및 FIFO 레지스터 상태
    fwrite(sqr_ch, sizeof(sqr_ch), 1, fp);
    fwrite(&wave_ch, sizeof(wave_ch), 1, fp);
    fwrite(&noise_ch, sizeof(noise_ch), 1, fp);
    fwrite(wave_ram, sizeof(wave_ram), 1, fp);
    fwrite(&snd_psg_vol, sizeof(snd_psg_vol), 1, fp);
    fwrite(&snd_pcm_vol, sizeof(snd_pcm_vol), 1, fp);
    fwrite(&snd_psg_enb, sizeof(snd_psg_enb), 1, fp);
    fwrite(&snd_bias, sizeof(snd_bias), 1, fp);
    fwrite(snd_ch_state, sizeof(snd_ch_state), 1, fp);

    // [8] 현재 그려진 화면 프레임버퍼도 함께 저장 (불러오자마자 화면 즉시 표시용)
    fwrite(gba_screen, sizeof(gba_screen), 1, fp);

    fclose(fp);
    return 1;
}

// -----------------------------------------------------------------------------
// 2. 저장된 게임 상태 그대로 불러오기 (Load State) - 성공 시 1, 실패 시 0 반환
// -----------------------------------------------------------------------------
__declspec(dllexport) int __stdcall GBA_LoadState0(const char* state_path) {
    if (!state_path || !state_path[0]) return 0;

    FILE* fp = fopen(state_path, "rb");
    if (!fp) return 0;

    uint32_t magic = 0, ver = 0;
    fread(&magic, sizeof(uint32_t), 1, fp);
    fread(&ver, sizeof(uint32_t), 1, fp);

    // 파일 포맷 검증
    if (magic != STATE_MAGIC || ver != STATE_VER) {
        fclose(fp);
        return 0;
    }

    // [1] CPU 상태 복원
    fread(&arm_r, sizeof(arm_r), 1, fp);
    fread(arm_pipe, sizeof(arm_pipe), 1, fp);
    fread(&arm_op, sizeof(arm_op), 1, fp);
    fread(&arm_cycles, sizeof(arm_cycles), 1, fp);

    // [2] 메모리 영역 복원
    fread(wram, sizeof(wram), 1, fp);
    fread(iwram, sizeof(iwram), 1, fp);
    fread(vram, sizeof(vram), 1, fp);
    fread(oam, sizeof(oam), 1, fp);
    fread(palette, sizeof(palette), 1, fp);
    fread(sram, sizeof(sram), 1, fp);

    // [3] 카트리지 세이브 메모리 및 상태 복원
    fread(flash, sizeof(flash), 1, fp);
    fread(eeprom, sizeof(eeprom), 1, fp);
    fread(&eeprom_idx, sizeof(eeprom_idx), 1, fp);
    fread(&eeprom_addr, sizeof(eeprom_addr), 1, fp);
    //fread(&eeprom_mode, sizeof(eeprom_mode), 1, fp);
    //fread(&flash_mode, sizeof(flash_mode), 1, fp);
    //fread(&flash_bank, sizeof(flash_bank), 1, fp);
    //fread(&flash_id_mode, sizeof(flash_id_mode), 1, fp);

    // [4] PPU / IO 레지스터 복원
    fread(&disp_cnt, sizeof(disp_cnt), 1, fp);
    fread(&green_inv, sizeof(green_inv), 1, fp);
    fread(&disp_stat, sizeof(disp_stat), 1, fp);
    fread(&v_count, sizeof(v_count), 1, fp);
    fread(bg, sizeof(bg), 1, fp);
    fread(bg_pa, sizeof(bg_pa), 1, fp);
    fread(bg_pb, sizeof(bg_pb), 1, fp);
    fread(bg_pc, sizeof(bg_pc), 1, fp);
    fread(bg_pd, sizeof(bg_pd), 1, fp);
    fread(bg_refxe, sizeof(bg_refxe), 1, fp);
    fread(bg_refye, sizeof(bg_refye), 1, fp);
    fread(bg_refxi, sizeof(bg_refxi), 1, fp);
    fread(bg_refyi, sizeof(bg_refyi), 1, fp);
    fread(&win0_h, sizeof(win0_h), 1, fp);
    fread(&win1_h, sizeof(win1_h), 1, fp);
    fread(&win0_v, sizeof(win0_v), 1, fp);
    fread(&win1_v, sizeof(win1_v), 1, fp);
    fread(&win_in, sizeof(win_in), 1, fp);
    fread(&win_out, sizeof(win_out), 1, fp);
    fread(&bld_cnt, sizeof(bld_cnt), 1, fp);
    fread(&bld_alpha, sizeof(bld_alpha), 1, fp);
    fread(&bld_bright, sizeof(bld_bright), 1, fp);

    // [5] 인터럽트 및 시스템 제어 레지스터 복원
    fread(&int_enb, sizeof(int_enb), 1, fp);
    fread(&int_ack, sizeof(int_ack), 1, fp);
    fread(&wait_cnt, sizeof(wait_cnt), 1, fp);
    fread(&int_enb_m, sizeof(int_enb_m), 1, fp);
    fread(&post_boot, sizeof(post_boot), 1, fp);
    fread(&int_halt, sizeof(int_halt), 1, fp);

    // [6] DMA 및 타이머 복원
    fread(dma_ch, sizeof(dma_ch), 1, fp);
    //fread(dma_dst_addr, sizeof(dma_dst_addr), 1, fp);
    //fread(dma_src_addr, sizeof(dma_src_addr), 1, fp);
    //fread(dma_count, sizeof(dma_count), 1, fp);
    fread(tmr, sizeof(tmr), 1, fp);
    fread(tmr_icnt, sizeof(tmr_icnt), 1, fp);
    fread(&tmr_enb, sizeof(tmr_enb), 1, fp);

    // [7] 사운드 채널 상태 복원
    fread(sqr_ch, sizeof(sqr_ch), 1, fp);
    fread(&wave_ch, sizeof(wave_ch), 1, fp);
    fread(&noise_ch, sizeof(noise_ch), 1, fp);
    fread(wave_ram, sizeof(wave_ram), 1, fp);
    fread(&snd_psg_vol, sizeof(snd_psg_vol), 1, fp);
    fread(&snd_pcm_vol, sizeof(snd_pcm_vol), 1, fp);
    fread(&snd_psg_enb, sizeof(snd_psg_enb), 1, fp);
    fread(&snd_bias, sizeof(snd_bias), 1, fp);
    fread(snd_ch_state, sizeof(snd_ch_state), 1, fp);

    // [8] 화면 버퍼 복원
    fread(gba_screen, sizeof(gba_screen), 1, fp);

    fclose(fp);

    // [9] 후처리: 메모리 웨이트스테이트 테이블 갱신, 사운드 큐 정리, 비디오 필터 즉시 반영
    update_ws();
    fifo_a_len = 0;
    fifo_b_len = 0;
    snd_cur_play = snd_cur_write; // 사운드 링버퍼 지연 초기화 (노이즈 방지)
    apply_scale3x();

    return 1;
}

// 2의 거듭제곱 정렬 함수 (ROM 미러링 마스크 계산용)
static uint32_t to_pow2(uint32_t val) {
    val--;
    val |= (val >> 1);
    val |= (val >> 2);
    val |= (val >> 4);
    val |= (val >> 8);
    val |= (val >> 16);
    return val + 1;
}

extern int16_t snd_buffer[BUFF_SAMPLES];
extern uint32_t snd_cur_play;
extern uint32_t snd_cur_write;

// gba_dll.c 상단에 외부 포인터 선언 추가
extern uint8_t *wram;
extern uint8_t *iwram;
extern uint8_t *vram;
extern uint8_t *pram;
extern uint8_t *oam;
extern uint8_t *rom;
extern uint8_t *bios;
extern uint8_t *eeprom;
extern uint8_t *sram;
extern uint8_t *flash;

extern bool     int_halt;
extern uint32_t arm_cycles;
extern uint32_t snd_cycles;
extern int8_t   fifo_a_samp;
extern int8_t   fifo_b_samp;

static bool is_memory_allocated = false;

// -----------------------------------------------------------------------------
// 하드웨어의 모든 메모리 및 I/O 레지스터를 처음 켰을 때와 똑같이 0으로 초기화하는 내부 함수
// -----------------------------------------------------------------------------
static void hardware_clear_all(void) {
    // 1. 모든 RAM 및 세이브 메모리 영역 초기화
    if (wram)   memset(wram,   0, 0x40000);
    if (iwram)  memset(iwram,  0, 0x8000);
    if (vram)   memset(vram,   0, 0x18000);
    if (oam)    memset(oam,    0, 0x400);
    if (pram)   memset(pram,   0, 0x400);
    if (eeprom) memset(eeprom, 0xff, 0x2000);  // 세이브 메모리는 초기값이 0xFF여야 게임이 정상 인식함!
    if (sram)   memset(sram,   0xff, 0x10000);
    if (flash)  memset(flash,  0xff, 0x20000);

    memset(palette, 0, sizeof(palette));
    memset(gba_screen, 0, sizeof(gba_screen));

    // 2. CPU HALT 상태 및 사이클 카운터 초기화
    int_halt   = false;
    arm_cycles = 0;

    // 3. 인터럽트 및 시스템 제어 레지스터 초기화
    int_enb_m.w = 0;
    int_enb.w   = 0;
    int_ack.w   = 0;
    wait_cnt.w  = 0;
    post_boot   = 0;
    io_open_bus = false;
    key_input.w = 0x3ff;
    update_ws();

    // 4. LCD 및 그래픽 제어 레지스터 초기화
    disp_cnt.w   = 0;
    green_inv.w  = 0;
    disp_stat.w  = 0;
    v_count.w    = 0;
    win_in.w     = 0;
    win_out.w    = 0;
    bld_cnt.w    = 0;
    bld_alpha.w  = 0;
    bld_bright.w = 0;

    memset(bg,       0, sizeof(bg));
    memset(bg_pa,    0, sizeof(bg_pa));
    memset(bg_pb,    0, sizeof(bg_pb));
    memset(bg_pc,    0, sizeof(bg_pc));
    memset(bg_pd,    0, sizeof(bg_pd));
    memset(bg_refxe, 0, sizeof(bg_refxe));
    memset(bg_refye, 0, sizeof(bg_refye));
    memset(bg_refxi, 0, sizeof(bg_refxi));
    memset(bg_refyi, 0, sizeof(bg_refyi));

    // 5. 타이머, DMA, 시리얼 통신 레지스터 초기화
    tmr_enb      = 0;
    memset(tmr,    0, sizeof(tmr));
    memset(dma_ch, 0, sizeof(dma_ch));
    r_cnt.w      = 0;
    sio_cnt.w    = 0;
    sio_data8.w  = 0;
    sio_data32.w = 0;

    // 6. 사운드 버퍼 및 채널 상태 초기화
    snd_cycles    = 0;
    snd_cur_play  = 0;
    snd_cur_write = 2048;
    fifo_a_len    = 0;
    fifo_b_len    = 0;
    fifo_a_samp   = 0;
    fifo_b_samp   = 0;
    snd_fifo_a_0  = 0; snd_fifo_a_1 = 0; snd_fifo_a_2 = 0; snd_fifo_a_3 = 0;
    snd_fifo_b_0  = 0; snd_fifo_b_1 = 0; snd_fifo_b_2 = 0; snd_fifo_b_3 = 0;

    memset(fifo_a,       0, sizeof(fifo_a));
    memset(fifo_b,       0, sizeof(fifo_b));
    memset(snd_ch_state, 0, sizeof(snd_ch_state));
    memset(snd_buffer,   0, sizeof(snd_buffer));
    memset(sqr_ch,       0, sizeof(sqr_ch));
    memset(&wave_ch,     0, sizeof(wave_ch));
    memset(&noise_ch,    0, sizeof(noise_ch));
    memset(wave_ram,     0, sizeof(wave_ram));

    snd_pcm_vol.w = 0;
    snd_psg_vol.w = 0;
    snd_bias.w    = 0x200;
    snd_psg_enb.w = PSG_ENB;
    wave_reset();

    win0_h.w = 0; win1_h.w = 0; win0_v.w = 0; win1_v.w = 0;
}

__declspec(dllexport) int __stdcall GBA_Init(const char* rom_path) {
    if (!rom_path || !rom_path[0]) return -1;

    // [중요!] arm_init()은 최초 1회만 호출! (두 번째부터는 해제/재할당하지 않고 기존 버퍼 재사용)
    if (!is_memory_allocated) {
        arm_init();
        is_memory_allocated = true;
    }

    // 모든 메모리와 레지스터를 0으로 청소
    hardware_clear_all();

    // 바이오스 복사
    memcpy(bios, gba_bios_bin, sizeof(gba_bios_bin));

    // 카트리지 ROM 로드
    FILE *fp = fopen(rom_path, "rb");
    if (!fp) return -1;

    fseek(fp, 0, SEEK_END);
    cart_rom_size = ftell(fp);
    cart_rom_mask = to_pow2(cart_rom_size) - 1;

    if (cart_rom_size > MAX_ROM_SIZE) {
        cart_rom_size = MAX_ROM_SIZE;
    }

    fseek(fp, 0, SEEK_SET);
    memset(rom, 0, MAX_ROM_SIZE);
    fread(rom, 1, cart_rom_size, fp);
    fclose(fp);

    // CPU 리셋
    arm_reset();

    return 0;
}
// -----------------------------------------------------------------------------
// 2. 에뮬레이터 전체 상태 리셋 (현재 로드된 게임을 처음부터 다시 시작할 때)
// -----------------------------------------------------------------------------
__declspec(dllexport) void __stdcall GBA_Reset(void) {
    if (!is_memory_allocated) return;

    hardware_clear_all();
    memcpy(bios, gba_bios_bin, sizeof(gba_bios_bin));
    arm_reset(); // CPU 레지스터 및 파이프라인 리셋
}

// -----------------------------------------------------------------------------
// 6. 에뮬레이터 종료 및 자원 해제
// -----------------------------------------------------------------------------
__declspec(dllexport) void __stdcall GBA_Close(void) {
    if (is_memory_allocated) {
        arm_uninit();
        is_memory_allocated = false;
    }
}


// -----------------------------------------------------------------------------
// 3. 키 입력 상태 전달
// -----------------------------------------------------------------------------
__declspec(dllexport) void __stdcall GBA_SetKeys(uint16_t keys) {
    key_input.w = keys;
}

// video.c에 선언된 480x320 버퍼 참조
extern uint32_t gba_screen_2x[480 * 320];

// -----------------------------------------------------------------------------
// 4. 1프레임 실행 및 화면 버퍼 포인터 반환
// -----------------------------------------------------------------------------
__declspec(dllexport) void* __stdcall GBA_RunFrame(void) {
    run_frame();
    return (void*)gba_screen;
   // return (void*)gba_screen_2x; 
}

// -----------------------------------------------------------------------------
// 5. 사운드 스트림 믹싱 (sound.h의 sound_mix 연동)
// VB6 waveOut의 요청 바이트(2,192바이트)를 받아 PCM 스트림을 대상 포인터에 기록
// -----------------------------------------------------------------------------
__declspec(dllexport) int __stdcall GBA_GetAudioSamples(void* out_buffer, int byte_len) {
    if (!out_buffer || byte_len <= 0) return 0;

    // sound.h: void sound_mix(void *data, uint8_t *stream, int32_t len);
    // data 인자에는 NULL을 전달하고, out_buffer에 PCM 오디오 바이트 스트림을 채웁니다.
    sound_mix(NULL, (uint8_t*)out_buffer, byte_len);

    return byte_len;
}

