/* RAM-backed "peripherals" for the host build (see inc/stm32f4xx.h). */
#define D(x,T) T h_##x;
D(GPIOA,GPIO_TypeDef) D(GPIOB,GPIO_TypeDef) D(GPIOC,GPIO_TypeDef)
D(TIM1,TIM_TypeDef) D(TIM2,TIM_TypeDef) D(TIM3,TIM_TypeDef) D(TIM4,TIM_TypeDef)
D(USART2,USART_TypeDef) D(I2C1,I2C_TypeDef) D(SPI2,SPI_TypeDef) D(ADC1,ADC_TypeDef)
D(RTC,RTC_TypeDef) D(RCC,RCC_TypeDef) D(PWR,PWR_TypeDef) D(EXTI,EXTI_TypeDef) D(SYSCFG,SYSCFG_TypeDef)
D(NVIC,NVIC_Type)
#undef D
