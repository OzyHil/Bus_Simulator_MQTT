// Código adaptado de: https://github.com/raspberrypi/pico-examples/tree/master/pico_w/wifi/mqtt

#include "pico/stdlib.h"     // Biblioteca da Raspberry Pi Pico para funções padrão (GPIO, temporização, etc.)
#include "pico/cyw43_arch.h" // Biblioteca para arquitetura Wi-Fi da Pico com CYW43
#include "pico/unique_id.h"  // Biblioteca com recursos para trabalhar com os pinos GPIO do Raspberry Pi Pico

#include "hardware/gpio.h" // Biblioteca de hardware de GPIO
#include "hardware/irq.h"  // Biblioteca de hardware de interrupções
#include "hardware/adc.h"  // Biblioteca de hardware para conversão ADC

#include "lwip/apps/mqtt.h"      // Biblioteca LWIP MQTT -  fornece funções e recursos para conexão MQTT
#include "lwip/apps/mqtt_priv.h" // Biblioteca que fornece funções e recursos para Geração de Conexões
#include "lwip/dns.h"            // Biblioteca que fornece funções e recursos suporte DNS:
#include "lwip/altcp_tls.h"      // Biblioteca que fornece funções e recursos para conexões seguras usando TLS:

#include "Led_Matrix.h"

#define WIFI_SSID "TSUNAMI_EVERALDO"   // Substitua pelo nome da sua rede Wi-Fi
#define WIFI_PASSWORD "amizade5560" // Substitua pela senha da sua rede Wi-Fi
#define MQTT_SERVER "192.168.0.100"   // Substitua pelo endereço do host - broket MQTT: Ex: 192.168.1.107
#define MQTT_USERNAME "hilquias"     // Substitua pelo nome da host MQTT - Username
#define MQTT_PASSWORD "hilquias"     // Substitua pelo Password da host MQTT - credencial de acesso - caso exista

// Definição da escala de temperatura
#ifndef TEMPERATURE_UNITS
#define TEMPERATURE_UNITS 'C' // Set to 'F' for Fahrenheit
#endif

#ifndef MQTT_SERVER
#error Need to define MQTT_SERVER
#endif

// This file includes your client certificate for client server authentication
#ifdef MQTT_CERT_INC
#include MQTT_CERT_INC
#endif

#ifndef MQTT_TOPIC_LEN
#define MQTT_TOPIC_LEN 100
#endif

// Dados do cliente MQTT
typedef struct
{
    mqtt_client_t *mqtt_client_inst;
    struct mqtt_connect_client_info_t mqtt_client_info;
    char data[MQTT_OUTPUT_RINGBUF_SIZE];
    char topic[MQTT_TOPIC_LEN];
    uint32_t len;
    ip_addr_t mqtt_server_address;
    bool connect_done;
    int subscribe_count;
    bool stop_client;
} MQTT_CLIENT_DATA_T;

#ifndef DEBUG_printf
#ifndef NDEBUG
#define DEBUG_printf printf
#else
#define DEBUG_printf(...)
#endif
#endif

#ifndef INFO_printf
#define INFO_printf printf
#endif

#ifndef ERROR_printf
#define ERROR_printf printf
#endif

// Temporização da coleta de temperatura - how often to measure our temperature
#define TEMP_WORKER_TIME_S 10

// Manter o programa ativo - keep alive in seconds
#define MQTT_KEEP_ALIVE_S 60

// QoS - mqtt_subscribe
// At most once (QoS 0)
// At least once (QoS 1)
// Exactly once (QoS 2)
#define MQTT_SUBSCRIBE_QOS 1
#define MQTT_PUBLISH_QOS 1
#define MQTT_PUBLISH_RETAIN 0

// Tópico usado para: last will and testament
#define MQTT_WILL_TOPIC "/online"
#define MQTT_WILL_MSG "0"
#define MQTT_WILL_QOS 1

#ifndef MQTT_DEVICE_NAME
#define MQTT_DEVICE_NAME "pico"
#endif

// Definir como 1 para adicionar o nome do cliente aos tópicos, para suportar vários dispositivos que utilizam o mesmo servidor
#ifndef MQTT_UNIQUE_TOPIC
#define MQTT_UNIQUE_TOPIC 0
#endif

#define STATION_1 0
#define STATION_2 4
#define STATION_3 7
#define STATION_4 12
#define DISTANCE 16

// Dados do cliente MQTT
typedef struct
{
    uint8_t posicao;
    char linha_atual[20];
} bus_station;

bus_station bus;

/* References for this implementation:
 * raspberry-pi-pico-c-sdk.pdf, Section '4.1.1. hardware_adc'
 * pico-examples/adc/adc_console/adc_console.c */

// Leitura de temperatura do microcotrolador
static float read_onboard_temperature(const char unit);

// Requisição para publicar
static void pub_request_cb(__unused void *arg, err_t err);

// Topico MQTT
static const char *full_topic(MQTT_CLIENT_DATA_T *state, const char *name);

// Controle do LED
static void control_led(MQTT_CLIENT_DATA_T *state, bool on);

// Controle do bus
static void control_bus(MQTT_CLIENT_DATA_T *state, bool on);

// Publicar temperatura
static void publish_temperature(MQTT_CLIENT_DATA_T *state);

// Requisição de Assinatura - subscribe
static void sub_request_cb(void *arg, err_t err);

// Requisição para encerrar a assinatura
static void unsub_request_cb(void *arg, err_t err);

// Tópicos de assinatura
static void sub_unsub_topics(MQTT_CLIENT_DATA_T *state, bool sub);

// Dados de entrada MQTT
static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags);

// Dados de entrada publicados
static void mqtt_incoming_publish_cb(void *arg, const char *topic, u32_t tot_len);

// Publicar temperatura
static void temperature_worker_fn(async_context_t *context, async_at_time_worker_t *worker);
static async_at_time_worker_t temperature_worker = {.do_work = temperature_worker_fn};

// Conexão MQTT
static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status);

// Inicializar o cliente MQTT
static void start_client(MQTT_CLIENT_DATA_T *state);

// Call back com o resultado do DNS
static void dns_found(const char *hostname, const ip_addr_t *ipaddr, void *arg);

int main(void)
{

    // Inicializa todos os tipos de bibliotecas stdio padrão presentes que estão ligados ao binário.
    stdio_init_all();
    INFO_printf("mqtt client starting\n");

    // Inicializa o conversor ADC
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(4);

    // Cria registro com os dados do cliente
    static MQTT_CLIENT_DATA_T state;

    // Inicializa a arquitetura do cyw43
    if (cyw43_arch_init())
    {
        panic("Failed to inizialize CYW43");
    }

    // Usa identificador único da placa
    char unique_id_buf[5];
    pico_get_unique_board_id_string(unique_id_buf, sizeof(unique_id_buf));
    for (int i = 0; i < sizeof(unique_id_buf) - 1; i++)
    {
        unique_id_buf[i] = tolower(unique_id_buf[i]);
    }

    // Gera nome único, Ex: pico1234
    char client_id_buf[sizeof(MQTT_DEVICE_NAME) + sizeof(unique_id_buf) - 1];
    memcpy(&client_id_buf[0], MQTT_DEVICE_NAME, sizeof(MQTT_DEVICE_NAME) - 1);
    memcpy(&client_id_buf[sizeof(MQTT_DEVICE_NAME) - 1], unique_id_buf, sizeof(unique_id_buf) - 1);
    client_id_buf[sizeof(client_id_buf) - 1] = 0;
    INFO_printf("Device name %s\n", client_id_buf);

    state.mqtt_client_info.client_id = client_id_buf;
    state.mqtt_client_info.keep_alive = MQTT_KEEP_ALIVE_S; // Keep alive in sec
#if defined(MQTT_USERNAME) && defined(MQTT_PASSWORD)
    state.mqtt_client_info.client_user = MQTT_USERNAME;
    state.mqtt_client_info.client_pass = MQTT_PASSWORD;
#else
    state.mqtt_client_info.client_user = NULL;
    state.mqtt_client_info.client_pass = NULL;
#endif
    static char will_topic[MQTT_TOPIC_LEN];
    strncpy(will_topic, full_topic(&state, MQTT_WILL_TOPIC), sizeof(will_topic));
    state.mqtt_client_info.will_topic = will_topic;
    state.mqtt_client_info.will_msg = MQTT_WILL_MSG;
    state.mqtt_client_info.will_qos = MQTT_WILL_QOS;
    state.mqtt_client_info.will_retain = true;
#if LWIP_ALTCP && LWIP_ALTCP_TLS
    // TLS enabled
#ifdef MQTT_CERT_INC
    static const uint8_t ca_cert[] = TLS_ROOT_CERT;
    static const uint8_t client_key[] = TLS_CLIENT_KEY;
    static const uint8_t client_cert[] = TLS_CLIENT_CERT;
    // This confirms the indentity of the server and the client
    state.mqtt_client_info.tls_config = altcp_tls_create_config_client_2wayauth(ca_cert, sizeof(ca_cert),
                                                                                client_key, sizeof(client_key), NULL, 0, client_cert, sizeof(client_cert));
#if ALTCP_MBEDTLS_AUTHMODE != MBEDTLS_SSL_VERIFY_REQUIRED
    WARN_printf("Warning: tls without verification is insecure\n");
#endif
#else
    state->client_info.tls_config = altcp_tls_create_config_client(NULL, 0);
    WARN_printf("Warning: tls without a certificate is insecure\n");
#endif
#endif

    // Conectar à rede WiFI - fazer um loop até que esteja conectado
    cyw43_arch_enable_sta_mode();
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000))
    {
        panic("Failed to connect");
    }
    INFO_printf("\nConnected to Wifi\n");

    // Faz um pedido de DNS para o endereço IP do servidor MQTT
    cyw43_arch_lwip_begin();
    int err = dns_gethostbyname(MQTT_SERVER, &state.mqtt_server_address, dns_found, &state);
    cyw43_arch_lwip_end();

    // Se tiver o endereço, inicia o cliente
    if (err == ERR_OK)
    {
        start_client(&state);
    }
    else if (err != ERR_INPROGRESS)
    { // ERR_INPROGRESS means expect a callback
        panic("dns request failed");
    }

    configure_leds_matrix(); // Configura a matriz de LEDs
    draw_led_matrix(0, DARK); // Limpa a matriz de LEDs

    // Loop condicionado a conexão mqtt
    while (!state.connect_done || mqtt_client_is_connected(state.mqtt_client_inst))
    {
        // draw_led_matrix(5, MAGENTA); // Desenha a matriz de LEDs

        cyw43_arch_poll();
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(10000));
    }

    INFO_printf("mqtt client exiting\n");
    return 0;
}

/* References for this implementation:
 * raspberry-pi-pico-c-sdk.pdf, Section '4.1.1. hardware_adc'
 * pico-examples/adc/adc_console/adc_console.c */
static float read_onboard_temperature(const char unit)
{

    /* 12-bit conversion, assume max value == ADC_VREF == 3.3 V */
    const float conversionFactor = 3.3f / (1 << 12);

    float adc = (float)adc_read() * conversionFactor;
    float tempC = 27.0f - (adc - 0.706f) / 0.001721f;

    if (unit == 'C' || unit != 'F')
    {
        return tempC;
    }
    else if (unit == 'F')
    {
        return tempC * 9 / 5 + 32;
    }

    return -1.0f;
}

// Requisição para publicar
static void pub_request_cb(__unused void *arg, err_t err)
{
    if (err != 0)
    {
        ERROR_printf("pub_request_cb failed %d", err);
    }
}

// Topico MQTT
static const char *full_topic(MQTT_CLIENT_DATA_T *state, const char *name)
{
#if MQTT_UNIQUE_TOPIC
    static char full_topic[MQTT_TOPIC_LEN];
    snprintf(full_topic, sizeof(full_topic), "/%s%s", state->mqtt_client_info.client_id, name);
    return full_topic;
#else
    return name;
#endif
}

// Controle do LED
static void control_led(MQTT_CLIENT_DATA_T *state, bool on)
{
    // Publish state on /state topic and on/off led board
    const char *message = on ? "On" : "Off";
    if (on)
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    else
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);

    // mqtt_publish(state->mqtt_client_inst, full_topic(state, "/led/state"), message, strlen(message), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);
}

// Controle do Onibus
static void control_bus(MQTT_CLIENT_DATA_T *state, bool on)
{
    if (on)
    {
        if (bus.posicao < DISTANCE)
            bus.posicao++;
        else
            bus.posicao = 0;
    }

    if ((bus.posicao >= STATION_4))
    {
        sprintf(bus.linha_atual, "Linha Rosa");
        draw_led_matrix(5, MAGENTA);
    }
    else if ((bus.posicao >= STATION_3))
    {
        sprintf(bus.linha_atual, "Linha Verde");
        draw_led_matrix(4, GREEN);
    }
    else if ((bus.posicao >= STATION_2))
    {
        sprintf(bus.linha_atual, "Linha Azul");
        draw_led_matrix(3, BLUE);
    }
    else
    {
        sprintf(bus.linha_atual, "Linha Amarela");
        draw_led_matrix(2, YELLOW);
    }

    char bus_position[10];
    char station_line[20];

    snprintf(bus_position, sizeof(bus_position), "%d", bus.posicao);
    snprintf(station_line, sizeof(station_line), "%s", bus.linha_atual);

    mqtt_publish(state->mqtt_client_inst, full_topic(state, "/bus/position"), bus_position, strlen(bus_position), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);
    mqtt_publish(state->mqtt_client_inst, full_topic(state, "/bus/station_line"), station_line, strlen(station_line), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);
}

// Publicar temperatura
static void publish_temperature(MQTT_CLIENT_DATA_T *state)
{
    static float old_temperature;
    const char *temperature_key = full_topic(state, "/temperature");
    float temperature = read_onboard_temperature(TEMPERATURE_UNITS);
    if (temperature != old_temperature)
    {
        old_temperature = temperature;
        // Publish temperature on /temperature topic
        char temp_str[16];
        snprintf(temp_str, sizeof(temp_str), "%.2f", temperature);
        INFO_printf("Publishing %s to %s\n", temp_str, temperature_key);
        mqtt_publish(state->mqtt_client_inst, temperature_key, temp_str, strlen(temp_str), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);
    }
}

// Requisição de Assinatura - subscribe
static void sub_request_cb(void *arg, err_t err)
{
    MQTT_CLIENT_DATA_T *state = (MQTT_CLIENT_DATA_T *)arg;
    if (err != 0)
    {
        panic("subscribe request failed %d", err);
    }
    state->subscribe_count++;
}

// Requisição para encerrar a assinatura
static void unsub_request_cb(void *arg, err_t err)
{
    MQTT_CLIENT_DATA_T *state = (MQTT_CLIENT_DATA_T *)arg;
    if (err != 0)
    {
        panic("unsubscribe request failed %d", err);
    }
    state->subscribe_count--;
    assert(state->subscribe_count >= 0);

    // Stop if requested
    if (state->subscribe_count <= 0 && state->stop_client)
    {
        mqtt_disconnect(state->mqtt_client_inst);
    }
}

// Tópicos de assinatura
static void sub_unsub_topics(MQTT_CLIENT_DATA_T *state, bool sub)
{
    mqtt_request_cb_t cb = sub ? sub_request_cb : unsub_request_cb;
    mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/led"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
    mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/print"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
    mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/ping"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
    mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/exit"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
    mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/bus"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
}

// Dados de entrada MQTT
static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags)
{
    MQTT_CLIENT_DATA_T *state = (MQTT_CLIENT_DATA_T *)arg;
#if MQTT_UNIQUE_TOPIC
    const char *basic_topic = state->topic + strlen(state->mqtt_client_info.client_id) + 1;
#else
    const char *basic_topic = state->topic;
#endif
    strncpy(state->data, (const char *)data, len);
    state->len = len;
    state->data[len] = '\0';

    DEBUG_printf("Topic: %s, Message: %s\n", state->topic, state->data);
    if (strcmp(basic_topic, "/led") == 0)
    {
        if (lwip_stricmp((const char *)state->data, "On") == 0 || strcmp((const char *)state->data, "1") == 0)
            control_led(state, true);
        else if (lwip_stricmp((const char *)state->data, "Off") == 0 || strcmp((const char *)state->data, "0") == 0)
            control_led(state, false);
    }
    else if (strcmp(basic_topic, "/bus") == 0)
    {
        if (lwip_stricmp((const char *)state->data, "Go") == 0 || strcmp((const char *)state->data, "1") == 0)
            control_bus(state, true);
        else if (lwip_stricmp((const char *)state->data, "Stop") == 0 || strcmp((const char *)state->data, "0") == 0)
            control_bus(state, false);
    }
    else if (strcmp(basic_topic, "/print") == 0)
    {
        INFO_printf("%.*s\n", len, data);
    }
    else if (strcmp(basic_topic, "/ping") == 0)
    {
        char buf[11];
        snprintf(buf, sizeof(buf), "%u", to_ms_since_boot(get_absolute_time()) / 1000);
        mqtt_publish(state->mqtt_client_inst, full_topic(state, "/uptime"), buf, strlen(buf), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);
    }
    else if (strcmp(basic_topic, "/exit") == 0)
    {
        state->stop_client = true;      // stop the client when ALL subscriptions are stopped
        sub_unsub_topics(state, false); // unsubscribe
    }
}

// Dados de entrada publicados
static void mqtt_incoming_publish_cb(void *arg, const char *topic, u32_t tot_len)
{
    MQTT_CLIENT_DATA_T *state = (MQTT_CLIENT_DATA_T *)arg;
    strncpy(state->topic, topic, sizeof(state->topic));
}

// Publicar temperatura
static void temperature_worker_fn(async_context_t *context, async_at_time_worker_t *worker)
{
    MQTT_CLIENT_DATA_T *state = (MQTT_CLIENT_DATA_T *)worker->user_data;
    publish_temperature(state);
    async_context_add_at_time_worker_in_ms(context, worker, TEMP_WORKER_TIME_S * 1000);
}

// Conexão MQTT
static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status)
{
    MQTT_CLIENT_DATA_T *state = (MQTT_CLIENT_DATA_T *)arg;
    if (status == MQTT_CONNECT_ACCEPTED)
    {
        state->connect_done = true;
        sub_unsub_topics(state, true); // subscribe;

        // indicate online
        if (state->mqtt_client_info.will_topic)
        {
            mqtt_publish(state->mqtt_client_inst, state->mqtt_client_info.will_topic, "1", 1, MQTT_WILL_QOS, true, pub_request_cb, state);
        }

        // Publish temperature every 10 sec if it's changed
        temperature_worker.user_data = state;
        async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &temperature_worker, 0);
    }
    else if (status == MQTT_CONNECT_DISCONNECTED)
    {
        if (!state->connect_done)
        {
            panic("Failed to connect to mqtt server");
        }
    }
    else
    {
        panic("Unexpected status");
    }
}

// Inicializar o cliente MQTT
static void start_client(MQTT_CLIENT_DATA_T *state)
{
#if LWIP_ALTCP && LWIP_ALTCP_TLS
    const int port = MQTT_TLS_PORT;
    INFO_printf("Using TLS\n");
#else
    const int port = MQTT_PORT;
    INFO_printf("Warning: Not using TLS\n");
#endif

    state->mqtt_client_inst = mqtt_client_new();
    if (!state->mqtt_client_inst)
    {
        panic("MQTT client instance creation error");
    }
    INFO_printf("IP address of this device %s\n", ipaddr_ntoa(&(netif_list->ip_addr)));
    INFO_printf("Connecting to mqtt server at %s\n", ipaddr_ntoa(&state->mqtt_server_address));

    cyw43_arch_lwip_begin();
    if (mqtt_client_connect(state->mqtt_client_inst, &state->mqtt_server_address, port, mqtt_connection_cb, state, &state->mqtt_client_info) != ERR_OK)
    {
        panic("MQTT broker connection error");
    }
#if LWIP_ALTCP && LWIP_ALTCP_TLS
    // This is important for MBEDTLS_SSL_SERVER_NAME_INDICATION
    mbedtls_ssl_set_hostname(altcp_tls_context(state->mqtt_client_inst->conn), MQTT_SERVER);
#endif
    mqtt_set_inpub_callback(state->mqtt_client_inst, mqtt_incoming_publish_cb, mqtt_incoming_data_cb, state);
    cyw43_arch_lwip_end();
}

// Call back com o resultado do DNS
static void dns_found(const char *hostname, const ip_addr_t *ipaddr, void *arg)
{
    MQTT_CLIENT_DATA_T *state = (MQTT_CLIENT_DATA_T *)arg;
    if (ipaddr)
    {
        state->mqtt_server_address = *ipaddr;
        start_client(state);
    }
    else
    {
        panic("dns request failed");
    }
}

// // Código adaptado de: https://github.com/raspberrypi/pico-examples/tree/master/pico_w/wifi/mqtt

// #include "pico/stdlib.h"            // Biblioteca da Raspberry Pi Pico para funções padrão (GPIO, temporização, etc.)
// #include "pico/cyw43_arch.h"        // Biblioteca para arquitetura Wi-Fi da Pico com CYW43
// #include "pico/unique_id.h"         // Biblioteca com recursos para trabalhar com os pinos GPIO do Raspberry Pi Pico

// #include "hardware/gpio.h"          // Biblioteca de hardware de GPIO
// #include "hardware/irq.h"           // Biblioteca de hardware de interrupções
// #include "hardware/adc.h"           // Biblioteca de hardware para conversão ADC

// #include "lwip/apps/mqtt.h"         // Biblioteca LWIP MQTT -  fornece funções e recursos para conexão MQTT
// #include "lwip/apps/mqtt_priv.h"    // Biblioteca que fornece funções e recursos para Geração de Conexões
// #include "lwip/dns.h"               // Biblioteca que fornece funções e recursos suporte DNS:
// #include "lwip/altcp_tls.h"         // Biblioteca que fornece funções e recursos para conexões seguras usando TLS:

// #include "Led_Matrix.h"             // Certifique-se de que este arquivo esteja no seu projeto

// #define WIFI_SSID "TSUNAMI_EVERALDO"                  // Substitua pelo nome da sua rede Wi-Fi
// #define WIFI_PASSWORD "amizade5560"      // Substitua pela senha da sua rede Wi-Fi
// #define MQTT_SERVER "192.168.0.100"                // Substitua pelo endereço do host - broket MQTT: Ex: 192.168.1.107
// #define MQTT_USERNAME "hilquias"     // Substitua pelo nome da host MQTT - Username
// #define MQTT_PASSWORD "hilquias"     // Substitua pelo Password da host MQTT - credencial de acesso - caso exista

// // Definição da escala de temperatura
// #ifndef TEMPERATURE_UNITS
// #define TEMPERATURE_UNITS 'C' // Set to 'F' for Fahrenheit
// #endif

// #ifndef MQTT_SERVER
// #error Need to define MQTT_SERVER
// #endif

// // This file includes your client certificate for client server authentication
// #ifdef MQTT_CERT_INC
// #include MQTT_CERT_INC
// #endif

// #ifndef MQTT_TOPIC_LEN
// #define MQTT_TOPIC_LEN 100
// #endif

// // Dados do cliente MQTT
// typedef struct {
//     mqtt_client_t* mqtt_client_inst;
//     struct mqtt_connect_client_info_t mqtt_client_info;
//     char data[MQTT_OUTPUT_RINGBUF_SIZE];
//     char topic[MQTT_TOPIC_LEN];
//     uint32_t len;
//     ip_addr_t mqtt_server_address;
//     bool connect_done;
//     int subscribe_count;
//     bool stop_client;
// } MQTT_CLIENT_DATA_T;

// #ifndef DEBUG_printf
// #ifndef NDEBUG
// #define DEBUG_printf printf
// #else
// #define DEBUG_printf(...)
// #endif
// #endif

// #ifndef INFO_printf
// #define INFO_printf printf
// #endif

// #ifndef ERROR_printf
// #define ERROR_printf printf
// #endif

// // Temporização da coleta de temperatura - how often to measure our temperature
// #define TEMP_WORKER_TIME_S 10

// // Manter o programa ativo - keep alive in seconds
// #define MQTT_KEEP_ALIVE_S 120

// // QoS - mqtt_subscribe
// // At most once (QoS 0)
// // At least once (QoS 1)
// // Exactly once (QoS 2)
// #define MQTT_SUBSCRIBE_QOS 1
// #define MQTT_PUBLISH_QOS 1
// #define MQTT_PUBLISH_RETAIN 0

// // Tópico usado para: last will and testament
// #define MQTT_WILL_TOPIC "/online"
// #define MQTT_WILL_MSG "1" // Alterado para "1" para indicar que o cliente está online
// #define MQTT_WILL_QOS 1

// #ifndef MQTT_DEVICE_NAME
// #define MQTT_DEVICE_NAME "pico"
// #endif

// // Definir como 1 para adicionar o nome do cliente aos tópicos, para suportar vários dispositivos que utilizam o mesmo servidor
// #ifndef MQTT_UNIQUE_TOPIC
// #define MQTT_UNIQUE_TOPIC 0
// #endif

// /* References for this implementation:
//  * raspberry-pi-pico-c-sdk.pdf, Section '4.1.1. hardware_adc'
//  * pico-examples/adc/adc_console/adc_console.c */

// // Definições para a simulação do ônibus
// #define NUM_PONTOS 4
// #define DISTANCIA_INICIAL 100.0f
// #define VELOCIDADE_PADRAO 50.0f
// #define PROXIMO_PONTO_THRESHOLD 5.0f

// // Tópicos MQTT
// #define BUS_COMANDO "bus/comando"
// #define BUS_STATUS "bus/status"
// #define BUS_PONTO1_DISTANCIA "bus/ponto1/distancia"
// #define BUS_PONTO2_DISTANCIA "bus/ponto2/distancia"
// #define BUS_PONTO3_DISTANCIA "bus/ponto3/distancia"
// #define BUS_PONTO4_DISTANCIA "bus/ponto4/distancia"
// #define BUS_PONTO_ATUAL "bus/pontoAtual"
// #define BUS_VELOCIDADE "bus/velocidade"

// // Estrutura de Dados do Ônibus
// typedef struct {
//     int ponto_atual;
//     float distancias[NUM_PONTOS];
//     float velocidade;
//     bool em_movimento;
// } BusState;

// // Variáveis Globais
// static BusState bus;

// // Protótipos de Funções
// static void mqtt_process_message(MQTT_CLIENT_DATA_T *state, const char *topic, const char *payload);
// static void update_led_matrix(void);
// static void publish_bus_data(MQTT_CLIENT_DATA_T *state);
// static void sub_unsub_topics(MQTT_CLIENT_DATA_T* state, bool sub);

// // Callback para mensagens MQTT recebidas
// static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags);

// //Leitura de temperatura do microcotrolador
// static float read_onboard_temperature(const char unit);

// // Requisição para publicar
// static void pub_request_cb(__unused void *arg, err_t err);

// // Topico MQTT
// static const char *full_topic(MQTT_CLIENT_DATA_T *state, const char *name);

// // Controle do LED
// static void control_led(MQTT_CLIENT_DATA_T *state, bool on);

// // Publicar temperatura
// static void publish_temperature(MQTT_CLIENT_DATA_T *state);

// // Requisição de Assinatura - subscribe
// static void sub_request_cb(void *arg, err_t err);

// // Requisição para encerrar a assinatura
// static void unsub_request_cb(void *arg, err_t err);

// // Publicar temperatura
// static void temperature_worker_fn(async_context_t *context, async_at_time_worker_t *worker);
// static async_at_time_worker_t temperature_worker = { .do_work = temperature_worker_fn };

// // Conexão MQTT
// static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status);

// // Inicializar o cliente MQTT
// static void start_client(MQTT_CLIENT_DATA_T *state);

// // Call back com o resultado do DNS
// static void dns_found(const char *hostname, const ip_addr_t *ipaddr, void *arg);

// // Função para inicializar o estado do ônibus
// void init_bus() {
//     bus.ponto_atual = 1;
//     for (int i = 0; i < NUM_PONTOS; i++) {
//         bus.distancias[i] = DISTANCIA_INICIAL;
//     }
//     bus.velocidade = VELOCIDADE_PADRAO;
//     bus.em_movimento = false;
// }

// // Callback para mensagens MQTT recebidas
// static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags) {
//     MQTT_CLIENT_DATA_T* state = (MQTT_CLIENT_DATA_T*)arg;
// #if MQTT_UNIQUE_TOPIC
//     const char *basic_topic = state->topic + strlen(state->mqtt_client_info.client_id) + 1;
// #else
//     const char *basic_topic = state->topic;
// #endif

//     strncpy(state->data, (const char *)data, len);
//     state->len = len;
//     state->data[len] = '\0';

//     DEBUG_printf("Tópico: %s, Mensagem: %s\n", basic_topic, state->data);
//     mqtt_process_message(state, basic_topic, state->data);
// }

// // Função para processar mensagens MQTT
// static void mqtt_process_message(MQTT_CLIENT_DATA_T *state, const char *topic, const char *payload) {

//     if (strcmp(topic, BUS_COMANDO) == 0) {
//         if (strcmp(payload, "frente") == 0) {
//             bus.em_movimento = true;
//         } else if (strcmp(payload, "parar") == 0) {
//             bus.em_movimento = false;
//         } else if (strcmp(payload, "reset") == 0) {
//             init_bus(); // Reseta o estado do ônibus
//         } else if (strcmp(payload, "ponto/1") == 0) {
//             bus.ponto_atual = 1;
//         } else if (strcmp(payload, "ponto/2") == 0) {
//             bus.ponto_atual = 2;
//         } else if (strcmp(payload, "ponto/3") == 0) {
//             bus.ponto_atual = 3;
//         } else if (strcmp(payload, "ponto/4") == 0) {
//             bus.ponto_atual = 4;
//         }
//         printf("novo ponto = %d\n",bus.ponto_atual);

//         }else if (strcmp(topic, "/led") == 0){
//              if (lwip_stricmp((const char *)payload, "On") == 0 || strcmp((const char *)payload, "1") == 0)
//                 control_led(state, true);
//             else if (lwip_stricmp((const char *)payload, "Off") == 0 || strcmp((const char *)payload, "0") == 0)
//                 control_led(state, false);
//         }

//         printf("bus.em_movimento = %d", bus.em_movimento);
// }

// // Função para atualizar a matriz de LEDs (implemente a lógica aqui)
// static void update_led_matrix() {
//     // Lógica para atualizar a matriz de LEDs com base no estado do ônibus
//     // Exemplo:
//     // - Acender o LED correspondente ao ponto atual
//     // - Mostrar o progresso do ônibus entre os pontos
//     printf("Atualizando a matriz de LEDs...\n");
//         draw_led_matrix(bus.ponto_atual, MAGENTA); //exemplo
// }

// // Função para publicar os dados do ônibus nos tópicos MQTT
// static void publish_bus_data(MQTT_CLIENT_DATA_T *state) {
//     char buffer[20];

//     // Publica a distância para cada ponto
//     for (int i = 0; i < NUM_PONTOS; i++) {
//         snprintf(buffer, sizeof(buffer), "%.2f", bus.distancias[i]);
//         const char *topic;

//         switch (i + 1) {
//             case 1: topic = BUS_PONTO1_DISTANCIA; break;
//             case 2: topic = BUS_PONTO2_DISTANCIA; break;
//             case 3: topic = BUS_PONTO3_DISTANCIA; break;
//             case 4: topic = BUS_PONTO4_DISTANCIA; break;
//         }
//         mqtt_publish(state->mqtt_client_inst, full_topic(state, topic), buffer, strlen(buffer), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);
//     }

//     // Publica o ponto atual
//     snprintf(buffer, sizeof(buffer), "%d", bus.ponto_atual);
//     mqtt_publish(state->mqtt_client_inst, full_topic(state, BUS_PONTO_ATUAL), buffer, strlen(buffer), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);

//     // Publica a velocidade
//     snprintf(buffer, sizeof(buffer), "%.2f", bus.velocidade);
//     mqtt_publish(state->mqtt_client_inst, full_topic(state, BUS_VELOCIDADE), buffer, strlen(buffer), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);

//     // Publica o status (em movimento/parado)
//     const char *status = bus.em_movimento ? "em_movimento" : "parado";
//     mqtt_publish(state->mqtt_client_inst, full_topic(state, BUS_STATUS), status, strlen(status), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);
// }

// int main(void) {
//     // Inicializa todos os tipos de bibliotecas stdio padrão presentes que estão ligados ao binário.
//     stdio_init_all();
//     INFO_printf("mqtt client starting\n");

//     // Inicializa o conversor ADC
//     adc_init();
//     adc_set_temp_sensor_enabled(true);
//     adc_select_input(4);

//     // Cria registro com os dados do cliente
//     static MQTT_CLIENT_DATA_T state;

//     // Inicializa a arquitetura do cyw43
//     if (cyw43_arch_init()) {
//         panic("Failed to inizialize CYW43");
//     }

//     // Usa identificador único da placa
//     char unique_id_buf[5];
//     pico_get_unique_board_id_string(unique_id_buf, sizeof(unique_id_buf));
//     for(int i=0; i < sizeof(unique_id_buf) - 1; i++) {
//         unique_id_buf[i] = tolower(unique_id_buf[i]);
//     }

//     // Gera nome único, Ex: pico1234
//     char client_id_buf[sizeof(MQTT_DEVICE_NAME) + sizeof(unique_id_buf) - 1];
//     memcpy(&client_id_buf[0], MQTT_DEVICE_NAME, sizeof(MQTT_DEVICE_NAME) - 1);
//     memcpy(&client_id_buf[sizeof(MQTT_DEVICE_NAME) - 1], unique_id_buf, sizeof(unique_id_buf) - 1);
//     client_id_buf[sizeof(client_id_buf) - 1] = 0;
//     INFO_printf("Device name %s\n", client_id_buf);

//     state.mqtt_client_info.client_id = client_id_buf;
//     state.mqtt_client_info.keep_alive = MQTT_KEEP_ALIVE_S; // Keep alive in sec
// #if defined(MQTT_USERNAME) && defined(MQTT_PASSWORD)
//     state.mqtt_client_info.client_user = MQTT_USERNAME;
//     state.mqtt_client_info.client_pass = MQTT_PASSWORD;
// #else
//     state.mqtt_client_info.client_user = NULL;
//     state.mqtt_client_info.client_pass = NULL;
// #endif
//     static char will_topic[MQTT_TOPIC_LEN];
//     strncpy(will_topic, full_topic(&state, MQTT_WILL_TOPIC), sizeof(will_topic));
//     state.mqtt_client_info.will_topic = will_topic;
//     state.mqtt_client_info.will_msg = MQTT_WILL_MSG;
//     state.mqtt_client_info.will_qos = MQTT_WILL_QOS;
//     state.mqtt_client_info.will_retain = true;
// #if LWIP_ALTCP && LWIP_ALTCP_TLS
//     // TLS enabled
// #ifdef MQTT_CERT_INC
//     static const uint8_t ca_cert[] = TLS_ROOT_CERT;
//     static const uint8_t client_key[] = TLS_CLIENT_KEY;
//     static const uint8_t client_cert[] = TLS_CLIENT_CERT;
//     // This confirms the indentity of the server and the client
//     state.mqtt_client_info.tls_config = altcp_tls_create_config_client_2wayauth(ca_cert, sizeof(ca_cert),
//             client_key, sizeof(client_key), NULL, 0, client_cert, sizeof(client_cert));
// #if ALTCP_MBEDTLS_AUTHMODE != MBEDTLS_SSL_VERIFY_REQUIRED
//     WARN_printf("Warning: tls without verification is insecure\n");
// #endif
// #else
//     state->client_info.tls_config = altcp_tls_create_config_client(NULL, 0);
//     WARN_printf("Warning: tls without a certificate is insecure\n");
// #endif
// #endif

//     // Conectar à rede WiFI - fazer um loop até que esteja conectado
//     cyw43_arch_enable_sta_mode();
//     if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
//         panic("Failed to connect");
//     }
//     INFO_printf("\nConnected to Wifi\n");

//     //Faz um pedido de DNS para o endereço IP do servidor MQTT
//     cyw43_arch_lwip_begin();
//     int err = dns_gethostbyname(MQTT_SERVER, &state.mqtt_server_address, dns_found, &state);
//     cyw43_arch_lwip_end();

//     // Inicializa o estado do ônibus
//     init_bus();

//     // Se tiver o endereço, inicia o cliente
//     if (err == ERR_OK) {
//         start_client(&state);
//     } else if (err != ERR_INPROGRESS) { // ERR_INPROGRESS means expect a callback
//         panic("dns request failed");
//     }

//     configure_leds_matrix(); // Configura a matriz de LEDs

//         // Tópicos de assinatura
//     mqtt_request_cb_t cb =  sub_request_cb ;
//     mqtt_sub_unsub(state.mqtt_client_inst, full_topic(&state, "/led"), MQTT_SUBSCRIBE_QOS, cb, &state, 1);
//      mqtt_sub_unsub(state.mqtt_client_inst, full_topic(&state, BUS_COMANDO), MQTT_SUBSCRIBE_QOS, cb, &state, 1);

//     // Loop condicionado a conexão mqtt
//     while (!state.connect_done || mqtt_client_is_connected(state.mqtt_client_inst)) {

//         // Simulação do movimento
//         if (bus.em_movimento) {
//             // Decrementa a distância para o próximo ponto
//             bus.distancias[bus.ponto_atual - 1] -= bus.velocidade * 0.1;
//             if (bus.distancias[bus.ponto_atual - 1] < 0) {
//                 bus.distancias[bus.ponto_atual - 1] = 0;
//             }

//             // Verifica se chegou ao próximo ponto
//             if (bus.distancias[bus.ponto_atual - 1] <= PROXIMO_PONTO_THRESHOLD) {
//                 printf("Chegou ao ponto %d\n", bus.ponto_atual);
//                 // Avança para o próximo ponto
//                 bus.ponto_atual++;
//                 if (bus.ponto_atual > NUM_PONTOS) {
//                     bus.ponto_atual = 1; // Volta ao início se chegar ao último ponto
//                 }
//                 // Reseta a distância para o novo ponto
//                 bus.distancias[bus.ponto_atual - 1] = DISTANCIA_INICIAL;
//             }
//         }

//         // Atualiza a matriz de LEDs
//         update_led_matrix();

//         // Publica os dados do ônibus (adapte para usar sua estrutura MQTT)
//         publish_bus_data(&state);

//         cyw43_arch_poll();
//         cyw43_arch_wait_for_work_until(make_timeout_time_ms(1000));
//     }

//     INFO_printf("mqtt client exiting\n");
//     return 0;
// }

// /* References for this implementation:
//  * raspberry-pi-pico-c-sdk.pdf, Section '4.1.1. hardware_adc'
//  * pico-examples/adc/adc_console/adc_console.c */
// static float read_onboard_temperature(const char unit) {

//     /* 12-bit conversion, assume max value == ADC_VREF == 3.3 V */
//     const float conversionFactor = 3.3f / (1 << 12);

//     float adc = (float)adc_read() * conversionFactor;
//     float tempC = 27.0f - (adc - 0.706f) / 0.001721f;

//     if (unit == 'C' || unit != 'F') {
//         return tempC;
//     } else if (unit == 'F') {
//         return tempC * 9 / 5 + 32;
//     }

//     return -1.0f;
// }

// // Requisição para publicar
// static void pub_request_cb(__unused void *arg, err_t err) {
//     if (err != 0) {
//         ERROR_printf("pub_request_cb failed %d", err);
//     }
// }

// //Topico MQTT
// static const char *full_topic(MQTT_CLIENT_DATA_T *state, const char *name) {
// #if MQTT_UNIQUE_TOPIC
//     static char full_topic[MQTT_TOPIC_LEN];
//     snprintf(full_topic, sizeof(full_topic), "/%s%s", state->mqtt_client_info.client_id, name);
//     return full_topic;
// #else
//     return name;
// #endif
// }

// // Controle do LED
// static void control_led(MQTT_CLIENT_DATA_T *state, bool on) {
//     // Publish state on /state topic and on/off led board
//     const char* message = on ? "On" : "Off";
//     if (on)
//         cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
//     else
//         cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);

//     mqtt_publish(state->mqtt_client_inst, full_topic(state, "/led/state"), message, strlen(message), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);
// }

// // Publicar temperatura
// static void publish_temperature(MQTT_CLIENT_DATA_T *state) {
//     static float old_temperature;
//     const char *temperature_key = full_topic(state, "/temperature");
//     float temperature = read_onboard_temperature(TEMPERATURE_UNITS);
//     if (temperature != old_temperature) {
//         old_temperature = temperature;
//         // Publish temperature on /temperature topic
//         char temp_str[16];
//         snprintf(temp_str, sizeof(temp_str), "%.2f", temperature);
//         INFO_printf("Publishing %s to %s\n", temp_str, temperature_key);
//         mqtt_publish(state->mqtt_client_inst, temperature_key, temp_str, strlen(temp_str), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);
//     }
// }

// // Requisição de Assinatura - subscribe
// static void sub_request_cb(void *arg, err_t err) {
//     MQTT_CLIENT_DATA_T* state = (MQTT_CLIENT_DATA_T*)arg;
//     if (err != 0) {
//         panic("subscribe request failed %d", err);
//     }
//     state->subscribe_count++;
// }

// // Requisição para encerrar a assinatura
// static void unsub_request_cb(void *arg, err_t err) {
//     MQTT_CLIENT_DATA_T* state = (MQTT_CLIENT_DATA_T*)arg;
//     if (err != 0) {
//         panic("unsubscribe request failed %d", err);
//     }
//     state->subscribe_count--;
//     assert(state->subscribe_count >= 0);

//     // Stop if requested
//     if (state->subscribe_count <= 0 && state->stop_client) {
//         mqtt_disconnect(state->mqtt_client_inst);
//     }
// }

// // Tópicos de assinatura
// static void sub_unsub_topics(MQTT_CLIENT_DATA_T* state, bool sub) {
//     mqtt_request_cb_t cb = sub ? sub_request_cb : unsub_request_cb;
//     mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/led"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
//     mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, BUS_COMANDO), MQTT_SUBSCRIBE_QOS, cb, state, sub);
//     //mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/print"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
//     //mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/ping"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
//     //mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/exit"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
// }

// // Dados de entrada publicados
// static void mqtt_incoming_publish_cb(void *arg, const char *topic, u32_t tot_len) {
//     MQTT_CLIENT_DATA_T* state = (MQTT_CLIENT_DATA_T*)arg;
//     strncpy(state->topic, topic, sizeof(state->topic));
// }

// // Publicar temperatura
// static void temperature_worker_fn(async_context_t *context, async_at_time_worker_t *worker) {
//     MQTT_CLIENT_DATA_T* state = (MQTT_CLIENT_DATA_T*)worker->user_data;
//     publish_temperature(state);
//     async_context_add_at_time_worker_in_ms(context, worker, TEMP_WORKER_TIME_S * 1000);
// }

// // Conexão MQTT
// static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status) {
//     MQTT_CLIENT_DATA_T* state = (MQTT_CLIENT_DATA_T*)arg;
//     if (status == MQTT_CONNECT_ACCEPTED) {
//         state->connect_done = true;
//         sub_unsub_topics(state, true); // subscribe;

//         // indicate online
//         if (state->mqtt_client_info.will_topic) {
//             mqtt_publish(state->mqtt_client_inst, state->mqtt_client_info.will_topic, "1", 1, MQTT_WILL_QOS, true, pub_request_cb, state);
//         }

//         // Publish temperature every 10 sec if it's changed
//         temperature_worker.user_data = state;
//         async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &temperature_worker, 0);

//     } else if (status == MQTT_CONNECT_DISCONNECTED) {
//         if (!state->connect_done) {
//             panic("Failed to connect to mqtt server");
//         }
//     }
//     else {
//         panic("Unexpected status");
//     }
// }

// // Inicializar o cliente MQTT
// static void start_client(MQTT_CLIENT_DATA_T *state) {
// #if LWIP_ALTCP && LWIP_ALTCP_TLS
//     const int port = MQTT_TLS_PORT;
//     INFO_printf("Using TLS\n");
// #else
//     const int port = MQTT_PORT;
//     INFO_printf("Warning: Not using TLS\n");
// #endif

//     state->mqtt_client_inst = mqtt_client_new();
//     if (!state->mqtt_client_inst) {
//         panic("MQTT client instance creation error");
//     }
//     INFO_printf("IP address of this device %s\n", ipaddr_ntoa(&(netif_list->ip_addr)));
//     INFO_printf("Connecting to mqtt server at %s\n", ipaddr_ntoa(&state->mqtt_server_address));

//     cyw43_arch_lwip_begin();
//     if (mqtt_client_connect(state->mqtt_client_inst, &state->mqtt_server_address, port, mqtt_connection_cb, state, &state->mqtt_client_info) != ERR_OK) {
//         panic("MQTT broker connection error");
//     }
// #if LWIP_ALTCP && LWIP_ALTCP_TLS
//     // This is important for MBEDTLS_SSL_SERVER_NAME_INDICATION
//     mbedtls_ssl_set_hostname(altcp_tls_context(state->mqtt_client_inst->conn), MQTT_SERVER);
// #endif
//     mqtt_set_inpub_callback(state->mqtt_client_inst, mqtt_incoming_publish_cb, mqtt_incoming_data_cb, state);
//     cyw43_arch_lwip_end();
// }

// // Call back com o resultado do DNS
// static void dns_found(const char *hostname, const ip_addr_t *ipaddr, void *arg) {
//     MQTT_CLIENT_DATA_T *state = (MQTT_CLIENT_DATA_T*)arg;
//     if (ipaddr) {
//         state->mqtt_server_address = *ipaddr;
//         start_client(state);
//     } else {
//         panic("dns request failed");
//     }
// }