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

// video.c에서 렌더링하는 32비트 프레임 버퍼 참조
extern uint32_t gba_screen[GBA_WIDTH * GBA_HEIGHT];

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

// 외부 전역 변수 참조 (arm.c, io.c, timer.c, sound.c 등에 정의된 상태 변수들)
extern uint8_t *wram;
extern uint8_t *iwram;
extern uint8_t *vram;
extern uint8_t *pram;
extern uint8_t *oam;
extern uint8_t *rom;
extern uint8_t *bios;

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
    // 1. 메모리 영역 0으로 초기화
    if (wram)  memset(wram,  0, 0x40000);
    if (iwram) memset(iwram, 0, 0x8000);
    if (vram)  memset(vram,  0, 0x18000);
    if (oam)   memset(oam,   0, 0x400);
    if (pram)  memset(pram,  0, 0x400);
    memset(palette, 0, sizeof(palette));
    memset(gba_screen, 0, sizeof(gba_screen));

    // 2. [핵심!] CPU HALT 상태 및 사이클 카운터 초기화 (이게 안 되면 두 번째 로딩 시 멈춤!)
    int_halt   = false;
    arm_cycles = 0;

    // 3. [핵심!] 이전 게임이 켜둔 인터럽트, 타이머, LCD 상태 강제 초기화
    //int_mas.w   = 0; // IME (마스터 인터럽트 비활성화)
    int_enb.w   = 0; // IE  (개별 인터럽트 비활성화)
    int_ack.w   = 0; // IF  (인터럽트 플래그 클리어)
    disp_stat.w = 0; // DISPSTAT 초기화
    disp_cnt.w  = 0; // DISPCNT 초기화
    v_count.w   = 0;
    tmr_enb     = 0; // 실행 중인 모든 타이머 정지

    // DMA 채널 제어 레지스터 초기화 (이전 게임의 DMA 폭주 방지)
    memset(dma_ch, 0, sizeof(dma_ch));

    // 4. 사운드 버퍼 및 채널 상태 완벽 초기화
    snd_cycles    = 0;
    snd_cur_play  = 0;
    snd_cur_write = 0;
    fifo_a_len    = 0;
    fifo_b_len    = 0;
    fifo_a_samp   = 0;
    fifo_b_samp   = 0;
    memset(fifo_a, 0, sizeof(fifo_a));
    memset(fifo_b, 0, sizeof(fifo_b));
    memset(snd_ch_state, 0, sizeof(snd_ch_state));
    memset(snd_buffer, 0, sizeof(snd_buffer));
    memset(sqr_ch, 0, sizeof(sqr_ch));
    memset(&wave_ch, 0, sizeof(wave_ch));
    memset(&noise_ch, 0, sizeof(noise_ch));
    snd_pcm_vol.w = 0;
    snd_psg_vol.w = 0;
    snd_psg_enb.w = PSG_ENB; // 사운드 마스터 전원 인가
    wave_reset();
}

// -----------------------------------------------------------------------------
// 1. 에뮬레이터 초기화 및 ROM 로드 (몇 번을 다시 호출해도 안전하게 동작)
// -----------------------------------------------------------------------------
__declspec(dllexport) int __stdcall GBA_Init(const char* rom_path) {
    if (!rom_path || !rom_path[0]) return -1;

    // 1) 이미 메모리가 할당되어 있다면 해제 후 재할당 (메모리 누수 방지)
    if (is_memory_allocated) {
        arm_uninit();
        is_memory_allocated = false;
    }
    arm_init();
    is_memory_allocated = true;

    // 2) 모든 RAM, VRAM, I/O 레지스터, HALT 플래그를 0으로 깨끗이 청소
    hardware_clear_all();

    // 3) 내장 바이오스 복사 (16,384 바이트)
    memcpy(bios, gba_bios_bin, sizeof(gba_bios_bin));

    // 4) 이전 ROM 데이터가 남지 않도록 ROM 버퍼 청소 후 새 ROM 파일 로드
    if (rom) memset(rom, 0, MAX_ROM_SIZE);

    FILE *fp = fopen(rom_path, "rb");
    if (!fp) return -1; // 파일 열기 실패

    fseek(fp, 0, SEEK_END);
    cart_rom_size = ftell(fp);
    cart_rom_mask = to_pow2(cart_rom_size) - 1;

    if (cart_rom_size > MAX_ROM_SIZE) {
        cart_rom_size = MAX_ROM_SIZE;
    }

    fseek(fp, 0, SEEK_SET);
    fread(rom, 1, cart_rom_size, fp);
    fclose(fp);

    // 5) ROM과 BIOS가 모두 메모리에 올라간 상태에서 CPU 리셋 및 파이프라인 채우기!
    arm_reset();

    return 0; // 성공
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

// -----------------------------------------------------------------------------
// 4. 1프레임 실행 및 화면 버퍼 포인터 반환
// -----------------------------------------------------------------------------
__declspec(dllexport) void* __stdcall GBA_RunFrame(void) {
    run_frame();
    return (void*)gba_screen;
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

