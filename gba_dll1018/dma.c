#include "arm.h"
#include "arm_mem.h"

#include "dma.h"
#include "io.h"
#include "sound.h"

//TODO: Timing - DMA should take some amount of cycles

// DMA 전송 중 재귀 호출(DMA가 IO 레지스터를 건드려 또 DMA를 부르는 현상) 방지 플래그
static bool in_dma = false;

void dma_transfer(dma_timing_e timing) {
    if (in_dma) return;
    in_dma = true;

    // [핵심 수정 1] arm_read / arm_write 내부에서 arm_cycles가 마음대로 증가하여
    // arm_exec()의 사이클 계산에 언더플로우(42억 사이클 폭주)가 발생하는 것을 원천 차단!
    uint32_t saved_cycles = arm_cycles;

    uint8_t ch;

    for (ch = 0; ch < 4; ch++) {
        if (!(dma_ch[ch].ctrl.w & DMA_ENB) ||
            ((dma_ch[ch].ctrl.w >> 12) & 3) != timing)
            continue;

        if (ch == 3)
            eeprom_idx = 0;

        int8_t unit_size = (dma_ch[ch].ctrl.w & DMA_32) ? 4 : 2;

        bool dst_reload = false;

        int8_t dst_inc = 0;
        int8_t src_inc = 0;

        switch ((dma_ch[ch].ctrl.w >> 5) & 3) {
        case 0: dst_inc = unit_size; break;
        case 1: dst_inc = -unit_size; break;
        case 3:
            dst_inc = unit_size;
            dst_reload = true;
            break;
        }

        switch ((dma_ch[ch].ctrl.w >> 7) & 3) {
        case 0: src_inc = unit_size; break;
        case 1: src_inc = -unit_size; break;
        }

        // [핵심 수정 2] dma_count가 0이거나 비정상적으로 클 때 42억 번 무한 루프에 빠지는 버그 수정!
        uint32_t count = dma_count[ch];
        if (count == 0) {
            count = (ch == 3) ? 0x10000 : 0x4000;
        }

        while (count--) {
            if (dma_ch[ch].ctrl.w & DMA_32)
                arm_write(dma_dst_addr[ch] & ~3, arm_read(dma_src_addr[ch] & ~3));
            else
                arm_writeh(dma_dst_addr[ch] & ~1, arm_readh(dma_src_addr[ch] & ~1));

            dma_dst_addr[ch] += dst_inc;
            dma_src_addr[ch] += src_inc;

            // 대량 전송 중 arm_cycles 오버플로우 방지
            arm_cycles = saved_cycles;
        }

        dma_count[ch] = 0;

        if (dma_ch[ch].ctrl.w & DMA_IRQ)
            trigger_irq(DMA0_FLAG << ch);

        if (dma_ch[ch].ctrl.w & DMA_REP) {
            // 리로드 시에도 count.w가 0이면 최대치(0x4000 / 0x10000)로 정확히 세팅!
            uint32_t reload_cnt = dma_ch[ch].count.w;
            if (reload_cnt == 0) {
                reload_cnt = (ch == 3) ? 0x10000 : 0x4000;
            }
            dma_count[ch] = reload_cnt;

            if (dst_reload) {
                dma_dst_addr[ch] = dma_ch[ch].dst.w;
            }

            continue;
        }

        dma_ch[ch].ctrl.w &= ~DMA_ENB;
    }

    // CPU 사이클을 DMA 실행 전 상태로 깔끔하게 복구
    arm_cycles = saved_cycles;
    in_dma = false;
}

void dma_transfer_fifo(uint8_t ch) {
    // DMA 채널 활성화 및 SPECIAL 모드 체크
    if (!(dma_ch[ch].ctrl.w & DMA_ENB) ||
        ((dma_ch[ch].ctrl.w >> 12) & 3) != SPECIAL)
        return;

    // [핵심 수정 3] timers_clock 도중 호출되는 dma_transfer_fifo에서
    // arm_read()가 arm_cycles를 증가시켜 버리면, arm_exec()로 돌아갔을 때
    // (target_cycles - arm_cycles) 계산에서 언더플로우가 터져 게임이 즉시 멈춥니다!
    uint32_t saved_cycles = arm_cycles;

    uint8_t i;

    // GBA FIFO 모드는 한 번 트리거될 때 4개의 32비트 워드(총 16바이트)를 전송
    for (i = 0; i < 4; i++) {
        // 1. 메모리(ROM/WRAM)에서 32비트 정렬된 오디오 데이터 1워드 읽기
        uint32_t val = arm_read(dma_src_addr[ch] & ~3);

        // 2. 바이트 단위로 분해 후 FIFO 큐에 직접 밀어 넣기
        if (ch == 1) {
            snd_fifo_a_0 = (val >> 0) & 0xff;
            snd_fifo_a_1 = (val >> 8) & 0xff;
            snd_fifo_a_2 = (val >> 16) & 0xff;
            snd_fifo_a_3 = (val >> 24) & 0xff;
            fifo_a_copy(); // FIFO A에 4바이트 푸시
        }
        else if (ch == 2) {
            snd_fifo_b_0 = (val >> 0) & 0xff;
            snd_fifo_b_1 = (val >> 8) & 0xff;
            snd_fifo_b_2 = (val >> 16) & 0xff;
            snd_fifo_b_3 = (val >> 24) & 0xff;
            fifo_b_copy(); // FIFO B에 4바이트 푸시
        }

        // 3. 소스 주소 증감 처리 (비트 7-8: 0=증가, 1=감소, 2=고정)
        switch ((dma_ch[ch].ctrl.w >> 7) & 3) {
        case 0: dma_src_addr[ch] += 4; break;
        case 1: dma_src_addr[ch] -= 4; break;
        }
    }

    // [핵심 수정 4] arm_read()로 인해 증가해 버린 arm_cycles를 원래대로 복원!
    arm_cycles = saved_cycles;

    // 4. 인터럽트 요구 시 플래그 세트
    if (dma_ch[ch].ctrl.w & DMA_IRQ)
        trigger_irq(DMA0_FLAG << ch);

    // 5. 만약 Repeat(DMA_REP) 비트가 꺼져 있다면 1회 전송 후 DMA 비활성화 (mGBA 스펙)
    if (!(dma_ch[ch].ctrl.w & DMA_REP)) {
        dma_ch[ch].ctrl.w &= ~DMA_ENB;
    }
}

/*
void dma_transfer(dma_timing_e timing) {
    uint8_t ch;

    for (ch = 0; ch < 4; ch++) {
        if (!(dma_ch[ch].ctrl.w & DMA_ENB) ||
            ((dma_ch[ch].ctrl.w >> 12) & 3) != timing)
            continue;

        if (ch == 3)
            eeprom_idx = 0;

        int8_t unit_size = (dma_ch[ch].ctrl.w & DMA_32) ? 4 : 2;

        bool dst_reload = false;

        int8_t dst_inc = 0;
        int8_t src_inc = 0;

        switch ((dma_ch[ch].ctrl.w >> 5) & 3) {
            case 0: dst_inc =  unit_size; break;
            case 1: dst_inc = -unit_size; break;
            case 3:
                dst_inc = unit_size;
                dst_reload = true;
                break;
        }

        switch ((dma_ch[ch].ctrl.w >> 7) & 3) {
            case 0: src_inc =  unit_size; break;
            case 1: src_inc = -unit_size; break;
        }

        while (dma_count[ch]--) {
            if (dma_ch[ch].ctrl.w & DMA_32)
                arm_write(dma_dst_addr[ch],  arm_read(dma_src_addr[ch]));
            else
                arm_writeh(dma_dst_addr[ch], arm_readh(dma_src_addr[ch]));

            dma_dst_addr[ch] += dst_inc;
            dma_src_addr[ch] += src_inc;
        }

        if (dma_ch[ch].ctrl.w & DMA_IRQ)
            trigger_irq(DMA0_FLAG << ch);

        if (dma_ch[ch].ctrl.w & DMA_REP) {
            dma_count[ch] = dma_ch[ch].count.w;

            if (dst_reload) {
                dma_dst_addr[ch] = dma_ch[ch].dst.w;
            }

            continue;
        }

        dma_ch[ch].ctrl.w &= ~DMA_ENB;
    }
}

void dma_transfer_fifo(uint8_t ch) {
    // DMA 채널 활성화 및 SPECIAL 모드 체크
    if (!(dma_ch[ch].ctrl.w & DMA_ENB) ||
        ((dma_ch[ch].ctrl.w >> 12) & 3) != SPECIAL)
        return;

    uint8_t i;

    // GBA FIFO 모드는 한 번 트리거될 때 4개의 32비트 워드(총 16바이트)를 전송
    for (i = 0; i < 4; i++) {
        // 1. 메모리(ROM/WRAM)에서 32비트 오디오 데이터 1워드 읽기
        uint32_t val = arm_read(dma_src_addr[ch]);

        // 2. 바이트 단위로 분해 후 FIFO 큐에 직접 밀어 넣기
        if (ch == 1) {
            snd_fifo_a_0 = (val >>  0) & 0xff;
            snd_fifo_a_1 = (val >>  8) & 0xff;
            snd_fifo_a_2 = (val >> 16) & 0xff;
            snd_fifo_a_3 = (val >> 24) & 0xff;
            fifo_a_copy(); // FIFO A에 4바이트 푸시
        } else if (ch == 2) {
            snd_fifo_b_0 = (val >>  0) & 0xff;
            snd_fifo_b_1 = (val >>  8) & 0xff;
            snd_fifo_b_2 = (val >> 16) & 0xff;
            snd_fifo_b_3 = (val >> 24) & 0xff;
            fifo_b_copy(); // FIFO B에 4바이트 푸시
        }

        // 3. 소스 주소 증감 처리 (비트 7-8: 0=증가, 1=감소, 2=고정)
        switch ((dma_ch[ch].ctrl.w >> 7) & 3) {
            case 0: dma_src_addr[ch] += 4; break;
            case 1: dma_src_addr[ch] -= 4; break;
        }
    }

    // 4. 인터럽트 요구 시 플래그 세트
    if (dma_ch[ch].ctrl.w & DMA_IRQ)
        trigger_irq(DMA0_FLAG << ch);
} */