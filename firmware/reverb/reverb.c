/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

//------ TODO ------//
// - Rewrite audio output to pull data using dma instead of using
// pwm interrupts - See https://github.com/earlephilhower/arduino-pico/blob/master/libraries/PWMAudio/src/PWMAudio.cpp
// in PWMAudio::begin()
// - Move algorithm to a different file
// - Bind controls
//------      ------//

#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include <hardware/regs/dreq.h>
#include <hardware/structs/adc.h>
#include <hardware/structs/io_bank0.h>
#include <stdbool.h>
#include <stdint.h>


//- INPUT & PROCESSING -//
#define INPUT_PIN_L 26
#define INPUT_PIN_R 27

// Sampling frequency in Hz
#define SAMPLING_FREQ 44100
// Sample transfer count, per channel
#define SAMPLES 256

int16_t sampleBuffer[SAMPLES * 2]; // DMA Buffer, interlaced L/R
int16_t streamBufferL[SAMPLES]; // Scaled working buffer, split into stereo
int16_t streamBufferR[SAMPLES]; // Scaled working buffer, split into stereo

dma_channel_config config;
int dma_chan;

//- OUTPUT -//
#define OUTPUT_PIN_L 20
#define OUTPUT_PIN_R 21

uint slice_l;
uint slice_r;
uint32_t clockdiv;

uint32_t index_l;
uint32_t index_r;

//- CONTROLS -//
#define NUM_POTS 2
const uint POT_CHANNELS[NUM_POTS] = {2, 3};
uint16_t pot_values[NUM_POTS] = {0, 0};

#define NUM_SWITCHES 3
const uint SWITCH_PINS[NUM_SWITCHES] = {1, 2, 3};
bool switch_states[NUM_SWITCHES] = {false};

//- ALGORITHM -//
#define	NUM_COMBS	8
#define	NUM_ALLPASSES 4
#define MUTED 0
#define FIXED_GAIN 0.015f
#define SCALE_WET 3.0f
#define SCALE_DRY 2.0f
#define SCALE_DAMP 0.4f
#define SCALE_ROOM 0.28f
#define OFFSET_ROOM 0.7f
#define INITIAL_ROOM 0.5f
#define INITIAL_DAMP 0.5f
#define INITIAL_WET (1.0f/SCALE_WET)
#define INITIAL_DRY	0.0f
#define INITIAL_WIDTH 1.0f
#define INITIAL_MODE 0.0f
#define FREEZE_MODE	0.5f
#define STEREO_SPREAD 23

// These values assume 44.1KHz sample rate
// they will probably be OK for 48KHz sample rate
// but would need scaling for 96KHz (or other) sample rates.
// The values were obtained by listening tests.
// Total is around 15000, assuming 4 byte float this is 60KB of memory.
#define COMB_TUNING_L1		 1116
#define COMB_TUNING_R1		 (1116+STEREO_SPREAD)
#define COMB_TUNING_L2		 1188
#define COMB_TUNING_R2		 (1188+STEREO_SPREAD)
#define COMB_TUNING_L3		 1277
#define COMB_TUNING_R3		 (1277+STEREO_SPREAD)
#define COMB_TUNING_L4		 1356
#define COMB_TUNING_R4		 (1356+STEREO_SPREAD)
#define COMB_TUNING_L5		 1422
#define COMB_TUNING_R5		 (1422+STEREO_SPREAD)
#define COMB_TUNING_L6		 1491
#define COMB_TUNING_R6		 (1491+STEREO_SPREAD)
#define COMB_TUNING_L7		 1557
#define COMB_TUNING_R7		 (1557+STEREO_SPREAD)
#define COMB_TUNING_L8		 1617
#define COMB_TUNING_R8		 (1617+STEREO_SPREAD)
#define ALLPASS_TUNING_L1	 556
#define ALLPASS_TUNING_R1	 (556+STEREO_SPREAD)
#define ALLPASS_TUNING_L2	 441
#define ALLPASS_TUNING_R2	 (441+STEREO_SPREAD)
#define ALLPASS_TUNING_L3	 341
#define ALLPASS_TUNING_R3	 (341+STEREO_SPREAD)
#define ALLPASS_TUNING_L4	 225
#define ALLPASS_TUNING_R4	 (225+STEREO_SPREAD)

// Buffers for the combs
float	bufcombL1[COMB_TUNING_L1];
float	bufcombR1[COMB_TUNING_R1];
float	bufcombL2[COMB_TUNING_L2];
float	bufcombR2[COMB_TUNING_R2];
float	bufcombL3[COMB_TUNING_L3];
float	bufcombR3[COMB_TUNING_R3];
float	bufcombL4[COMB_TUNING_L4];
float	bufcombR4[COMB_TUNING_R4];
float	bufcombL5[COMB_TUNING_L5];
float	bufcombR5[COMB_TUNING_R5];
float	bufcombL6[COMB_TUNING_L6];
float	bufcombR6[COMB_TUNING_R6];
float	bufcombL7[COMB_TUNING_L7];
float	bufcombR7[COMB_TUNING_R7];
float	bufcombL8[COMB_TUNING_L8];
float	bufcombR8[COMB_TUNING_R8];

// Buffers for the allpasses
float	bufallpassL1[ALLPASS_TUNING_L1];
float	bufallpassR1[ALLPASS_TUNING_R1];
float	bufallpassL2[ALLPASS_TUNING_L2];
float	bufallpassR2[ALLPASS_TUNING_R2];
float	bufallpassL3[ALLPASS_TUNING_L3];
float	bufallpassR3[ALLPASS_TUNING_R3];
float	bufallpassL4[ALLPASS_TUNING_L4];
float	bufallpassR4[ALLPASS_TUNING_R4];

// COMB BUFFERS
float* l_comb_buffers[NUM_COMBS] = { 
  bufcombL1,
  bufcombL2,
  bufcombL3,
  bufcombL4,
  bufcombL5,
  bufcombL6,
  bufcombL7,
  bufcombL8 
};
const int L_COMB_BUFFER_LENGTHS[NUM_COMBS] = {
  COMB_TUNING_L1,
  COMB_TUNING_L2,
  COMB_TUNING_L3,
  COMB_TUNING_L4,
  COMB_TUNING_L5, 
  COMB_TUNING_L6,
  COMB_TUNING_L7,
  COMB_TUNING_L8
}; 
float* r_comb_buffers[NUM_COMBS] = { 
  bufcombR1,
  bufcombR2,
  bufcombR3,
  bufcombR4,
  bufcombR5,
  bufcombR6,
  bufcombR7,
  bufcombR8 
};
const int R_COMB_BUFFER_LENGTHS[NUM_COMBS] = {
  COMB_TUNING_R1,
  COMB_TUNING_R2,
  COMB_TUNING_R3,
  COMB_TUNING_R4,
  COMB_TUNING_R5, 
  COMB_TUNING_R6,
  COMB_TUNING_R7,
  COMB_TUNING_R8
}; 
// ALLPASS BUFFERS
float* l_allpass_buffers[NUM_COMBS] = {
  bufallpassL1,
  bufallpassL2,
  bufallpassL3,
  bufallpassL4,
};
const int L_ALLPASS_BUFFER_LENGTHS[NUM_ALLPASSES] = {
  ALLPASS_TUNING_L1,
  ALLPASS_TUNING_L2,
  ALLPASS_TUNING_L3,
  ALLPASS_TUNING_L4,
};
float* r_allpass_buffers[NUM_COMBS] = {
  bufallpassR1,
  bufallpassR2,
  bufallpassR3,
  bufallpassR4,
};
const int R_ALLPASS_BUFFER_LENGTHS[NUM_ALLPASSES] = {
  ALLPASS_TUNING_R1,
  ALLPASS_TUNING_R2,
  ALLPASS_TUNING_R3,
  ALLPASS_TUNING_R4,
};

// Params
float comb_feedback_l[NUM_COMBS];
float comb_feedback_r[NUM_COMBS];
float comb_damp_l[NUM_COMBS];
float comb_damp_r[NUM_COMBS];
float comb_filter_store_l[NUM_COMBS];
float comb_filter_store_r[NUM_COMBS];
int comb_buf_idx_l[NUM_COMBS];
int comb_buf_idx_r[NUM_COMBS];

float allpass_feedback_l[NUM_ALLPASSES];
float allpass_feedback_r[NUM_ALLPASSES];
int allpass_buf_idx_l[NUM_ALLPASSES];
int allpass_buf_idx_r[NUM_ALLPASSES];

// Vars
float gain;
float roomsize, roomsize1;
float damp, damp1;
float wet, wet1, wet2;
float dry;
float width;
float mode;

//------ AUDIO INPUT ------//

void init_adc_dma() {
  // INIT ADC
  adc_init();

  adc_gpio_init(INPUT_PIN_L);
  adc_gpio_init(INPUT_PIN_R);

  adc_set_round_robin(0b1100); // Enable first two adc channels (L/R)

  adc_fifo_setup( //
      true,       // Write each completed conversion to sample FIFO
      true,       // Enable dma request (DREQ)
      1,          // Send DREQ when 1 sample present
      false,      // No err flag
      false       // Shift to be 1 byte in size
  );

  adc_set_clkdiv((48000000 / (SAMPLING_FREQ * 2)) - 1);

  // INIT DMA
  dma_chan = dma_claim_unused_channel(true);
  config = dma_channel_get_default_config(dma_chan);

  // Read from first add, write to incrementing.
  channel_config_set_transfer_data_size(&config, DMA_SIZE_16);
  channel_config_set_read_increment(&config, false);
  channel_config_set_write_increment(&config, true);

  channel_config_set_dreq(&config, DREQ_ADC);

  dma_channel_configure(
    dma_chan,
    &config, 
    (uint16_t *)sampleBuffer,
    &adc_hw->fifo, 
    SAMPLES, 
    true
  );

  adc_run(true);
}

//------ PWM OUTPUT ------//

void pwm_int_handler_l() {
  pwm_clear_irq(slice_l);
  if (index_l < SAMPLES) {
    pwm_set_gpio_level(OUTPUT_PIN_L, streamBufferL[index_l]);
    index_l++;
  } else {
    index_l = 0;
  }
}


void pwm_int_handler_r() {
  pwm_clear_irq(slice_r);
  if (index_r < SAMPLES) {
    pwm_set_gpio_level(OUTPUT_PIN_R, streamBufferR[index_r]);
    index_r++;
  } else {
    index_r = 0;
  }
}

void init_pwm() {
  gpio_set_function(OUTPUT_PIN_L, GPIO_FUNC_PWM);
  gpio_set_function(OUTPUT_PIN_R, GPIO_FUNC_PWM);

  slice_l = pwm_gpio_to_slice_num(OUTPUT_PIN_L);
  slice_r = pwm_gpio_to_slice_num(OUTPUT_PIN_R);

  pwm_clear_irq(slice_l);
  pwm_clear_irq(slice_r);
  pwm_set_irq_enabled(slice_l, true);
  pwm_set_irq_enabled(slice_r, true);

  irq_set_exclusive_handler(slice_l, pwm_int_handler_l);
  irq_set_exclusive_handler(slice_r, pwm_int_handler_r);

  irq_set_enabled(slice_l, true);
  irq_set_enabled(slice_r, true);

  // PWM Config
  pwm_config pwm_conf = pwm_get_default_config();

  // Base clock is 125MHz, divide by wrap, then clockdiv divides further (integer increments)
  // We want same pwm freq as sample rate, so need to get a good clockdiv and then optimise wrap from there.
  #define TOP_MAX 65534
  #define DIV_MIN ((0x01 << 4) + 0x0)
  #define DIV_MAX ((0xFF << 4) + 0xF)
  uint32_t clock = 125000000;
  clockdiv = (clock << 4) / SAMPLING_FREQ / (TOP_MAX + 1);
  if (clockdiv < DIV_MIN) {
    clockdiv = DIV_MIN;
  }
  uint32_t top = ((clock << 4) / clockdiv / SAMPLING_FREQ) - 1;

  pwm_config_set_clkdiv(&pwm_conf, clockdiv);
  pwm_config_set_wrap(&pwm_conf, top);

  pwm_init(slice_l, &pwm_conf, true);
  pwm_init(slice_r, &pwm_conf, true);

  pwm_set_gpio_level(OUTPUT_PIN_L, 0);
  pwm_set_gpio_level(OUTPUT_PIN_R, 0);
}

//------ CONTROLS ------//

void init_controls() {
  // POTS
  for (int i = 0; i < NUM_POTS; i++) {
    uint pin = POT_CHANNELS[i] + 26;

    adc_gpio_init(pin);
  }

  // SWITCHES
  for (int i = 0; i < NUM_SWITCHES; i++) {
    gpio_init(SWITCH_PINS[i]);
    gpio_set_dir(SWITCH_PINS[i], false);
  }

}

void read_controls() {
  // POTS
  for (int i = 0; i < NUM_POTS; i++) {
    uint channel = POT_CHANNELS[i];
    adc_select_input(channel);
    const float conversion_factor = 3.3f / (1 << 12);
    uint16_t result = adc_read();
    pot_values[i] = result * conversion_factor;    
  }

  // SWITCHES
  for (int i = 0; i < NUM_SWITCHES; i++) {
    uint pin = SWITCH_PINS[i];
    bool result = gpio_get(pin);
    // Not really debouncing needed
    switch_states[i] = result;
  }
}

//------ COMPUTE ------//
void update_compute_internal();

//- GET / SET -//

void set_roomsize(float value) {
  roomsize = (value * SCALE_ROOM) + OFFSET_ROOM;
  update_compute_internal();
}

float get_roomsize() {
  return (roomsize - OFFSET_ROOM) / SCALE_ROOM;
}

void set_damp(float value) {
  damp = value * SCALE_DAMP;
  update_compute_internal();
}

float get_damp() {
  return damp / SCALE_DAMP;
}

void set_wet(float value) {
  wet = value * SCALE_WET;
  update_compute_internal();
}

float get_wet() {
  return wet / SCALE_WET;
}

void set_dry(float value) {
  dry = value * SCALE_DRY;
}

float get_dry() {
  return dry / SCALE_DRY;
}

void set_width(float value) {
  width = value;
  update_compute_internal();
}

float get_width() {
  return width;
}

void set_mode(float value) {
  mode = value;
  update_compute_internal();
}

float get_mode() {
  if (mode >= FREEZE_MODE) {
    return 1;
  } else {
    return 0;
  }
}

//- COMPUTE MAIN -//
void mute() {
  if (get_mode() >= FREEZE_MODE) {
    return;
  }

  // Empty buffers
  for (int i = 0; i < NUM_COMBS; i++) {
    float* bufferL = l_comb_buffers[i];
    for (int j = 0; j < L_COMB_BUFFER_LENGTHS[i]; j++) {
      bufferL[j] = 0;
    }
    
    float* bufferR = r_comb_buffers[i];
    for (int j = 0; j < R_COMB_BUFFER_LENGTHS[i]; j++) {
      bufferR[j] = 0;
    }
  }

  for (int i = 0; i < NUM_ALLPASSES; i++) {
    float* bufferL = l_allpass_buffers[i];
    for (int j = 0; j < L_ALLPASS_BUFFER_LENGTHS[i]; j++) {
      bufferL[j] = 0;
    }
    
    float* bufferR = r_allpass_buffers[i];
    for (int j = 0; j < R_ALLPASS_BUFFER_LENGTHS[i]; j++) {
      bufferR[j] = 0;
    }
  }
}

void init_compute() {
  for (int i = 0; i < NUM_ALLPASSES; i++) {
    allpass_feedback_l[i] = 0.5f;
    allpass_feedback_r[i] = 0.5f;
  } 
  
  set_wet(INITIAL_WET);
  set_roomsize(INITIAL_ROOM);
  set_dry(INITIAL_DRY);
  set_damp(INITIAL_DAMP);
  set_width(INITIAL_WIDTH);
  set_mode(INITIAL_MODE);

  mute();
}

#define undenormalise(sample) if(((*(unsigned int*)&sample)&0x7f800000)==0) sample=0.0f

inline void process_comb(int index, float input, float* outputL, float* outputR) {
  float outL, outR;

  outL = l_comb_buffers[index][comb_buf_idx_l[index]];
  outR = r_comb_buffers[index][comb_buf_idx_r[index]];
  undenormalise(outL);
  undenormalise(outR);

  comb_filter_store_l[index] = (outL * (1-comb_damp_l[index])) + (comb_filter_store_l[index] * comb_damp_l[index]);
  comb_filter_store_r[index] = (outR * (1-comb_damp_r[index])) + (comb_filter_store_r[index] * comb_damp_r[index]);
  undenormalise(comb_filter_store_l[index]);
  undenormalise(comb_filter_store_r[index]);

  l_comb_buffers[index][comb_buf_idx_l[index]] = input + (comb_filter_store_l[index] * comb_feedback_l[index]);
  r_comb_buffers[index][comb_buf_idx_r[index]] = input + (comb_filter_store_r[index] * comb_feedback_r[index]);

  if (++comb_buf_idx_l[index]>=L_COMB_BUFFER_LENGTHS[index]) comb_buf_idx_l[index] = 0;
  if (++comb_buf_idx_r[index]>=R_COMB_BUFFER_LENGTHS[index]) comb_buf_idx_r[index] = 0;

  *outputL += outL;
  *outputR += outR;
} 

inline void process_allpass(int index, float input, float* outputL, float* outputR) {
  float outL, outR;
  float bufOutL, bufOutR;

  bufOutL = l_allpass_buffers[index][allpass_buf_idx_l[index]];
  bufOutR = r_allpass_buffers[index][allpass_buf_idx_r[index]];
  undenormalise(bufOutL);
  undenormalise(bufOutR);

  outL = -input + bufOutL;
  outR = -input + bufOutR;
  l_allpass_buffers[index][allpass_buf_idx_l[index]] = input + (bufOutL*allpass_feedback_l[index]);
  r_allpass_buffers[index][allpass_buf_idx_r[index]] = input + (bufOutR*allpass_feedback_r[index]);
  
  if (++allpass_buf_idx_l[index]>=L_ALLPASS_BUFFER_LENGTHS[index]) allpass_buf_idx_l[index] = 0;
  if (++allpass_buf_idx_r[index]>=R_ALLPASS_BUFFER_LENGTHS[index]) allpass_buf_idx_r[index] = 0;

  *outputL = outL;
  *outputR = outR;
} 

void compute_mix(float *input_l, float *input_r, float *output_l, float *output_r, long num_samples) {
  float outL, outR, input;
  
  while (num_samples-- > 0) {
    outL = outR = 0;
    input = (*input_l +*input_r) * gain;

    // accumulate comb filters in parallel
    for (int i = 0; i < NUM_COMBS; i++) {
      process_comb(i, input, &outL, &outR);
    }

    // Feed through allpasses in series
    for (int i = 0; i < NUM_ALLPASSES; i++) {
      process_allpass(i, input, &outL, &outR);
    }

    // Mix output
    *output_l += outL*wet1 + outR*wet2 + *input_l*dry;
    *output_r += outR*wet1 + outL*wet2 + *input_r*dry;
  }
}

void update_compute_internal() {
  int i;

  wet1 = wet*(width/2 + 0.5f);
  wet2 = wet*((1-width)/2);

  if (mode >= FREEZE_MODE) {
    roomsize1 = 1;
    damp1 = 0;
    gain = MUTED;
  } else {
    roomsize1 = roomsize;
    damp1 = damp;
    gain = FIXED_GAIN;
  }

  for (int i = 0; i < NUM_COMBS; i++) {
    comb_feedback_l[i] = roomsize1;
    comb_feedback_r[i] = roomsize1;
  }

  for (int i = 0; i < NUM_COMBS; i++) {
    comb_damp_l[i] = damp1;
    comb_damp_r[i] = damp1;
  }
}

void compute(int16_t* inputBufferL, int16_t* inputBufferR) {
  
}

//------ MAIN ------//

void loop() {
  //------ GET INPUT ------//
  // Wait for dma to finish
  dma_channel_wait_for_finish_blocking(dma_chan);
  
  // Stop and clean fifo
  adc_run(false);
  adc_fifo_drain();

  // Copy samples into buffer
  for (int i = 0; i < SAMPLES * 2; i++) {
    if (i % 2 == 0) {
      streamBufferL[i] = sampleBuffer[i]/4 - 512 + 49; // scale to working range: -512 to 512
    } else {
      streamBufferR[i] =  sampleBuffer[i]/4 - 512 + 49; // Scale to working range: -512 to 512.
    }
    
  }

  // Capture controls
  read_controls();

  // Restart capture
  dma_channel_configure(
    dma_chan,
    &config, 
    (uint16_t *)sampleBuffer,
    &adc_hw->fifo, 
    SAMPLES, 
    true
  );
  adc_run(true);

  // Compute
  compute(streamBufferL, streamBufferR);

  //------ OUTPUT ------//
  index_l = 0;
  index_r = 0;
}

int main() {
  init_adc_dma();
  init_controls();
  init_pwm();
  init_compute();

  while (1) {
    loop();
  }
}