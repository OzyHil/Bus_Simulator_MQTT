#include "Led_Matrix.h" // Inclusão da biblioteca para controlar a matriz de LEDs

refs pio;

// Matriz que será usada como base para colorir
const int8_t matrix[NUM_PIXELS] = {
    5,
    5,
    5,
    2,
    2,
    2,
    0,
    1,
    0,
    1,
    0,
    4,
    0,
    1,
    0,
    3,
    0,
    1,
    0,
    4,
    4,
    4,
    0,
    3,
    3,
};

void configure_leds_matrix()
{
  pio.ref = pio0;

  pio.state_machine = pio_claim_unused_sm(pio.ref, true);     // Obtém uma máquina de estado livre
  pio.offset = pio_add_program(pio.ref, &pio_matrix_program); // Adiciona o programa da matriz

  pio_matrix_program_init(pio.ref, pio.state_machine, pio.offset, LED_MATRIX); // Inicializa o programa
}

// Converte uma estrutura de cor RGB para um valor 32 bits conforme o protocolo da matriz
uint32_t rgb_matrix(led_color color)
{
  return (color.green << 24) | (color.red << 16) | (color.blue << 8);
}

// Função para desenhar as cores do semáforo
void draw_led_matrix(uint8_t station_line, led_color color)
{
  for (int8_t i = 0; i < NUM_PIXELS; i++)
  {
    uint32_t color_to_send;

    if (matrix[i] == station_line)
    {
      color_to_send = rgb_matrix(color); 
    }
    else if (matrix[i] == 1)
    {
      color_to_send = rgb_matrix(RED); // Cor acesa
    }
    else
    {
      color_to_send = rgb_matrix(DARK); // Cor apagada
    }

    pio_sm_put_blocking(pio.ref, pio.state_machine, color_to_send);
  }
}