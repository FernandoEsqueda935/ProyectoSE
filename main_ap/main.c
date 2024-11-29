#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_spiffs.h>
#include <nvs_flash.h>
#include <esp_netif.h>
#include <esp_http_server.h>
#include "freertos/FreeRTOS.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#define TAG "ESP_TCP_SERVER"

#define PORT                        3333
#define KEEPALIVE_IDLE              5
#define KEEPALIVE_INTERVAL          5
#define KEEPALIVE_COUNT             3

#define ID_REQUEST_FROM_CLIENT 0x01
#define PUT_MEASURE_FROM_CLIENT 0x02
#define MEASURE_TIME_REQUEST_FROM_CLIENT 0x03

#define TEMPERATURE_MEASURE 0x0A
#define HUMIDITY_MEASURE 0x0B
#define BOTH_MEASURE 0x0C

#define TIME_MEASURE_15S 0x0D
#define TIME_MEASURE_30S 0x0E
#define TIME_MEASURE_1M 0x0F


#define WIFI_SSID "ESP32_AP"
#define WIFI_PASS "12345678"
#define MAX_STA_CONN 4

#define MAX_DEVICE_REGISTERED 2
uint8_t id_maker = 0xFF;

uint8_t device_registered = 0;

typedef struct {
    uint8_t id;
    uint32_t temperature[10];
    uint32_t humidity[10];
    uint8_t index;
} device_measure_t;

typedef struct {
    uint8_t id;
    uint8_t measure_type;
    uint8_t time_measure;
} device_t; 

device_t devices[2] = {0};

device_measure_t devices_measure[2] = {0};


EventGroupHandle_t response_event_group;

EventBits_t waiting_for_response = BIT0;

#define BUFFER_SIZE 8
#define MOD(n)                ((n) & (BUFFER_SIZE-1))
#define IS_BUFFER_EMPTY(buf)  (buf.in_idx == buf.out_idx)
#define IS_BUFFER_FULL(buf)   (MOD(buf.out_idx-1) == buf.in_idx)

typedef struct {
    uint8_t in_idx:4;
    uint8_t out_idx:4;
    int buffer[BUFFER_SIZE];
} ring_buffer_t;

ring_buffer_t ring_buffer = {0};


const char* sensor_form_firts = "<form action=\"/\" method=\"POST\" class=\"sensor-container\" target=\"_self\">\
      <div class=\"sensor-title\">Sensor 1</div>\
      <div class=\"sensor-options\">\
        <label>\"";
const char* sensor_form_opt1 = "<select name=\"s%co1\">\"";

const char * sensor_form_opts1 = "<option value=\"t\">Temperature</option>\
            <option value=\"h\">Humidity</option>\
            <option value=\"b\">Temperature and humidity</option>\
          </select>\
        </label>\
        <label>\"";
          
const char * sensor_form_opt2 ="<select name=\"s%co2\">\"";
            
const char * sensor_form_opts2= "<option value=\"1\">15 seconds</option>\
            <option value=\"2\">30 seconds</option>\
            <option value=\"3\">1 minute</option>\
          </select>\
        </label>\
      </div>\
      <button type=\"submit\">Enviar</button>\
    </form>";

static const char * table_sensor = "<table>\
    <tr><th>Measure</th><th>T1</th><th>T2</th><th>T3</th><th>T4</th><th>T5</th><th>T6</th><th>T7</th><th>T8</th><th>T9</th><th>T10</th></tr>\
    <tr><td>Temperature °C</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td></tr>\
    <tr><td>Humidity RH</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td></tr>\
</table>";

void update_device_measures( uint8_t device , uint8_t measure_type, uint32_t temperature , uint32_t humidity) {
    if (devices_measure[device].index < 10) {
        devices_measure[device].temperature[devices_measure[device].index] = temperature/100;
        devices_measure[device].humidity[devices_measure[device].index] = humidity;
        devices_measure[device].index++;
    }
    else {
        for (int idx = 0; idx < 9; idx++) {
            devices_measure[device].temperature[idx] = devices_measure[device].temperature[idx + 1];
            devices_measure[device].humidity[idx] = devices_measure[device].humidity[idx + 1];
        }
        devices_measure[device].temperature[9] = temperature/100;
        devices_measure[device].humidity[9] = humidity;

    }
}

void socket_response( int socket) {
    uint8_t t_buffer[6];
    uint8_t r_buffer[32];
    int r_buffer_len;

    /*
    int socket = ring_buffer.buffer[ring_buffer.out_idx];
    ring_buffer.out_idx = MOD(ring_buffer.out_idx + 1);
    */
    r_buffer_len = recv(socket, r_buffer, sizeof(r_buffer) - 1, 0);
    r_buffer[r_buffer_len] = '\0';
    ESP_LOGI(TAG, "Datos recibidos crudos %s", r_buffer);

    if (r_buffer_len < 0) {
        ESP_LOGE(TAG, "Error reciviendo primeros datos: errno %d", errno);
        goto close;
    } else if (r_buffer_len == 0) {
        ESP_LOGW(TAG, "Connection closed");
        goto close;
    }

    ESP_LOGI(TAG, "Datos recibidos %d", (int) r_buffer[0]);

    if (r_buffer[0] == ID_REQUEST_FROM_CLIENT) {
        if (device_registered < MAX_DEVICE_REGISTERED) {
            t_buffer[0] = id_maker;

            int len = send(socket, t_buffer, 1, 0);
            if (len < 0) {
                ESP_LOGE(TAG, "Error enviando el ID %d", errno);
            }
            ESP_LOGI(TAG, "Datos enviados %d", (int) t_buffer[0]);
            devices[device_registered].id = id_maker;
            id_maker--;
            device_registered++;
        }
    } else if (r_buffer[1] == MEASURE_TIME_REQUEST_FROM_CLIENT) {
        for (int idx_dv = 0; idx_dv < MAX_DEVICE_REGISTERED; idx_dv++) {
            if (r_buffer[0] == devices[idx_dv].id) {
                if (devices[idx_dv].measure_type != 0x0 && devices[idx_dv].time_measure != 0x0) {
                    t_buffer[0] = devices[idx_dv].time_measure;
                    t_buffer[1] = devices[idx_dv].measure_type;
                } else {
                    t_buffer[0] = 0x0;
                    t_buffer[1] = 0x0;
                }
                int len = send(socket, t_buffer, 2, 0);
                if (len < 0) {
                    ESP_LOGE(TAG, "Error enviando los parametros de medicion %d", errno);
                }
                break;
            }
        }
    } else if (r_buffer[1] != 0) {
        for (int idx_dv = 0; idx_dv < MAX_DEVICE_REGISTERED; idx_dv++) {
            if (r_buffer[0] == devices[idx_dv].id) {
                switch (r_buffer[1]) {
                    case TEMPERATURE_MEASURE:
                        update_device_measures(idx_dv, TEMPERATURE_MEASURE, r_buffer[2] << 24 | r_buffer[3] << 16 | r_buffer[4] << 8 | r_buffer[5], 0);
                        break;
                    case HUMIDITY_MEASURE:
                        update_device_measures(idx_dv, HUMIDITY_MEASURE, 0, r_buffer[2] << 24 | r_buffer[3] << 16 | r_buffer[4] << 8 | r_buffer[5]);
                        break;
                    case BOTH_MEASURE:
                        update_device_measures(idx_dv, BOTH_MEASURE, r_buffer[2] << 24 | r_buffer[3] << 16 | r_buffer[4] << 8 | r_buffer[5], r_buffer[6] << 24 | r_buffer[7] << 16 | r_buffer[8] << 8 | r_buffer[9]);
                        break;
                }
                ESP_LOGI(TAG, "Datos actualizados %d", (int)devices_measure[idx_dv].temperature[devices_measure[idx_dv].index - 1]);
                break;
            }
        }
    }

    close:

    shutdown(socket, 0);

    close(socket);
    
    ESP_LOGI(TAG, "SOCKET CERRADO");
}

void tcp_server_task(void * arg) {
    //convierte el ip a string y se guarda aqui 
    //familia de la ip si es ipv4 o ipv6
    int addr_family = AF_INET;
    //protocolo de la ip (tcp)
    int ip_protocol = 0;
    //variables para el keepalive del socket
    int keepAlive = 1;
    int keepIdle = KEEPALIVE_IDLE;
    int keepInterval = KEEPALIVE_INTERVAL;
    int keepCount = KEEPALIVE_COUNT;
    //
    struct sockaddr_storage dest_addr;

    struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
    dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr_ip4->sin_family = AF_INET;
    dest_addr_ip4->sin_port = htons(PORT);
    ip_protocol = IPPROTO_IP;

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));

    listen(listen_sock, 8);

    //ESP_LOGI(TAG, "Socket listening");

    struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
    socklen_t addr_len = sizeof(source_addr);
     while (1) {
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);

        // Set tcp keepalive option
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));

        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }

        ESP_LOGI(TAG, "SOCKET ABIERTO");
        socket_response(sock);
        /*
        if (IS_BUFFER_FULL(ring_buffer) == 0) {
            ESP_LOGI(TAG, "SOCKET ABIERTO");
            ring_buffer.buffer[ring_buffer.in_idx] = sock;
            ring_buffer.in_idx = MOD(ring_buffer.in_idx + 1);
        } else {
            ESP_LOGE(TAG, "Buffer is full");
        }
        */
        vTaskDelay(20/portTICK_PERIOD_MS);
    }
}

void init_spiffs()
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true};

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Error al iniciar psiffs (%s)", esp_err_to_name(ret));
        return;
    }
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Error al obtener informacion de spiffs (%s)", esp_err_to_name(ret));
        return;
    }
    else
    {
        ESP_LOGI(TAG, "Total: %d, Usado: %d", total, used);
    }
}
static void event_handler(void * args, esp_event_base_t event_base, int32_t event_id, void * event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_AP_STACONNECTED:
                ESP_LOGI(TAG, "station connected");
                break;
            case WIFI_EVENT_AP_STADISCONNECTED:
                ESP_LOGI(TAG, "station disconnected");
                break;
            default:
                break;
        }
    }
}

void wifi_init_softap(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);



    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = 1,
            .password = WIFI_PASS,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };

    if (strlen(WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "WiFi Access Point iniciado. Mi SSID: %s, Password: %s", WIFI_SSID, WIFI_PASS);
}
esp_err_t get_handler(httpd_req_t *req) {
    FILE *file = fopen("/spiffs/index.html", "r");

    if (file == NULL) {
        ESP_LOGE(TAG, "Error al abrir el archivo");
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL; // Ensure this function returns an esp_err_t value
    }

    char line[256];

    char * table_update = (char *) malloc(strlen(table_sensor) + 100);

    while (fgets(line, sizeof(line), file) != NULL) {
        httpd_resp_send_chunk(req, line, HTTPD_RESP_USE_STRLEN);
    }
    fclose(file);

    for (int cnt = 0; cnt< device_registered; cnt ++) {
        if (devices[cnt].id != 0 && devices[cnt].measure_type == 0 && devices[cnt].time_measure == 0) {
            httpd_resp_send_chunk(req, sensor_form_firts, strlen(sensor_form_firts));
            asprintf(&table_update, sensor_form_opt1, (cnt + 1) + '0');
            httpd_resp_send_chunk(req, table_update, strlen(table_update));
            httpd_resp_send_chunk(req, sensor_form_opts1, strlen(sensor_form_opts1));
            asprintf(&table_update, sensor_form_opt2, (cnt + 1) + '0');
            httpd_resp_send_chunk(req, table_update, strlen(table_update));
            httpd_resp_send_chunk(req, sensor_form_opts2, strlen(sensor_form_opts2));
        }
        else if (devices[cnt].id == 0) {
            httpd_resp_send_chunk(req, "No hay sensor registrado", strlen("No hay sensor registrado"));
        }
        else {
            asprintf(&table_update, "<div class=\"sensor-title\">Sensor %d", cnt + 1);

            httpd_resp_send_chunk(req, table_update, strlen(table_update));

            switch (devices[cnt].measure_type) {
                case TEMPERATURE_MEASURE:
                    httpd_resp_send_chunk(req, " Temperature ", strlen(" Temperature "));
                    break;
                case HUMIDITY_MEASURE:
                    httpd_resp_send_chunk(req, " Humidity ", strlen(" Humidity "));
                    break;
                case BOTH_MEASURE:
                    httpd_resp_send_chunk(req, " Temperature and Humidity ", strlen(" Temperature and Humidity "));
                    break;
            }
            switch (devices[cnt].time_measure) {
                case TIME_MEASURE_15S:
                    httpd_resp_send_chunk(req, "Period: 15 seconds</div>", strlen("Period: 15 seconds</div>"));
                    break;
                case TIME_MEASURE_30S:
                    httpd_resp_send_chunk(req, "Period: 30 seconds</div>", strlen("Period: 30 seconds</div>"));
                    break;
                case TIME_MEASURE_1M:
                    httpd_resp_send_chunk(req, "Period: 1 minute</div>", strlen("Perioed: 1 minute</div>"));
                    break;
            }

            asprintf(&table_update, table_sensor, 
            (int)devices_measure[cnt].temperature[0], (int)devices_measure[cnt].temperature[1], (int)devices_measure[cnt].temperature[2], 
            (int)devices_measure[cnt].temperature[3], (int)devices_measure[cnt].temperature[4], (int)devices_measure[cnt].temperature[5], 
            (int)devices_measure[cnt].temperature[6], (int)devices_measure[cnt].temperature[7], (int)devices_measure[cnt].temperature[8], 
            (int)devices_measure[cnt].temperature[9], (int)devices_measure[cnt].humidity[0], (int)devices_measure[cnt].humidity[1], 
            (int)devices_measure[cnt].humidity[2], (int)devices_measure[cnt].humidity[3], (int)devices_measure[cnt].humidity[4], 
            (int)devices_measure[cnt].humidity[5], (int)devices_measure[cnt].humidity[6], (int)devices_measure[cnt].humidity[7], 
            (int)devices_measure[cnt].humidity[8], (int)devices_measure[cnt].humidity[9]);
            httpd_resp_send_chunk(req, table_update, strlen(table_update));
        }
    }

    if (device_registered == 0) {
        httpd_resp_send_chunk(req, "No hay ningun sensor conectado", strlen("No hay ningun sensor conectado"));
    }

    httpd_resp_send_chunk(req, NULL, 0); // Indicate the end of the response
    free(table_update);
    return ESP_OK;

}
esp_err_t post_handler(httpd_req_t *req) {

    uint8_t inputs[req->content_len + 1];
    httpd_req_recv(req, (char *) inputs, req->content_len);
    inputs[req->content_len] = '\0';
    ESP_LOGI(TAG, "Recibido: %s", inputs);

uint8_t device = inputs[1] - '1';

switch (inputs[5]) {
    case 't':
        devices[device].measure_type = TEMPERATURE_MEASURE;
        break;
    case 'h':
        devices[device].measure_type = HUMIDITY_MEASURE;
        break;
    case 'b':
        devices[device].measure_type = BOTH_MEASURE;
        break;
}

switch (inputs[12]) {
    case '1':
        devices[device].time_measure = TIME_MEASURE_15S;
        break;
    case '2':
        devices[device].time_measure = TIME_MEASURE_30S;
        break;
    case '3':
        devices[device].time_measure = TIME_MEASURE_1M;
        break;
}

get_handler(req);

ESP_LOGI(TAG, "device %d, measure_type %d, time_measure %d", 
                                                (int) devices[device].id, (int) devices[device].measure_type, 
                                                (int) devices[device].time_measure);
    return ESP_OK;
}
void start_web_server (void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t html_get = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = get_handler,
            .user_ctx = NULL
        };
        httpd_uri_t html_post = {
            .uri = "/",
            .method = HTTP_POST,
            .handler = post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &html_get);
        httpd_register_uri_handler(server, &html_post);
    }

}
/*
void response_task(void * arg) {
    response_event_group = xEventGroupCreate();
    while (1) {
        if (IS_BUFFER_EMPTY(ring_buffer) == 0) {
            //socket_response();
        }
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}
*/
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }


    init_spiffs();

    wifi_init_softap();
    start_web_server();

    xTaskCreate(tcp_server_task, "tcp_server_task", 4096, (void *)AF_INET, 5, NULL);
    //xTaskCreate(response_task, "response_task", 4096, NULL, 5, NULL);   

}
