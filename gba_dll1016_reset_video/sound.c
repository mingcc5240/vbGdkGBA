#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "io.h"
#include "sound.h"
#include "dma.h"

#define PSG_MAX    0x7f
#define PSG_MIN  -0x80

#define SAMP_MAX   0x1ff
#define SAMP_MIN  -0x200

#define SAMPLE_TIME  (1.0 / (SND_FREQUENCY))

static double duty_lut[4]   = { 0.125, 0.250, 0.500, 0.750 };
static double duty_lut_i[4] = { 0.875, 0.750, 0.500, 0.250 };

static int8_t square_sample(uint8_t ch) {
    if (!(snd_psg_enb.w & (CH_SQR1 << ch))) return 0;

    uint8_t  sweep_time = (sqr_ch[ch].sweep.w >>  4) & 0x7;
    uint8_t  duty       = (sqr_ch[ch].tone.w  >>  6) & 0x3;
    uint8_t  env_step   = (sqr_ch[ch].tone.w  >>  8) & 0x7;
    uint8_t  envelope   = (sqr_ch[ch].tone.w  >> 12) & 0xf;
    uint8_t  snd_len    = (sqr_ch[ch].tone.w  >>  0) & 0x3f;
    uint16_t freq_hz    = (sqr_ch[ch].ctrl.w  >>  0) & 0x7ff;

    // [수정 1] 볼륨(envelope)이 0이고 증가 모드가 아니면 아예 연산하지 않고 무음 반환
    if (envelope == 0 && !(sqr_ch[ch].tone.w & ENV_INC)) return 0;

    double frequency = 131072.0 / (2048 - freq_hz);
    double length = (64 - snd_len) / 256.0;
    double sweep_interval = 0.0078125 * (sweep_time + 1);
    double envelope_interval = env_step / 64.0;
    double cycle_samples = SND_FREQUENCY / frequency;

    if (sqr_ch[ch].ctrl.w & CH_LEN) {
        snd_ch_state[ch].length_time += SAMPLE_TIME;
        if (snd_ch_state[ch].length_time >= length) {
            snd_psg_enb.w &= ~(CH_SQR1 << ch);
            return 0;
        }
    }

    // [수정 2] sweep_time이 0이면 스윕 비활성화! (기존 코드는 sweep_time == 0일 때도 스윕이 돌아서 음이 미쳐 날뛰었음!)
    if (ch == 0 && sweep_time > 0) {
        snd_ch_state[0].sweep_time += SAMPLE_TIME;
        if (snd_ch_state[0].sweep_time >= sweep_interval) {
            snd_ch_state[0].sweep_time -= sweep_interval;
            uint8_t sweep_shift = sqr_ch[0].sweep.w & 7;
            if (sweep_shift) {
                int32_t disp = freq_hz >> sweep_shift;
                int32_t new_freq = (sqr_ch[0].sweep.w & SWEEP_DEC) ? ((int32_t)freq_hz - disp) : ((int32_t)freq_hz + disp);

                if (new_freq >= 0 && new_freq < 0x7ff) {
                    freq_hz = (uint16_t)new_freq;
                    sqr_ch[0].ctrl.w &= ~0x7ff;
                    sqr_ch[0].ctrl.w |= freq_hz;
                } else if (new_freq >= 0x7ff) {
                    snd_psg_enb.w &= ~CH_SQR1;
                    return 0;
                }
            }
        }
    }

    if (env_step) {
        snd_ch_state[ch].env_time += SAMPLE_TIME;
        if (snd_ch_state[ch].env_time >= envelope_interval) {
            snd_ch_state[ch].env_time -= envelope_interval;
            if (sqr_ch[ch].tone.w & ENV_INC) {
                if (envelope < 0xf) envelope++;
            } else {
                if (envelope > 0x0) envelope--;
            }
            sqr_ch[ch].tone.w &= ~0xf000;
            sqr_ch[ch].tone.w |= envelope << 12;
        }
    }

    snd_ch_state[ch].samples++;
    if (snd_ch_state[ch].phase) {
        double phase_change = cycle_samples * duty_lut[duty];
        if (snd_ch_state[ch].samples > phase_change) {
            snd_ch_state[ch].samples = 0; // 위상 폭주 방지를 위해 깔끔하게 0으로 리셋
            snd_ch_state[ch].phase = false;
        }
    } else {
        double phase_change = cycle_samples * duty_lut_i[duty];
        if (snd_ch_state[ch].samples > phase_change) {
            snd_ch_state[ch].samples = 0;
            snd_ch_state[ch].phase = true;
        }
    }

    return snd_ch_state[ch].phase
        ? (int8_t)((envelope / 15.0) * PSG_MAX)
        : (int8_t)((envelope / 15.0) * PSG_MIN);
}

static int8_t wave_sample(void) {
    if (!((snd_psg_enb.w & CH_WAVE) && (wave_ch.wave.w & WAVE_PLAY))) return 0;

    uint8_t  snd_len = (wave_ch.volume.w >>  0) & 0xff;
    uint8_t  volume  = (wave_ch.volume.w >> 13) & 0x7;
    uint16_t freq_hz = (wave_ch.ctrl.w   >>  0) & 0x7ff;

    if (volume == 0) return 0;

    double frequency = 2097152.0 / (2048 - freq_hz);
    double length = (256 - snd_len) / 256.0;
    double cycle_samples = SND_FREQUENCY / frequency;

    if (wave_ch.ctrl.w & CH_LEN) {
        snd_ch_state[2].length_time += SAMPLE_TIME;
        if (snd_ch_state[2].length_time >= length) {
            snd_psg_enb.w &= ~CH_WAVE;
            return 0;
        }
    }

    snd_ch_state[2].samples++;
    if (snd_ch_state[2].samples >= cycle_samples) {
        snd_ch_state[2].samples = 0;
        if (--wave_samples)
            wave_position = (wave_position + 1) & 0x3f;
        else
            wave_reset();
    }

    // [수정 3] 음수 비트 시프트 버그를 막기 위해 나눗셈으로 정확히 계산
    int16_t raw_nibble = (wave_position & 1)
        ? ((wave_ram[(wave_position >> 1) & 0x1f] >> 0) & 0xf)
        : ((wave_ram[(wave_position >> 1) & 0x1f] >> 4) & 0xf);

    int16_t samp = raw_nibble - 8;

    if (volume & 4) {
        samp = (samp * 3) / 4; // 75% 볼륨
    } else {
        switch (volume & 3) {
            case 0: samp = 0; break;
            case 1: break;            // 100%
            case 2: samp /= 2; break; // 50%
            case 3: samp /= 4; break; // 25%
        }
    }

    return samp >= 0
        ? (int8_t)((samp /  7.0) * PSG_MAX)
        : (int8_t)((samp / -8.0) * PSG_MIN);
}

static int8_t noise_sample(void) {
    if (!(snd_psg_enb.w & CH_NOISE)) return 0;

    uint8_t env_step = (noise_ch.env.w  >>  8) & 0x7;
    uint8_t envelope = (noise_ch.env.w  >> 12) & 0xf;
    uint8_t snd_len  = (noise_ch.env.w  >>  0) & 0x3f;
    uint8_t freq_div = (noise_ch.ctrl.w >>  0) & 0x7;
    uint8_t freq_rsh = (noise_ch.ctrl.w >>  4) & 0xf;

    double frequency = freq_div
        ? (524288.0 / freq_div) / (1 << (freq_rsh + 1))
        : (524288.0 * 2.0) / (1 << (freq_rsh + 1));

    double length = (64 - snd_len) / 256.0;
    double envelope_interval = env_step / 64.0;
    double cycle_samples = SND_FREQUENCY / frequency;

    if (noise_ch.ctrl.w & CH_LEN) {
        snd_ch_state[3].length_time += SAMPLE_TIME;
        if (snd_ch_state[3].length_time >= length) {
            snd_psg_enb.w &= ~CH_NOISE;
            return 0;
        }
    }

    if (env_step) {
        snd_ch_state[3].env_time += SAMPLE_TIME;
        if (snd_ch_state[3].env_time >= envelope_interval) {
            snd_ch_state[3].env_time -= envelope_interval;
            if (noise_ch.env.w & ENV_INC) {
                if (envelope < 0xf) envelope++;
            } else {
                if (envelope > 0x0) envelope--;
            }
            noise_ch.env.w &= ~0xf000;
            noise_ch.env.w |= envelope << 12;
        }
    }

    uint8_t carry = snd_ch_state[3].lfsr & 1;

    snd_ch_state[3].samples++;
    if (snd_ch_state[3].samples >= cycle_samples) {
        // [수정] cycle_samples가 급격히 줄어들 때 폭주 방지
        if (cycle_samples > 1.0) {
            while (snd_ch_state[3].samples >= cycle_samples)
                snd_ch_state[3].samples -= cycle_samples;
        } else {
            snd_ch_state[3].samples = 0;
        }

        snd_ch_state[3].lfsr >>= 1;
        uint8_t high = (snd_ch_state[3].lfsr & 1) ^ carry;
        if (noise_ch.ctrl.w & NOISE_7)
            snd_ch_state[3].lfsr |= (high <<  6);
        else
            snd_ch_state[3].lfsr |= (high << 14);
    }

    return carry
        ? (envelope / 15.0) * PSG_MAX
        : (envelope / 15.0) * PSG_MIN;
}

int16_t snd_buffer[BUFF_SAMPLES];
uint32_t snd_cur_play  = 0;
uint32_t snd_cur_write =  1096; // 정확히 1프레임 분량 앞서서 시작
// 이전 샘플 값을 기억하여 언더런 시 파형이 끊기지 않게 유지하는 변수
static int16_t last_left  = 0;
static int16_t last_right = 0;


void wave_reset(void) {
    if (wave_ch.wave.w & WAVE_64) {
        wave_position = 0;
        wave_samples  = 64;
    } else {
        wave_position = (wave_ch.wave.w >> 1) & 0x20;
        wave_samples  = 32;
    }
}

void sound_buffer_wrap(void) {
    // 인덱스 마스킹으로 인한 불연속 단절(뚜뚜 소리) 방지를 위해 완전히 비워둠
}

void fifo_a_copy(void) {
    if (fifo_a_len + 4 > 0x20) return;
    fifo_a[fifo_a_len++] = snd_fifo_a_0;
    fifo_a[fifo_a_len++] = snd_fifo_a_1;
    fifo_a[fifo_a_len++] = snd_fifo_a_2;
    fifo_a[fifo_a_len++] = snd_fifo_a_3;
}

void fifo_b_copy(void) {
    if (fifo_b_len + 4 > 0x20) return;
    fifo_b[fifo_b_len++] = snd_fifo_b_0;
    fifo_b[fifo_b_len++] = snd_fifo_b_1;
    fifo_b[fifo_b_len++] = snd_fifo_b_2;
    fifo_b[fifo_b_len++] = snd_fifo_b_3;
}

int8_t fifo_a_samp = 0;
int8_t fifo_b_samp = 0;

void fifo_a_load(void) {
    if (fifo_a_len > 0) {
        fifo_a_samp = (int8_t)fifo_a[0];
        fifo_a_len--;
        for (uint8_t i = 0; i < fifo_a_len; i++) {
            fifo_a[i] = fifo_a[i + 1];
        }
        if (fifo_a_len <= 16) {
            dma_transfer_fifo(1);
        }
    }
}

void fifo_b_load(void) {
    if (fifo_b_len > 0) {
        fifo_b_samp = (int8_t)fifo_b[0];
        fifo_b_len--;
        for (uint8_t i = 0; i < fifo_b_len; i++) {
            fifo_b[i] = fifo_b[i + 1];
        }
        if (fifo_b_len <= 16) {
            dma_transfer_fifo(2);
        }
    }
}

uint32_t snd_cycles = 0;
static int32_t psg_vol_lut[8] = { 0x000, 0x024, 0x049, 0x06d, 0x092, 0x0b6, 0x0db, 0x100 };
static int32_t psg_rsh_lut[4] = { 0xa, 0x9, 0x8, 0x7 };

static inline int16_t clip(int32_t value) {
    if (value > SAMP_MAX) return SAMP_MAX;
    if (value < SAMP_MIN) return SAMP_MIN;
    return (int16_t)value;
}

// -----------------------------------------------------------------------------
// 사운드 클록: 512사이클마다 정확하게 1쌍의 스테레오 샘플 생성
// -----------------------------------------------------------------------------
// sound.c

void sound_clock(uint32_t cycles) {
    snd_cycles += cycles;

    while (snd_cycles >= SAMP_CYCLES) {
        // 1. DirectSound PCM (FIFO에서 꺼내진 최근 샘플)
        int16_t samp_ch4 = (fifo_a_samp << 1) >> !(snd_pcm_vol.w & 4);
        int16_t samp_ch5 = (fifo_b_samp << 1) >> !(snd_pcm_vol.w & 8);

        int16_t samp_pcm_l = 0;
        int16_t samp_pcm_r = 0;

        if (snd_pcm_vol.w & CH_DMAA_L) samp_pcm_l = clip(samp_pcm_l + samp_ch4);
        if (snd_pcm_vol.w & CH_DMAB_L) samp_pcm_l = clip(samp_pcm_l + samp_ch5);

        if (snd_pcm_vol.w & CH_DMAA_R) samp_pcm_r = clip(samp_pcm_r + samp_ch4);
        if (snd_pcm_vol.w & CH_DMAB_R) samp_pcm_r = clip(samp_pcm_r + samp_ch5);

        // 2. PSG 4개 채널 계산
        int16_t samp_ch0 = square_sample(0);
        int16_t samp_ch1 = square_sample(1);
        int16_t samp_ch2 = wave_sample();
        int16_t samp_ch3 = noise_sample();

        // 거친 화이트 노이즈만 살짝 감쇄 (30%)
        samp_ch3 = (samp_ch3 * 3) / 10;

        int32_t samp_psg_l = 0;
        int32_t samp_psg_r = 0;

        if (snd_psg_vol.w & CH_SQR1_L)  samp_psg_l = clip(samp_psg_l + samp_ch0);
        if (snd_psg_vol.w & CH_SQR2_L)  samp_psg_l = clip(samp_psg_l + samp_ch1);
        if (snd_psg_vol.w & CH_WAVE_L)  samp_psg_l = clip(samp_psg_l + samp_ch2);
        if (snd_psg_vol.w & CH_NOISE_L) samp_psg_l = clip(samp_psg_l + samp_ch3);

        if (snd_psg_vol.w & CH_SQR1_R)  samp_psg_r = clip(samp_psg_r + samp_ch0);
        if (snd_psg_vol.w & CH_SQR2_R)  samp_psg_r = clip(samp_psg_r + samp_ch1);
        if (snd_psg_vol.w & CH_WAVE_R)  samp_psg_r = clip(samp_psg_r + samp_ch2);
        if (snd_psg_vol.w & CH_NOISE_R) samp_psg_r = clip(samp_psg_r + samp_ch3);

        samp_psg_l  *= psg_vol_lut[(snd_psg_vol.w >> 4) & 7];
        samp_psg_r  *= psg_vol_lut[(snd_psg_vol.w >> 0) & 7];

        samp_psg_l >>= psg_rsh_lut[(snd_pcm_vol.w >> 0) & 3];
        samp_psg_r >>= psg_rsh_lut[(snd_pcm_vol.w >> 0) & 3];

        // 3. 링버퍼에 샘플 기록 (snd_cur_write는 절대 마스킹하지 않고 계속 증가)
        snd_buffer[snd_cur_write++ & BUFF_SAMPLES_MSK] = clip(samp_psg_l + samp_pcm_l);
        snd_buffer[snd_cur_write++ & BUFF_SAMPLES_MSK] = clip(samp_psg_r + samp_pcm_r);

        snd_cycles -= SAMP_CYCLES;
    }
}

// 이전 샘플과의 부드러운 연결을 위한 필터 상태 변수
static int32_t lpf_left = 0;
static int32_t lpf_right = 0;

void sound_mix(void* data, uint8_t* stream, int32_t len) {
    int16_t* out = (int16_t*)stream;
    int32_t out_frames = len / 4;
    if (out_frames <= 0) return;

    uint32_t avail_samples = snd_cur_write - snd_cur_play;

    if (avail_samples > BUFF_SAMPLES) {
        snd_cur_play = snd_cur_write - (BUFF_SAMPLES / 2);
        avail_samples = BUFF_SAMPLES / 2;
    }

    uint32_t avail_frames = avail_samples / 2;

    if (avail_frames == 0) {
        for (int32_t i = 0; i < out_frames; i++) {
            out[i * 2 + 0] = last_left;
            out[i * 2 + 1] = last_right;
        }
        return;
    }

    double step = (double)avail_frames / (double)out_frames;
    if (avail_frames >= (uint32_t)out_frames && avail_frames < (uint32_t)(out_frames * 2)) {
        step = 1.0;
    }

    double read_pos = 0.0;

    for (int32_t i = 0; i < out_frames; i++) {
        uint32_t frame_idx = (uint32_t)read_pos;
        double   frac = read_pos - (double)frame_idx; // 소수점 위치 (보간용)

        if (frame_idx >= avail_frames) {
            frame_idx = avail_frames - 1;
            frac = 0.0;
        }

        uint32_t next_idx = (frame_idx + 1 < avail_frames) ? (frame_idx + 1) : frame_idx;

        // 현재 샘플(s0)과 다음 샘플(s1) 인덱스
        uint32_t idx0_l = (snd_cur_play + frame_idx * 2 + 0) & BUFF_SAMPLES_MSK;
        uint32_t idx0_r = (snd_cur_play + frame_idx * 2 + 1) & BUFF_SAMPLES_MSK;
        uint32_t idx1_l = (snd_cur_play + next_idx * 2 + 0) & BUFF_SAMPLES_MSK;
        uint32_t idx1_r = (snd_cur_play + next_idx * 2 + 1) & BUFF_SAMPLES_MSK;

        // 1. 선형 보간(Linear Interpolation): 샘플과 샘플 사이의 계단 현상을 매끄럽게 연결
        double s_left = (1.0 - frac) * snd_buffer[idx0_l] + frac * snd_buffer[idx1_l];
        double s_right = (1.0 - frac) * snd_buffer[idx0_r] + frac * snd_buffer[idx1_r];

        // 2. 볼륨 증폭 (<< 6과 동일한 64배)
        int32_t raw_left = (int32_t)(s_left * 64.0);
        int32_t raw_right = (int32_t)(s_right * 64.0);

        // 3. GBA 고유의 거친 고주파 노이즈를 다듬어주는 소프트 로우패스 필터 (비율 75% : 25%)
        lpf_left = (raw_left * 3 + lpf_left) >> 2;
        lpf_right = (raw_right * 3 + lpf_right) >> 2;

        int32_t left = lpf_left;
        int32_t right = lpf_right;

        // 4. 클리핑 방지
        if (left > 32767) left = 32767;
        else if (left < -32768) left = -32768;

        if (right > 32767) right = 32767;
        else if (right < -32768) right = -32768;

        out[i * 2 + 0] = (int16_t)left;
        out[i * 2 + 1] = (int16_t)right;

        read_pos += step;
    }

    uint32_t consumed_frames = (step == 1.0) ? (uint32_t)out_frames : avail_frames;
    snd_cur_play += consumed_frames * 2;

    last_left = out[(out_frames - 1) * 2 + 0];
    last_right = out[(out_frames - 1) * 2 + 1];
}

void sound_mix1(void *data, uint8_t *stream, int32_t len) {
    int16_t *out = (int16_t *)stream;
    int32_t out_frames = len / 4; // 16비트 스테레오 기준 출력해야 할 총 프레임(쌍) 수
    if (out_frames <= 0) return;

    // 1. 현재 버퍼에 쌓여있는 '읽지 않은 샘플(int16_t 개수)' 계산
    uint32_t avail_samples = snd_cur_write - snd_cur_play;

    // 버퍼 오버플로우로 너무 많이 밀렸다면 버퍼 크기의 절반 수준으로 리셋
    if (avail_samples > BUFF_SAMPLES) {
        snd_cur_play = snd_cur_write - (BUFF_SAMPLES / 2);
        avail_samples = BUFF_SAMPLES / 2;
    }

    uint32_t avail_frames = avail_samples / 2; // 스테레오 쌍(Frame) 개수

    // 2. 만약 쌓인 샘플이 아예 없다면 마지막 출력값을 유지하여 '틱' 노이즈 방지
    if (avail_frames == 0) {
        for (int32_t i = 0; i < out_frames; i++) {
            out[i * 2 + 0] = last_left;
            out[i * 2 + 1] = last_right;
        }
        return;
    }

    // 3. 쌓인 샘플이 요청량(out_frames)과 다르더라도(예: 180개만 쌓였을 때),
    //    요청된 출력 버퍼 길이에 딱 맞게 비율(step)을 계산해 부드럽게 늘려서(보간) 채움
    double step = (double)avail_frames / (double)out_frames;
    
    // 만약 정상적인 속도(90% ~ 110% 이내)로 충분한 샘플이 들어오고 있다면 1:1 재생
    if (avail_frames >= (uint32_t)out_frames && avail_frames < (uint32_t)(out_frames * 2)) {
        step = 1.0;
    }

    double read_pos = 0.0;

    for (int32_t i = 0; i < out_frames; i++) {
        uint32_t frame_idx = (uint32_t)read_pos;
        if (frame_idx >= avail_frames) {
            frame_idx = avail_frames - 1;
        }

        uint32_t idx_l = (snd_cur_play + frame_idx * 2 + 0) & BUFF_SAMPLES_MSK;
        uint32_t idx_r = (snd_cur_play + frame_idx * 2 + 1) & BUFF_SAMPLES_MSK;

        int32_t left  = (int32_t)snd_buffer[idx_l] << 7;
        int32_t right = (int32_t)snd_buffer[idx_r] << 7;

        if (left > 32767) left = 32767;
        else if (left < -32768) left = -32768;

        if (right > 32767) right = 32767;
        else if (right < -32768) right = -32768;

        out[i * 2 + 0] = (int16_t)left;
        out[i * 2 + 1] = (int16_t)right;

        read_pos += step;
    }

    // 4. 실제로 소비한 만큼만 snd_cur_play 포인터를 전진시킴
    uint32_t consumed_frames = (step == 1.0) ? (uint32_t)out_frames : avail_frames;
    snd_cur_play += consumed_frames * 2;

    // 다음 언더런 방지를 위해 마지막 샘플 저장
    last_left  = out[(out_frames - 1) * 2 + 0];
    last_right = out[(out_frames - 1) * 2 + 1];
}

void sound_mix0(void *data, uint8_t *stream, int32_t len) {
    int16_t *out = (int16_t *)stream;
    int32_t samples_to_read = len / 2; // len = 2048바이트 기준 1024개 샘플
    int32_t i;

    // 현재 쓰기 위치보다 항상 1프레임(약 1024샘플) 뒤를 안정적으로 따라가도록 고정
    // 이렇게 하면 읽기 커서가 쓰기 커서를 앞지르거나(언더런), 오래된 영역을 긁지 않습니다.
    snd_cur_play = snd_cur_write - samples_to_read;

    for (i = 0; i < samples_to_read; i += 2) {
        uint32_t idx_l = snd_cur_play++ & BUFF_SAMPLES_MSK;
        uint32_t idx_r = snd_cur_play++ & BUFF_SAMPLES_MSK;

        // << 5 스케일링으로 MaxAmp가 32768 클리핑에 닿지 않도록 방지
        int32_t left  = (int32_t)snd_buffer[idx_l] << 5;
        int32_t right = (int32_t)snd_buffer[idx_r] << 5;

        // [확인!] 아래와 같이 버퍼를 0으로 지우는 코드가 있다면 반드시 삭제되어야 합니다!
        // snd_buffer[idx_l] = 0;  <-- 이 줄이 있으면 NonZero가 160대로 떨어집니다!
        // snd_buffer[idx_r] = 0;  <-- 삭제 확인!

        if (left > 32767) left = 32767;
        else if (left < -32768) left = -32768;

        if (right > 32767) right = 32767;
        else if (right < -32768) right = -32768;

        out[i + 0] = (int16_t)left;
        out[i + 1] = (int16_t)right;
    }
}