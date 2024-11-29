#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include <sys/socket.h>
#include <errno.h>
#include <netdb.h>            
#include <arpa/inet.h>
#include "esp_netif.h"
#include "esp_sleep.h"

#define WIFI_SSID "ESP32_AP"
#define WIFI_PASS "12345678"
#define HOST_IP_ADDR "192.168.4.1"
#define PORT 3333

#define ID_REQUEST_FROM_CLIENT 0x01
#define PUT_MEASURE_FROM_CLIENT 0x02
#define MEASURE_TIME_REQUEST_FROM_CLIENT 0x03

#define TEMPERATURE_MEASURE 0x0A
#define HUMIDITY_MEASURE 0x0B
#define BOTH_MEASURE 0x0C
///variables para los intervalos de mediciones
#define TIME_MEASURE_15S 0x0D
#define TIME_MEASURE_30S 0x0E
#define TIME_MEASURE_1M 0x0F

static const char *TAG = "ESP32_STA";

RTC_DATA_ATTR static uint8_t id = 0;
RTC_DATA_ATTR static uint8_t  measure_type= 0;
RTC_DATA_ATTR static uint8_t  measure_time = 0;
//los pines para esp en modo maestro
#define I2C_MASTER_SCL_IO           22
#define I2C_MASTER_SDA_IO           21

//los numeros de los buses i2c
#define I2C_SLAVE_NUM              I2C_NUM_1
#define I2C_MASTER_NUM              I2C_NUM_0

//bits de temperatura ready o de temperatura solicitada
#define I2C_GET_TEMPERATURE BIT0
#define I2C_TEMPERATURE_READY BIT1

#define DATA_LENGTH 2

QueueHandle_t s_receive_queue;

//Para controlar cuando se deba hacer una medicion de temperatura
EventGroupHandle_t i2c_event_group;


i2c_master_bus_handle_t bus_handle;

i2c_master_dev_handle_t dev_handle;

int32_t temperature = 0;

EventGroupHandle_t wifi_event_group;

EventBits_t wifi_connected = BIT0;

long signed int t_fine;

long signed int DIG_T1 = 27698;
long signed int DIG_T2= -410;
long signed int DIG_T3= 189;

unsigned char DIG_H1 = 75;
signed int DIG_H2= 356;
unsigned char DIG_H3= 0;
signed int DIG_H4= 0;
signed int DIG_H5= 0;
signed char DIG_H6= 30;

int32_t humidity = 0;

long signed int get_temperature();
long signed int get_humidity();



void tcp_client_task(void) {
    xEventGroupWaitBits(wifi_event_group, wifi_connected, pdFALSE, pdTRUE, portMAX_DELAY);
    uint8_t tx_buffer[32];
    char rx_buffer[128];
    char host_ip[] = HOST_IP_ADDR;
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;
    struct sockaddr_in dest_addr;
    while (1) {
    inet_pton(AF_INET, host_ip, &dest_addr.sin_addr);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(PORT);

    if (measure_type != 0 && measure_time != 0) {
        tx_buffer[0] = id;
        tx_buffer[1] = measure_type;
        if (measure_type == TEMPERATURE_MEASURE) {
            temperature = get_temperature();
            tx_buffer[2] = (temperature >> 24) & 0xFF;
            tx_buffer[3] = (temperature >> 16) & 0xFF;
            tx_buffer[4] = (temperature >> 8) & 0xFF;
            tx_buffer[5] = temperature & 0xFF;
        }
        else if (measure_type == HUMIDITY_MEASURE) {
            humidity = get_humidity();
            tx_buffer[2] = (humidity >> 24) & 0xFF;
            tx_buffer[3] = (humidity >> 16) & 0xFF;
            tx_buffer[4] = (humidity >> 8) & 0xFF;
            tx_buffer[5] = humidity & 0xFF;
        }
        else if (measure_type == BOTH_MEASURE) {
            temperature = get_temperature();
            humidity = get_humidity();
            tx_buffer[2] = (temperature >> 24) & 0xFF;
            tx_buffer[3] = (temperature >> 16) & 0xFF;
            tx_buffer[4] = (temperature >> 8) & 0xFF;
            tx_buffer[5] = temperature & 0xFF;
            tx_buffer[6] = (humidity >> 24) & 0xFF;
            tx_buffer[7] = (humidity >> 16) & 0xFF;
            tx_buffer[8] = (humidity >> 8) & 0xFF;
            tx_buffer[9] = humidity & 0xFF;
        }
    }

    ESP_LOGI(TAG, "Iniciando conexión al servidor TCP...");
    xEventGroupWaitBits(wifi_event_group, wifi_connected, pdFALSE, pdTRUE, portMAX_DELAY);
    int sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (sock < 0) {
        ESP_LOGE(TAG, "Error creando socket: errno %d", errno);
        vTaskDelay(5000 / portTICK_PERIOD_MS); 
        goto cleanup;
    }
    ESP_LOGI(TAG, "Socket creado correctamente");
    int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Error conectando al servidor: errno %d", errno);
        goto cleanup;
    }
    ESP_LOGI(TAG, "Conexión exitosa al servidor %s:%d", host_ip, PORT);

    //
    if (id == 0) {
        tx_buffer[0] = (uint8_t) ID_REQUEST_FROM_CLIENT;
        err = send(sock, tx_buffer, 1, 0);
        if (err < 0) {
            ESP_LOGE(TAG, "Error enviando datos: errno %d", errno);
            goto cleanup; 
        }

        int len = recv(sock, rx_buffer, 1, 0);
        if (len < 0) {
            ESP_LOGE(TAG, "Error recibiendo datos: errno %d", errno);
            goto cleanup; 
        } else if (len == 0) {
            ESP_LOGW(TAG, "Servidor cerró la conexión");
            goto cleanup;
        }

        id = rx_buffer[0];
        
    } 

    else if (id != 0 && measure_type == 0 && measure_time == 0) {
        
        tx_buffer[0] = id;
        tx_buffer[1] = (uint8_t) MEASURE_TIME_REQUEST_FROM_CLIENT;
        send(sock, tx_buffer, 2, 0);
        if (err < 0) {
            ESP_LOGE(TAG, "Error enviando datos: errno %d", errno);
            goto cleanup;
        }
        int len = recv(sock, rx_buffer, 2, 0);
        if (len < 0) {
            ESP_LOGE(TAG, "Error recibiendo datos: errno %d", errno);
            goto cleanup;
        } else if (len == 0) {
            ESP_LOGW(TAG, "Servidor cerró la conexión");
            goto cleanup;
        }
        
        if(rx_buffer[0] != 0 && rx_buffer[1] != 0) {
            if (rx_buffer[0] == TIME_MEASURE_15S) {
                measure_time = 15;
            }
            else if (rx_buffer[0] == TIME_MEASURE_30S) {
                measure_time = 30;
            }
            else if (rx_buffer[0] == TIME_MEASURE_1M) {
                measure_time = 60;
            }
            measure_type = rx_buffer[1];
        }
        
        else {
            ESP_LOGI(TAG, "No se recibieron tipo de medicion ni tiempo");
        }
    }
    else {
        if (measure_type == BOTH_MEASURE) {
            err = send(sock, tx_buffer, 10, 0);
        }
        else {
            err = send(sock, tx_buffer, 6, 0);
        }
        if (err < 0) {
            ESP_LOGE(TAG, "Error enviando datos: errno %d", errno);
            goto cleanup;
        }
        
        
        shutdown(sock, 0);
        close(sock);


        vTaskDelay(1000/ portTICK_PERIOD_MS);
        esp_sleep_enable_timer_wakeup( (measure_time * 1000000) );
        ESP_LOGI(TAG, "Entrando en modo de bajo consumo...");
        esp_wifi_disconnect();
        esp_wifi_stop();
        esp_wifi_deinit();
        
        esp_deep_sleep_start();
    }

    cleanup:
        if (sock != -1) {
            ESP_LOGI(TAG, "Cerrando socket...");
            shutdown(sock, 0);
            close(sock);
        }
        ESP_LOGI(TAG, "Esperando antes de reintentar...");
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}


static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Desconectado. Intentando reconexion...");
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, wifi_connected);
    }
}

void wifi_init_sta(void) {


    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "Estacion WiFi iniciada. Conectando al SSID: %s ...", WIFI_SSID);
}

void i2c_master_init() {
    i2c_master_bus_config_t i2c_mst_config_1 = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
        .i2c_port = I2C_MASTER_NUM,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_config_1, &bus_handle));
    
}

void i2c_slaves_devices_init() {
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x76,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle));
}

long signed int BME280_temperature_compensate_T_double(long signed int adc_T) {
    long signed int var1, var2;
    var1 = ((((adc_T>>3) - ((long signed int)DIG_T1<<1))) * ((long signed int)DIG_T2)) >> 11;
    var2 = (((((adc_T>>4) - ((long signed int)DIG_T1)) * ((adc_T>>4) - ((long signed int)DIG_T1))) >> 12) * ((long signed int)DIG_T3)) >> 14;
    return var1 + var2;
}

long signed int BME280_humidity_compensate_T_double (long signed int adc_H) {
    long signed int value;
    value = (t_fine - ((long signed int)76800));
    value = (((((adc_H << 14) - (((long signed int)DIG_H4) << 20) - (((long signed int)DIG_H5) * value)) + ((long signed int)16384)) >> 15) * 
    (((((((value * ((long signed int)DIG_H6)) >> 10) * (((value * ((long signed int)DIG_H3)) >> 11) + ((long signed int)32768)) >> 10) + 
    ((long signed int)2097152)) * ((long signed int)DIG_H2) + 8192) >> 14)));
    value = (value - (((((value >> 15) * (value >> 15)) >> 7) * ((long signed int)DIG_H1)) >> 4));
    value = (value < 0 ? 0 : value);
    value = (value > 419430400 ? 419430400 : value);
    return value>>12;
}

void get_temperature_compensation_values() {
    uint8_t r_buffer[6] = {0 , 0 , 0, 0 , 0 , 0 };
    uint8_t data= 0x88;
    
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev_handle, &data, 1, r_buffer, 6, -1));
    DIG_T1 = r_buffer[0] | r_buffer[1] << 8;
    DIG_T2 = r_buffer[2] | r_buffer[3] << 8;
    DIG_T3 = r_buffer[4] | r_buffer[5] << 8;
}

void get_humidity_compenstation_values() {
    uint8_t r_buffer[8] = {0 , 0 , 0, 0 , 0 , 0, 0, 0};
    
    uint8_t data= 0xA1;
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev_handle, &data, 1, r_buffer, 1, -1));
    
    data= 0xE1;
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev_handle, &data, 1, (r_buffer + 1), 7, -1));
    
    DIG_H1 = r_buffer[0];
    DIG_H2 = r_buffer[1] | r_buffer[2] << 8;
    DIG_H3 = r_buffer[3];
    DIG_H4 = r_buffer[4] << 4 | (r_buffer[5] & 0x0F);
    DIG_H5 = r_buffer[6] << 4 | r_buffer[5] >> 4;
    DIG_H6 = r_buffer[7];

}

long signed int get_temperature() {
    //instrucciones necesarias para solo obtener la medicion de temperatura, direccion/valor 
    uint8_t data_wr[8] = {0xF5, 0x00, 0xF2, 0x00, 0xF4, 0xA1};
    uint8_t r_buffer[6] = {0 , 0 , 0, 0 , 0 , 0 };
    
    ESP_ERROR_CHECK(i2c_master_transmit(dev_handle, data_wr, 6, -1));

    data_wr[0]= 0xF3;
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev_handle, data_wr, 1, r_buffer, 1, -1));

    while (r_buffer[0] & (1<<0|1<<3)) {
        ESP_ERROR_CHECK(i2c_master_transmit_receive(dev_handle, data_wr, 1, r_buffer, 1, -1));
    }

    data_wr[0]= 0xFA;
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev_handle, data_wr, 1, r_buffer, 3, -1));

    long signed int adc_T = 0;
    adc_T = r_buffer[0] << 12 | r_buffer[1] << 4 | r_buffer[2] >> 4;

    t_fine = BME280_temperature_compensate_T_double(adc_T);

    long signed int T = (t_fine * 5 + 128) >> 8;

    return T;
}

long signed int get_humidity() {
    //instrucciones necesarias para solo obtener la medicion de humedad, direccion/valor 
    get_temperature();
    
    uint8_t data_wr[8] = {0xF5, 0x00, 0xF2, 0x05, 0xF4, 0x01};
    uint8_t r_buffer[6] = {0 , 0 , 0, 0 , 0 , 0 };
    
    ESP_ERROR_CHECK(i2c_master_transmit(dev_handle, data_wr, 6, -1));

    data_wr[0]= 0xF3;
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev_handle, data_wr, 1, r_buffer, 1, -1));

    while (r_buffer[0] & (1<<0|1<<3)) {
        ESP_ERROR_CHECK(i2c_master_transmit_receive(dev_handle, data_wr, 1, r_buffer, 1, -1));
    }

    data_wr[0]= 0xFD;
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev_handle, data_wr, 1, r_buffer, 2, -1));

    long signed int adc_H = 0;

    adc_H = r_buffer[0] << 8 | r_buffer[1];

    long signed int h_fine = BME280_humidity_compensate_T_double(adc_H);

    long signed int H = h_fine;

    return H>>10;

}



void app_main(void)
{
    wifi_event_group = xEventGroupCreate(); 
    i2c_master_init();
    i2c_slaves_devices_init();
    
    get_temperature_compensation_values();
    get_humidity_compenstation_values();

    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();

    xTaskCreate( (TaskFunction_t) tcp_client_task, "tcp_client", 6128, NULL, 5, NULL);
}