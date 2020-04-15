
#include "at32f4xx.h"
#include "defines.h"
#include "setup.h"
#include "config.h"
#include <stdbool.h>
#include <string.h>

#ifdef CONTROL_PPM
TIM_HandleTypeDef TimHandle;
TIM_HandleTypeDef TimHandleTim3;
uint8_t ppm_count = 0;
#endif // CONTROL_PPM

uint32_t timeout = 100;

#ifdef CONTROL_NUNCHUCK
uint8_t nunchuck_data[6] = {0};

uint8_t i2cBuffer[2];

extern I2C_HandleTypeDef hi2c2;
DMA_HandleTypeDef hdma_i2c2_rx;
DMA_HandleTypeDef hdma_i2c2_tx;
#endif // CONTROL_NUNCHUCK

#ifdef CONTROL_PWM
// Maximum number of PWM channels this code supports, may not be the actual
// number of PWM channels in use
#define MAX_PWM_CHANNELS 8
// Minimum pulse length in microsecond, pulses shorter than this are ignored
#define MIN_PULSE_LENGTH 750
// Maximum pulse length in microsecond, pulses longer than this are ignored
#define MAX_PULSE_LENGTH 2250

static uint32_t last_activity_timestamp[MAX_PWM_CHANNELS];
static bool last_activity_timestamp_valid[MAX_PWM_CHANNELS];
static uint32_t pulse_start_timestamp[MAX_PWM_CHANNELS];
static bool pulse_started[MAX_PWM_CHANNELS];
static bool rc_dead = true;

uint16_t pwm_channel[MAX_PWM_CHANNELS];
#endif // CONTROL_PWM

#ifdef CONTROL_PPM
uint16_t ppm_captured_value[PPM_NUM_CHANNELS + 1] = {500, 500};
uint16_t ppm_captured_value_buffer[PPM_NUM_CHANNELS+1] = {500, 500};
uint32_t ppm_timeout = 0;

bool ppm_valid = true;

#define IN_RANGE(x, low, up) (((x) >= (low)) && ((x) <= (up)))

void PPM_ISR_Callback() {
	static uint8_t skipNext = 0;
	static uint32_t frameStart;
  // Dummy loop with 16 bit count wrap around
  uint16_t rc_delay = TIM2->CNT;
  TIM2->CNT = 0;

  if (rc_delay > 3000) {
	  //start of frame
	  TIM3->CNT = 0;
    if (ppm_valid && ppm_count == PPM_NUM_CHANNELS) {
      ppm_timeout = 0;
      memcpy(ppm_captured_value, ppm_captured_value_buffer, sizeof(ppm_captured_value));
    }
    ppm_valid = true;
    ppm_count = 0;
	skipNext = 0;
  }
  else if (ppm_count < PPM_NUM_CHANNELS){
	  if(IN_RANGE(rc_delay, 900, 2100)){
		timeout = 0;
		if(skipNext){	//We received an incomplete frame and propably see the second half of it
			if((TIM3->CNT - frameStart) < (ppm_captured_value_buffer[ppm_count]/10 + 100)){
				//We can expect the value to not change that suddenly
				//So if we see the next value aprox after the time the last channel took
				//We can be safe this channel is finished
				ppm_count++;
				skipNext = 0;
			} else {
				//we have more disturbance in our signal or we lost track of the channels
				skipNext = 1;
			}
		} else {
			ppm_captured_value_buffer[ppm_count++] = CLAMP(rc_delay, 1000, 2000) - 1000;
		}
	  } 
	  else {
		  if(TIM3->CNT > 2000){
			  //There is some error and we received something way after the start 
			  ppm_valid = false;
			  ppm_count = 0;
		  }
		  if(rc_delay < 900) {
			  frameStart = TIM3->CNT - rc_delay/10;	//The start of the broken frame
			  skipNext = 1;
		  }
	  }
  } else {
    ppm_valid = false;
  }
}

// SysTick executes once each ms
void PPM_SysTick_Callback() {
  ppm_timeout++;
  // Stop after 500 ms without PPM signal
  if(ppm_timeout > 500) {
    int i;
    for(i = 0; i < PPM_NUM_CHANNELS; i++) {
      ppm_captured_value[i] = 500;
    }
    ppm_timeout = 0;
  }
}

void PPM_Init() {
  GPIO_InitTypeDef GPIO_InitStruct;
  /*Configure GPIO pin : PA3 */
  GPIO_InitStruct.Pin = GPIO_PIN_3;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  __HAL_RCC_TIM2_CLK_ENABLE();
  TimHandle.Instance = TIM2;
  TimHandle.Init.Period = UINT16_MAX;
  TimHandle.Init.Prescaler = (HAL_RCC_GetHCLKFreq()/DELAY_TIM_FREQUENCY_US)-1;
  TimHandle.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  TimHandle.Init.CounterMode = TIM_COUNTERMODE_UP;
  HAL_TIM_Base_Init(&TimHandle);
  __HAL_TIM_ENABLE(&TimHandle);
  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI3_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI3_IRQn);
  HAL_TIM_Base_Start(&TimHandle);
  
  __HAL_RCC_TIM3_CLK_ENABLE();
  TimHandleTim3.Instance = TIM3;
  TimHandleTim3.Init.Period = UINT16_MAX;
  TimHandleTim3.Init.Prescaler = (HAL_RCC_GetHCLKFreq()/100000)-1;	//10us
  TimHandleTim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  TimHandleTim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  HAL_TIM_Base_Init(&TimHandleTim3);
  __HAL_TIM_ENABLE(&TimHandleTim3);
  HAL_TIM_Base_Start(&TimHandleTim3);
}
#endif // CONTROL_PPM

#ifdef CONTROL_PWM
void PWM_EXTI_Callback(unsigned int channel, unsigned int level) {
  uint32_t current_timestamp = TMR_GetCounter(TMR2); // in microseconds
  if (channel < MAX_PWM_CHANNELS) {

    if (rc_dead) {
      // Got RC signal back (or it just started), initialise everything
      for (int i = 0; i < MAX_PWM_CHANNELS; i++) {
        pwm_channel[i] = PWM_CENTER;
        pulse_started[i] = false;
        last_activity_timestamp_valid[i] = false;
      }
    }

    if (level) {
      // Start of pulse
      pulse_start_timestamp[channel] = current_timestamp;
      pulse_started[channel] = true;
    } else {
      if (pulse_started[channel]) {
        // End of pulse
        uint32_t pulse_length = current_timestamp - pulse_start_timestamp[channel];
        // Only accept pulse if it isn't too short or too long. Pulses outside
        // the acceptable range are probably glitches due to noise.
        // Don't write PWM_CENTER to pwm_channel for invalid pulses
        // because it's probably just a temporary glitch.
        if ((pulse_length >= MIN_PULSE_LENGTH) && (pulse_length <= MAX_PULSE_LENGTH)) {
          pwm_channel[channel] = pulse_length;
        }
        pulse_started[channel] = false;
      }
    }
    last_activity_timestamp_valid[channel] = true;
    last_activity_timestamp[channel] = current_timestamp;
    rc_dead = false;
  }
}

// You must call this before reading from pwm_channel, to check for validity.
// Returns true if pwm_channel values are valid,
// returns false if RC is dead.
bool PWM_Read() {
  uint32_t current_timestamp = TMR_GetCounter(TMR2); // in microseconds

  if (!rc_dead) {
    // Check if all channels have died, this indicates dead RC
    rc_dead = true; // will become false if any channel has activity
    for (int i = 0; i < MAX_PWM_CHANNELS; i++) {
      if (last_activity_timestamp_valid[i]
        && ((current_timestamp - last_activity_timestamp[i]) <= (PWM_TIMEOUT * 1000UL))) {
        rc_dead = false;
        break;
      }
    }
    // Check for pulse timeout on individual channels
    for (int i = 0; i < MAX_PWM_CHANNELS; i++) {
      if (last_activity_timestamp_valid[i]
        && ((current_timestamp - last_activity_timestamp[i]) > (PWM_TIMEOUT * 1000UL))) {
        // Channel is stuck, maybe a connection came loose?
        // For safety, set channel pulse length to center.
        pwm_channel[i] = PWM_CENTER;
        pulse_started[i] = false;
        last_activity_timestamp_valid[i] = false;
      }
    }
  }
  return !rc_dead;
}
#endif // CONTROL_PWM

#ifdef CONTROL_NUNCHUCK
void Nunchuck_Init() {
    //-- START -- init WiiNunchuck
  i2cBuffer[0] = 0xF0;
  i2cBuffer[1] = 0x55;

  HAL_I2C_Master_Transmit(&hi2c2,0xA4,(uint8_t*)i2cBuffer, 2, 100);
  HAL_Delay(10);

  i2cBuffer[0] = 0xFB;
  i2cBuffer[1] = 0x00;

  HAL_I2C_Master_Transmit(&hi2c2,0xA4,(uint8_t*)i2cBuffer, 2, 100);
  HAL_Delay(10);
}

void Nunchuck_Read() {
  i2cBuffer[0] = 0x00;
  HAL_I2C_Master_Transmit(&hi2c2,0xA4,(uint8_t*)i2cBuffer, 1, 100);
  HAL_Delay(5);
  if (HAL_I2C_Master_Receive(&hi2c2,0xA4,(uint8_t*)nunchuck_data, 6, 100) == HAL_OK) {
    timeout = 0;
  } else {
    timeout++;
  }

  if (timeout > 3) {
    HAL_Delay(50);
    Nunchuck_Init();
  }

  //setScopeChannel(0, (int)nunchuck_data[0]);
  //setScopeChannel(1, (int)nunchuck_data[1]);
  //setScopeChannel(2, (int)nunchuck_data[5] & 1);
  //setScopeChannel(3, ((int)nunchuck_data[5] >> 1) & 1);
}
#endif // CONTROL_NUNCHUCK
