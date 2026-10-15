#include "arm.h"
#include "arm_mem.h"

#include "dma.h"
#include "io.h"
#include "sound.h"

//TODO: Timing - DMA should take some amount of cycles

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
}