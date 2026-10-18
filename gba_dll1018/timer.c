#include "arm.h"

#include "dma.h"
#include "io.h"
#include "sound.h"
#include "timer.h"

static const uint8_t pscale_shift_lut[4]  = { 0, 6, 8, 10 };

void timers_clock(uint32_t cycles) {
    uint8_t idx;
    uint32_t overflow_cnt = 0; // bool 대신 오버플로우 발생 '횟수'를 정확히 추적

    for (idx = 0; idx < 4; idx++) {
        if (!(tmr[idx].ctrl.w & TMR_ENB)) {
            overflow_cnt = 0;
            continue;
        }

        // [버그 수정 1] GBA 스펙상 Timer 0 (idx == 0)은 Cascade 모드가 절대 작동하지 않음!
        if ((tmr[idx].ctrl.w & TMR_CASCADE) && (idx > 0)) {
            tmr[idx].count.w += overflow_cnt;
        }
        else {
            uint8_t shift = pscale_shift_lut[tmr[idx].ctrl.w & 3];
            uint32_t inc = (tmr_icnt[idx] += cycles) >> shift;

            tmr[idx].count.w += inc;
            tmr_icnt[idx] -= inc << shift;
        }

        overflow_cnt = 0;

        // [버그 수정 2] 단일 if문이 아닌 while문으로 다중 오버플로우를 완벽히 해소!
        // count.w가 0xFFFF 이하로 내려올 때까지 정확히 반복합니다.
        while (tmr[idx].count.w > 0xffff) {
            uint32_t reload = tmr[idx].reload.w;

            // 만약 reload 값이 너무 커서(예: 0xFFFF) 루프가 수백 번 도는 현상(프리징)을 막기 위한 안전장치
            uint32_t period = 0x10000 - reload;
            if (period == 0) period = 1;

            tmr[idx].count.w -= period;
            overflow_cnt++;

            // 사운드 타이머는 보통 Timer 0 또는 Timer 1만 사용됨
            if (idx < 2) {
                if (((snd_pcm_vol.w >> 10) & 1) == idx) {
                    // DMA Sound A FIFO
                    fifo_a_load();
                    if (fifo_a_len <= 0x10) dma_transfer_fifo(1);
                }

                if (((snd_pcm_vol.w >> 14) & 1) == idx) {
                    // DMA Sound B FIFO
                    fifo_b_load();
                    if (fifo_b_len <= 0x10) dma_transfer_fifo(2);
                }
            }

            // 비정상적으로 큰 사이클이 들어와 사운드 루프가 64회 이상 돌면 나머지는 한 번에 스킵 (에뮬레이터 멈춤 방지)
            if (overflow_cnt >= 64 && tmr[idx].count.w > 0xffff) {
                uint32_t extra = (tmr[idx].count.w - 0x10000) / period + 1;
                tmr[idx].count.w -= extra * period;
                overflow_cnt += extra;
                break;
            }
        }

        // 오버플로우가 1번이라도 발생했다면 IRQ 트리거
        if ((tmr[idx].ctrl.w & TMR_IRQ) && (overflow_cnt > 0)) {
            trigger_irq(TMR0_FLAG << idx);
        }
    }
}

void timers_clock0(uint32_t cycles) {
    uint8_t idx;
    bool overflow = false;

    for (idx = 0; idx < 4; idx++) {
        if (!(tmr[idx].ctrl.w & TMR_ENB)) {
            overflow = false;
            continue;
        }

        if (tmr[idx].ctrl.w & TMR_CASCADE) {
            if (overflow) tmr[idx].count.w++;
        } else {
            uint8_t shift = pscale_shift_lut[tmr[idx].ctrl.w & 3];
            uint32_t inc = (tmr_icnt[idx] += cycles) >> shift;

            tmr[idx].count.w += inc;
            tmr_icnt[idx] -= inc << shift;
        }

        if ((overflow = (tmr[idx].count.w > 0xffff))) {
            tmr[idx].count.w = tmr[idx].reload.w + (tmr[idx].count.w - 0x10000);

            if (((snd_pcm_vol.w >> 10) & 1) == idx) {
                //DMA Sound A FIFO
                fifo_a_load();

                if (fifo_a_len <= 0x10) dma_transfer_fifo(1);
            }

            if (((snd_pcm_vol.w >> 14) & 1) == idx) {
                //DMA Sound B FIFO
                fifo_b_load();

                if (fifo_b_len <= 0x10) dma_transfer_fifo(2);
            }
        }

        if ((tmr[idx].ctrl.w & TMR_IRQ) && overflow)
            trigger_irq(TMR0_FLAG << idx);
    }
}