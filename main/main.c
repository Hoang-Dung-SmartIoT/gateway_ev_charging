#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "esp_eth_netif_glue.h"

#include "ethernet_init.h" // 👈 Ethernet example của Espressif

#include "gpio_ctrl.h"
#include "ds3231.h"
#include <time.h>

static const char *TAG = "APP";
static TaskHandle_t mqtt_pub_task_handle = NULL;

/* ================= MQTT ================= */

static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_started = false;

void time_task(void *arg)
{
    ds3231_time_t t;

    while (1)
    {
        if (ds3231_get_time(&t) == ESP_OK)
        {
            ESP_LOGI("TIME",
                     "%02d:%02d:%02d %02d/%02d/%04d",
                     t.hour, t.min, t.sec,
                     t.date, t.month, t.year);
        }
        vTaskDelay(pdMS_TO_TICKS(7000));
    }
}

static void mqtt_publish_task(void *arg)
{
    while (1)
    {
        if (mqtt_client)
        {
            led_green_on();
            const char *topic = "tbmq/cs_000001/port01/telemetry";
            const char *payload =
                "\"voltage\": 220.2,\"current\": 8,\"energy\": 2188";

            int msg_id = esp_mqtt_client_publish(
                mqtt_client,
                topic,
                payload,
                0, // len = 0 → dùng strlen
                1, // QoS
                0  // retain
            );
            vTaskDelay(pdMS_TO_TICKS(500)); // ⏱ gửi mỗi 5 giây
            if (msg_id >= 0)
            {
                ESP_LOGI("MQTT", "Telemetry published, msg_id=%d", msg_id);
            }
            else
            {
                ESP_LOGW("MQTT", "Telemetry publish failed");
            }
        }
        led_green_off();
        vTaskDelay(pdMS_TO_TICKS(3000)); // ⏱ gửi mỗi 5 giây
    }
}

static void mqtt_event_handler(void *arg,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI("MQTT", "Connected");

        esp_mqtt_client_subscribe(
            event->client,
            "tbmq/cs_000001/port01/command",
            0);

        /* 🚀 CHỈ TẠO TASK 1 LẦN */
        if (mqtt_pub_task_handle == NULL)
        {
            xTaskCreate(
                mqtt_publish_task,
                "mqtt_publish_task",
                4096,
                NULL,
                5,
                &mqtt_pub_task_handle);
            ESP_LOGI("MQTT", "Telemetry task started");
        }
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI("MQTT", "Topic: %.*s",
                 event->topic_len, event->topic);
        ESP_LOGI("MQTT", "Data : %.*s",
                 event->data_len, event->data);
        break;

    default:
        break;
    }
}

static void mqtt_start(void)
{
    if (mqtt_started)
        return;
    mqtt_started = true;

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = "mqtt://72.61.140.234:1883",
        .credentials.username = "thuanphat",
        .credentials.authentication.password = "123456789",
        .credentials.client_id = "phat123",
    };

    mqtt_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(
        mqtt_client,
        MQTT_EVENT_ANY,
        mqtt_event_handler,
        NULL);

    esp_mqtt_client_start(mqtt_client);
    ESP_LOGI(TAG, "MQTT started");
    led_blue_on();
}

/* ================= ETHERNET EVENT ================= */

static void got_ip_event_handler(void *arg,
                                 esp_event_base_t base,
                                 int32_t event_id,
                                 void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

    ESP_LOGI(TAG, "ETH GOT IP");
    ESP_LOGI(TAG, "IP: " IPSTR,
             IP2STR(&event->ip_info.ip));

    ds3231_sync_from_ntp();
    vTaskDelay(pdMS_TO_TICKS(1000));
    xTaskCreate(time_task, "time_task", 4096, NULL, 5, NULL);

    /* 🚀 CHỈ TẠI ĐÂY → START MQTT */
    mqtt_start();
}

/* ================= MAIN ================= */

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* ===== Register IP event ===== */
    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_ETH_GOT_IP,
            &got_ip_event_handler,
            NULL));
    gpio_ctrl_init();
    led_red_on();
    /* ===== Init Ethernet driver ===== */
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles;

    ESP_ERROR_CHECK(example_eth_init(&eth_handles, &eth_port_cnt));

    /* ===== CREATE esp-netif + ATTACH ===== */
    esp_netif_t *eth_netifs[eth_port_cnt];
    esp_eth_netif_glue_handle_t eth_glues[eth_port_cnt];

    for (int i = 0; i < eth_port_cnt; i++)
    {
        esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
        eth_netifs[i] = esp_netif_new(&cfg);

        eth_glues[i] = esp_eth_new_netif_glue(eth_handles[i]);

        ESP_ERROR_CHECK(
            esp_netif_attach(eth_netifs[i], eth_glues[i]));
    }

    /* ===== Start Ethernet ===== */
    for (int i = 0; i < eth_port_cnt; i++)
    {
        ESP_ERROR_CHECK(esp_eth_start(eth_handles[i]));
    }

    ESP_LOGI(TAG, "Ethernet started, waiting for IP...");
    ds3231_init();
    // ds3231_set_compile_time();
    // xTaskCreate(time_task, "time_task", 4096, NULL, 5, NULL);
}
