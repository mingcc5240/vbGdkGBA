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

