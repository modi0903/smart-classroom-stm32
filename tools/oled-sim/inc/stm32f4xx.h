/* Host shim used instead of the real stm32f4xx.h when main.c is compiled on a PC.
 * It keeps ST's register layouts (stm32f401xe.h) but points every peripheral at a
 * plain struct in RAM, so the firmware's drawing and button code can run unchanged. */
#include "stm32f401xe.h"

#define P(x, T) extern T h_##x;
P(GPIOA,GPIO_TypeDef) P(GPIOB,GPIO_TypeDef) P(GPIOC,GPIO_TypeDef)
P(TIM1,TIM_TypeDef) P(TIM2,TIM_TypeDef) P(TIM3,TIM_TypeDef) P(TIM4,TIM_TypeDef)
P(USART2,USART_TypeDef) P(I2C1,I2C_TypeDef) P(SPI2,SPI_TypeDef) P(ADC1,ADC_TypeDef)
P(RTC,RTC_TypeDef) P(RCC,RCC_TypeDef) P(PWR,PWR_TypeDef) P(EXTI,EXTI_TypeDef) P(SYSCFG,SYSCFG_TypeDef)
P(NVIC,NVIC_Type)
#undef P
#undef GPIOA
#undef GPIOB
#undef GPIOC
#undef TIM1
#undef TIM2
#undef TIM3
#undef TIM4
#undef USART2
#undef I2C1
#undef SPI2
#undef ADC1
#undef RTC
#undef RCC
#undef PWR
#undef EXTI
#undef SYSCFG
#undef NVIC
#define GPIOA  (&h_GPIOA)
#define GPIOB  (&h_GPIOB)
#define GPIOC  (&h_GPIOC)
#define TIM1   (&h_TIM1)
#define TIM2   (&h_TIM2)
#define TIM3   (&h_TIM3)
#define TIM4   (&h_TIM4)
#define USART2 (&h_USART2)
#define I2C1   (&h_I2C1)
#define SPI2   (&h_SPI2)
#define ADC1   (&h_ADC1)
#define RTC    (&h_RTC)
#define RCC    (&h_RCC)
#define PWR    (&h_PWR)
#define EXTI   (&h_EXTI)
#define SYSCFG (&h_SYSCFG)
#define NVIC   (&h_NVIC)
#define __disable_irq() ((void)0)
#define __enable_irq()  ((void)0)
