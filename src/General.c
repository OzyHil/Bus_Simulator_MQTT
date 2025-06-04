#include "General.h" // Inclusão da biblioteca geral do sistema
#include "Led_Matrix.h" // Inclusão da biblioteca para controlar a matriz de LEDs

// Função para inicializar a configuração do sistema
void init_system_config()
{
    // Configura o relógio do sistema para 128 kHz (sistema de 32 bits com precisão de tempo)
    set_sys_clock_khz(128000, false);

    configure_leds_matrix(); // Configura a matriz de LEDs
}

// Função para inicializar a funcionalidade PWM em um pino GPIO
void init_pwm(uint gpio, uint wrap)
{
    gpio_set_function(gpio, GPIO_FUNC_PWM); // Define o pino GPIO para a função PWM

    uint slice = pwm_gpio_to_slice_num(gpio); // Obtém o número do slice PWM associado ao pino GPIO

    pwm_set_clkdiv(slice, 16.0); // Define o divisor de clock PWM (controla a velocidade do sinal PWM)

    pwm_set_wrap(slice, wrap); // Define o valor de "wrap", que determina o ciclo completo do PWM

    pwm_set_enabled(slice, true); // Habilita a geração do sinal PWM no slice
}