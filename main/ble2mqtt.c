#include "config.h"
#include "broadcasters.h"
#include "ble.h"
#include "ble_utils.h"
#include "eth.h"
#include "httpd.h"
#include "log.h"
#include "mqtt.h"
#include "ota.h"
#include "resolve.h"
#include "wifi.h"
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <mdns.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <string.h>
#include <driver/gpio.h>

#include "swbd.h"

#define MAX_TOPIC_LEN 256
static const char *TAG = "BLE2MQTT";

const ble_uuid_t swbd_service =        {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xe0, 0xff, 0x00, 0x00};
const ble_uuid_t swbd_characteristic = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xe1, 0xff, 0x00, 0x00};

const ble_uuid_t enable_charact =      {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xe2, 0xff, 0x00, 0x00};
const ble_uuid_t speed_charact =       {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xe3, 0xff, 0x00, 0x00};
const ble_uuid_t sensivity_charact =   {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xe4, 0xff, 0x00, 0x00};
const ble_uuid_t move_sensor_charact=  {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xe5, 0xff, 0x00, 0x00}; 
const ble_uuid_t hours_charact=        {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xe6, 0xff, 0x00, 0x00}; 
const ble_uuid_t minutes_charact=      {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xe7, 0xff, 0x00, 0x00}; 

typedef struct {
    mac_addr_t mac;
    ble_uuid_t service;
    ble_uuid_t characteristic;
    uint8_t index;
} mqtt_ctx_t;

static const char *device_name_get(void)
{
    static const char *name = NULL;
    uint8_t *mac = NULL;

    if (name)
        return name;

    if ((name = config_network_hostname_get()))
        return name;

    switch (config_network_type_get())
    {
    case NETWORK_TYPE_ETH:
        mac = eth_mac_get();
        break;
    case NETWORK_TYPE_WIFI:
        mac = wifi_mac_get();
        break;
    }
    name = malloc(14);
    sprintf((char *)name, "BLE2MQTT-%02X%02X", mac[4], mac[5]);

    return name;
}

/* Bookkeeping functions */
static void uptime_publish(void)
{
    char topic[MAX_TOPIC_LEN];
    char buf[16];

    /* Only publish uptime when connected, we don't want it to be queued */
    if (!mqtt_is_connected())
        return;

    /* Uptime (in seconds) */
    sprintf(buf, "%" PRId64, esp_timer_get_time() / 1000 / 1000);
    snprintf(topic, MAX_TOPIC_LEN, "%s/Uptime", device_name_get());
    mqtt_publish(topic, (uint8_t *)buf, strlen(buf), config_mqtt_qos_get(),
        config_mqtt_retained_get());

    /* Free memory (in bytes) */
    sprintf(buf, "%" PRIu32, esp_get_free_heap_size());
    snprintf(topic, MAX_TOPIC_LEN, "%s/FreeMemory", device_name_get());
    mqtt_publish(topic, (uint8_t *)buf, strlen(buf), config_mqtt_qos_get(),
        config_mqtt_retained_get());
}

static void self_publish(void)
{
    char topic[MAX_TOPIC_LEN];
    char *payload;

    /* Current status */
    payload = "Online";
    snprintf(topic, MAX_TOPIC_LEN, "%s/Status", device_name_get());
    mqtt_publish(topic, (uint8_t *)payload, strlen(payload),
        config_mqtt_qos_get(), config_mqtt_retained_get());

    /* App version */
    payload = BLE2MQTT_VER;
    snprintf(topic, MAX_TOPIC_LEN, "%s/Version", device_name_get());
    mqtt_publish(topic, (uint8_t *)payload, strlen(payload),
        config_mqtt_qos_get(), config_mqtt_retained_get());

    /* Config version */
    payload = config_version_get();
    snprintf(topic, MAX_TOPIC_LEN, "%s/ConfigVersion", device_name_get());
    mqtt_publish(topic, (uint8_t *)payload, strlen(payload),
        config_mqtt_qos_get(), config_mqtt_retained_get());

    uptime_publish();
}

/* OTA functions */
static void ota_on_completed(ota_type_t type, ota_err_t err)
{
    ESP_LOGI(TAG, "Update completed: %s", ota_err_to_str(err));

    /* All done, restart */
    if (err == OTA_ERR_SUCCESS)
        abort();
    else
        ble_scan_start();
}

static void _ota_on_completed(ota_type_t type, ota_err_t err);

static void ota_on_mqtt(const char *topic, const uint8_t *payload, size_t len,
    void *ctx)
{
    char *url = malloc(len + 1);
    ota_type_t type = (ota_type_t)ctx;
    ota_err_t err;

    memcpy(url, payload, len);
    url[len] = '\0';
    ESP_LOGI(TAG, "Starting %s update from %s",
        type == OTA_TYPE_FIRMWARE ? "firmware" : "configuration", url);

    if ((err = ota_download(type, url, _ota_on_completed)) != OTA_ERR_SUCCESS)
        ESP_LOGE(TAG, "Failed updating: %s", ota_err_to_str(err));

    ble_disconnect_all();
    ble_scan_stop();
    free(url);
}

static void _ota_on_mqtt(const char *topic, const uint8_t *payload, size_t len,
    void *ctx);

static void ota_subscribe(void)
{
    char topic[MAX_TOPIC_LEN];

    /* Register for both a specific topic for this device and a general one */
    snprintf(topic, MAX_TOPIC_LEN, "%s/OTA/Firmware", device_name_get());
    mqtt_subscribe(topic, 0, _ota_on_mqtt, (void *)OTA_TYPE_FIRMWARE, NULL);
    mqtt_subscribe("BLE2MQTT/OTA/Firmware", 0, _ota_on_mqtt,
        (void *)OTA_TYPE_FIRMWARE, NULL);

    snprintf(topic, MAX_TOPIC_LEN, "%s/OTA/Config", device_name_get());
    mqtt_subscribe(topic, 0, _ota_on_mqtt, (void *)OTA_TYPE_CONFIG, NULL);
    mqtt_subscribe("BLE2MQTT/OTA/Config", 0, _ota_on_mqtt,
        (void *)OTA_TYPE_CONFIG, NULL);
}

static void ota_unsubscribe(void)
{
    char topic[27];

    sprintf(topic, "%s/OTA/Firmware", device_name_get());
    mqtt_unsubscribe(topic);
    mqtt_unsubscribe("BLE2MQTT/OTA/Firmware");

    sprintf(topic, "%s/OTA/Config", device_name_get());
    mqtt_unsubscribe(topic);
    mqtt_unsubscribe("BLE2MQTT/OTA/Config");
}

/* Management functions */
static void management_on_restart_mqtt(const char *topic,
    const uint8_t *payload, size_t len, void *ctx)
{
    if (len != 4 || strncmp((char *)payload, "true", len))
        return;

    abort();
}

static void _management_on_restart_mqtt(const char *topic,
    const uint8_t *payload, size_t len, void *ctx);

static void management_subscribe(void)
{
    char topic[MAX_TOPIC_LEN];

    snprintf(topic, MAX_TOPIC_LEN, "%s/Restart", device_name_get());
    mqtt_subscribe(topic, 0, _management_on_restart_mqtt, NULL, NULL);
    mqtt_subscribe("BLE2MQTT/Restart", 0, _management_on_restart_mqtt, NULL,
        NULL);
}

static void management_unsubscribe(void)
{
    char topic[MAX_TOPIC_LEN];

    snprintf(topic, MAX_TOPIC_LEN, "%s/Restart", device_name_get());
    mqtt_unsubscribe(topic);
    mqtt_unsubscribe("BLE2MQTT/Restart");
}

static void cleanup(void)
{
    ble_disconnect_all();
    ble_scan_stop();
    ota_unsubscribe();
    management_unsubscribe();
}

/* Network callback functions */
static void network_on_connected(void)
{
    char status_topic[MAX_TOPIC_LEN];

    log_start(config_log_host_get(), config_log_port_get());
    ESP_LOGI(TAG, "Connected to the network, connecting to MQTT");
    snprintf(status_topic, MAX_TOPIC_LEN, "%s/Status", device_name_get());

    mqtt_connect(config_mqtt_host_get(), config_mqtt_port_get(),
        config_mqtt_client_id_get(), config_mqtt_username_get(),
        config_mqtt_password_get(), config_mqtt_ssl_get(),
        config_mqtt_server_cert_get(), config_mqtt_client_cert_get(),
        config_mqtt_client_key_get(), status_topic, "Offline",
        config_mqtt_qos_get(), config_mqtt_retained_get());
}

static void network_on_disconnected(void)
{
    log_stop();
    ESP_LOGI(TAG, "Disconnected from the network, stopping MQTT");
    mqtt_disconnect();
    /* We don't get notified when manually stopping MQTT */
    cleanup();
}

/* MQTT callback functions */
static void mqtt_on_connected(void)
{
    ESP_LOGI(TAG, "Connected to MQTT, scanning for BLE devices");
    self_publish();
    ota_subscribe();
    management_subscribe();
    ble_scan_start();
}

static void mqtt_on_disconnected(void)
{
    static uint8_t num_disconnections = 0;

    ESP_LOGI(TAG, "Disconnected from MQTT, stopping BLE");
    cleanup();

    if (++num_disconnections % 3 == 0)
    {
        ESP_LOGI(TAG,
            "Failed connecting to MQTT 3 times, reconnecting to the network");
        wifi_reconnect();
    }
}

/* BLE functions */
static void ble_on_mqtt_connected_cb(const char *topic, const uint8_t *payload,
    size_t len, void *ctx)
{
    char new_topic[MAX_TOPIC_LEN];

    if (len == 4 && !strncmp((char *)payload, "true", len))
        return;

    /* Someone published our device is disconnected, set them straight */
    snprintf(new_topic, MAX_TOPIC_LEN, "%s%s/Connected",
        config_mqtt_prefix_get(), (char *)ctx);
    mqtt_publish(new_topic, (uint8_t *)"true", 4, config_mqtt_qos_get(),
        config_mqtt_retained_get());
}

static void _ble_on_mqtt_connected_cb(const char *topic, const uint8_t *payload,
    size_t len, void *ctx);

static void ble_publish_connected(mac_addr_t mac, uint8_t is_connected)
{
    char topic[MAX_TOPIC_LEN];

    snprintf(topic, MAX_TOPIC_LEN, "%s" MAC_FMT "/Connected",
        config_mqtt_prefix_get(), MAC_PARAM(mac));

    if (!is_connected)
        mqtt_unsubscribe(topic);

    mqtt_publish(topic, (uint8_t *)(is_connected ? "true" : "false"),
        is_connected ? 4 : 5, config_mqtt_qos_get(),
        config_mqtt_retained_get());

    if (is_connected)
    {
        const char *device_name = device_name_get();

        /* Subscribe for other devices claiming this device is disconnected */
        mqtt_subscribe(topic, config_mqtt_qos_get(), _ble_on_mqtt_connected_cb,
            strdup(mactoa(mac)), free);
        /* We are now the owner of this device */
        snprintf(topic, MAX_TOPIC_LEN, "%s" MAC_FMT "/Owner",
            config_mqtt_prefix_get(), MAC_PARAM(mac));
        mqtt_publish(topic, (uint8_t *)device_name, strlen(device_name),
            config_mqtt_qos_get(), config_mqtt_retained_get());
    }
}

static mqtt_ctx_t *ble_ctx_gen(mac_addr_t mac, const ble_uuid_t service,
    const ble_uuid_t characteristic, uint8_t index)
{
    mqtt_ctx_t *ctx = malloc(sizeof(mqtt_ctx_t));

    memcpy(ctx->mac, mac, sizeof(mac_addr_t));
    memcpy(ctx->service, service, sizeof(ble_uuid_t));
    memcpy(ctx->characteristic, characteristic, sizeof(ble_uuid_t));
    ctx->index = index;

    return ctx;
}

/* BLE callback functions */
static void ble_on_broadcaster_metadata(char *name, char *val, void *ctx)
{
    char topic[MAX_TOPIC_LEN];

    sprintf(topic, "%s/%s/%s", device_name_get(), (char *)ctx, name);
    /* Broadcaster topics shouldn't be retained */
    mqtt_publish(topic, (uint8_t *)val, strlen(val), config_mqtt_qos_get(), 0);
}

static void ble_on_broadcaster_discovered(mac_addr_t mac, uint8_t *adv_data,
    size_t adv_data_len, int rssi, broadcaster_ops_t *ops)
{
    char *mac_str = strdup(mactoa(mac));
    char rssi_str[6];
    ESP_LOGI(TAG, "Discovered %s broadcaster", ops->name);

    ble_on_broadcaster_metadata("Type", ops->name, mac_str);
    sprintf(rssi_str, "%d", rssi);
    ble_on_broadcaster_metadata("RSSI", rssi_str, mac_str);
    ops->metadata_get(adv_data, adv_data_len, rssi, ble_on_broadcaster_metadata,
        mac_str);

    free(mac_str);
}

static void ble_on_device_discovered(mac_addr_t mac, char * name, size_t name_len, int rssi)  // by MK
{

    uint8_t connect = config_ble_should_connect(mactoa(mac), name);

    //if ((strcmp(name,"SWBDdrv_mc5")==0)||(strcmp(name,"SWBDdrv_mc2")==0)) {connect=1;} else {connect=0;}

    
    ESP_LOGI(TAG, "Discovered BLE device: " MAC_FMT ", name: %s, (RSSI: %d), %sconnecting", // by MK
        MAC_PARAM(mac), name, rssi, connect ? "" : "not ");

    if (!connect)
        return;
    
    
    ble_connect(mac);
}

static void ble_on_device_connected(mac_addr_t mac)
{
    ESP_LOGI(TAG, "Connected to device: " MAC_FMT ", scanning",
        MAC_PARAM(mac));
    ble_publish_connected(mac, 1);
    ble_services_scan(mac);
}

static char *ble_topic_suffix(char *base, uint8_t is_get)
{
    static char topic[MAX_TOPIC_LEN];

    sprintf(topic, "%s%s", base, is_get ? config_mqtt_get_suffix_get() :
        config_mqtt_set_suffix_get());

    return topic;
}

static char *ble_topic(mac_addr_t mac, const ble_uuid_t service_uuid,
    const ble_uuid_t characteristic_uuid, uint8_t index)
{
    static char topic[MAX_TOPIC_LEN];
    int i = 0;

    i += snprintf(topic + i, MAX_TOPIC_LEN, "%s" MAC_FMT "/%s",
        config_mqtt_prefix_get(), MAC_PARAM(mac),
        ble_service_name_get(service_uuid));
    i += snprintf(topic + i, MAX_TOPIC_LEN - i, "/%s",
        ble_characteristic_name_get(characteristic_uuid));

    if (index > 0)
        i += snprintf(topic + i, MAX_TOPIC_LEN - i, "_%u", index);

    return topic;
}

static void ble_on_device_disconnected(mac_addr_t mac)
{
    char topic[MAX_TOPIC_LEN];

    ESP_LOGI(TAG, "Disconnected from device: " MAC_FMT, MAC_PARAM(mac));
    ble_publish_connected(mac, 0);
    snprintf(topic, MAX_TOPIC_LEN, "%s" MAC_FMT "/",
        config_mqtt_prefix_get(), MAC_PARAM(mac));
    mqtt_unsubscribe_topic_prefix(topic);
}

static void ble_on_mqtt_get(const char *topic, const uint8_t *payload,
    size_t len, void *ctx)
{
    ESP_LOGD(TAG, "Got read request: %s", topic);
    mqtt_ctx_t *data = (mqtt_ctx_t *)ctx;

    ble_characteristic_read(data->mac, data->service, data->characteristic,
        data->index);
}

static void ble_on_mqtt_set(const char *topic, const uint8_t *payload,
    size_t len, void *ctx)
{
    ESP_LOGI(TAG, "Got write request: %s, len: %u", topic, len);
    mqtt_ctx_t *data = (mqtt_ctx_t *)ctx;
    size_t buf_len;
    uint8_t *buf = atochar(data->characteristic, (const char *)payload,
        len, &buf_len);

    if (ble_uuid_equal(data->service, swbd_service)&&ble_uuid_equal(data->characteristic, enable_charact)) // enable
    {
        ESP_LOGI(TAG, "ENABLE");
        extern ble_device_t *devices_list;
        ble_device_t *device=ble_device_find_by_mac(devices_list, data->mac);
        if (device) 
        {
            uint8_t array[6];
            memcpy(array, device->swbd_condition, sizeof(array));

            array[0]=(array[0] & ~((uint8_t)1 << 7)) | (buf[0] << 7);
            array[2]=0xFF;
            array[3]=0xFF;
            array[5]=chsum(array);
          

            ESP_LOGI(TAG, "Write to: " MAC_FMT " , service: " UUID_FMT ", characteristic: " UUID_FMT " buf: " SWBD_FMT "", MAC_PARAM(data->mac), UUID_PARAM(swbd_service), UUID_PARAM(swbd_characteristic), SWBD_PARAM(array));
            ble_characteristic_write(data->mac, swbd_service, swbd_characteristic, data->index, array, 6);        
        }
    }
    else
    if (ble_uuid_equal(data->service, swbd_service)&&ble_uuid_equal(data->characteristic, speed_charact)) // speed
    {
        ESP_LOGI(TAG, "SPEED");
        extern ble_device_t *devices_list;
        ble_device_t *device=ble_device_find_by_mac(devices_list, data->mac);
        if (device) 
        {
            uint8_t array[6];
            memcpy(array, device->swbd_condition, sizeof(array));

            array[0]=(array[0] & (0b10000000)) | (buf[0]);
            array[2]=0xFF;
            array[3]=0xFF;
            array[5]=chsum(array);
          

            ESP_LOGI(TAG, "Write to: " MAC_FMT " , service: " UUID_FMT ", characteristic: " UUID_FMT " buf: " SWBD_FMT "", MAC_PARAM(data->mac), UUID_PARAM(swbd_service), UUID_PARAM(swbd_characteristic), SWBD_PARAM(array));
            ble_characteristic_write(data->mac, swbd_service, swbd_characteristic, data->index, array, 6);        
        }
    }
    else
    if (ble_uuid_equal(data->service, swbd_service)&&ble_uuid_equal(data->characteristic, sensivity_charact)) // sensivity
    {
        ESP_LOGI(TAG, "SENSIVITY");
        extern ble_device_t *devices_list;
        ble_device_t *device=ble_device_find_by_mac(devices_list, data->mac);
        if (device) 
        {
            uint8_t array[6];
            memcpy(array, device->swbd_condition, sizeof(array));

            array[1]=(array[1] & (0b10000000)) | (buf[0]);
            array[2]=0xFF;
            array[3]=0xFF;
            array[5]=chsum(array);
          

            ESP_LOGI(TAG, "Write to: " MAC_FMT " , service: " UUID_FMT ", characteristic: " UUID_FMT " buf: " SWBD_FMT "", MAC_PARAM(data->mac), UUID_PARAM(swbd_service), UUID_PARAM(swbd_characteristic), SWBD_PARAM(array));
            ble_characteristic_write(data->mac, swbd_service, swbd_characteristic, data->index, array, 6);        
        }
    }
    else
    if (ble_uuid_equal(data->service, swbd_service)&&ble_uuid_equal(data->characteristic, move_sensor_charact)) // move_sensor
    {
        ESP_LOGI(TAG, "MOVE SENSOR: " UUID_FMT "", UUID_PARAM(data->characteristic));
        extern ble_device_t *devices_list;
        ble_device_t *device=ble_device_find_by_mac(devices_list, data->mac);
        if (device) 
        {
            uint8_t array[6];
            memcpy(array, device->swbd_condition, sizeof(array));

            array[1]=(array[1] & ~((uint8_t)1 << 7)) | (buf[0] << 7);
            array[2]=0xFF;
            array[3]=0xFF;
            array[5]=chsum(array);
          

            ESP_LOGI(TAG, "Write to: " MAC_FMT " , service: " UUID_FMT ", characteristic: " UUID_FMT " buf: " SWBD_FMT "", MAC_PARAM(data->mac), UUID_PARAM(swbd_service), UUID_PARAM(swbd_characteristic), SWBD_PARAM(array));
            ble_characteristic_write(data->mac, swbd_service, swbd_characteristic, data->index, array, 6);        
        }
    }
    else
    if (ble_uuid_equal(data->service, swbd_service)&&ble_uuid_equal(data->characteristic, hours_charact)) // hours
    {
        ESP_LOGI(TAG, "HOURS : " UUID_FMT "", UUID_PARAM(data->characteristic) );
        extern ble_device_t *devices_list;
        ble_device_t *device=ble_device_find_by_mac(devices_list, data->mac);
        if (device) 
        {
            uint8_t array[6];
            memcpy(array, device->swbd_condition, sizeof(array));

            uint16_t time=(uint16_t)((array[2]<<8)|(array[3]));
           // uint8_t hours=time/60; 
            uint8_t minutes=time%60; 
            uint8_t new_hours=buf[0];
            uint16_t new_time=new_hours*60+minutes;


            array[2] = HI(new_time);
			array[3] = LO(new_time);
            
            array[5]=chsum(array);
          

            ESP_LOGI(TAG, "Write to: " MAC_FMT " , service: " UUID_FMT ", characteristic: " UUID_FMT " buf: " SWBD_FMT "", MAC_PARAM(data->mac), UUID_PARAM(swbd_service), UUID_PARAM(swbd_characteristic), SWBD_PARAM(array));
            ble_characteristic_write(data->mac, swbd_service, swbd_characteristic, data->index, array, 6);        
        }
    }
    else
    if (ble_uuid_equal(data->service, swbd_service)&&ble_uuid_equal(data->characteristic, minutes_charact)) // minutes
    {
        ESP_LOGI(TAG, "MINUTES : " UUID_FMT "", UUID_PARAM(data->characteristic) );
        extern ble_device_t *devices_list;
        ble_device_t *device=ble_device_find_by_mac(devices_list, data->mac);
        if (device) 
        {
            uint8_t array[6];
            memcpy(array, device->swbd_condition, sizeof(array));

            uint16_t time=(uint16_t)((array[2]<<8)|(array[3]));
            uint8_t hours=time/60; 
           // uint8_t minutes=time%60; 
            uint8_t new_minutes=buf[0];
            uint16_t new_time=hours*60+new_minutes;


            array[2] = HI(new_time);
			array[3] = LO(new_time);
            
            array[5]=chsum(array);
          

            ESP_LOGI(TAG, "Write to: " MAC_FMT " , service: " UUID_FMT ", characteristic: " UUID_FMT " buf: " SWBD_FMT "", MAC_PARAM(data->mac), UUID_PARAM(swbd_service), UUID_PARAM(swbd_characteristic), SWBD_PARAM(array));
            ble_characteristic_write(data->mac, swbd_service, swbd_characteristic, data->index, array, 6);        
        }
    } 
    else
    {
        ESP_LOGI(TAG, "COMMON");
        ESP_LOGI(TAG, "Write to: " MAC_FMT " , service: " UUID_FMT ", characteristic: " UUID_FMT " buf: " SWBD_FMT "", MAC_PARAM(data->mac), UUID_PARAM(data->service), UUID_PARAM(data->characteristic), SWBD_PARAM(buf));
        ble_characteristic_write(data->mac, data->service, data->characteristic,
        data->index, buf, buf_len);
    }
    

    

    /* Issue a read request to get latest value */
    ble_characteristic_read(data->mac, data->service, data->characteristic,
        data->index);
}

static void _ble_on_mqtt_get(const char *topic, const uint8_t *payload,
    size_t len, void *ctx);
static void _ble_on_mqtt_set(const char *topic, const uint8_t *payload,
    size_t len, void *ctx);

static void ble_on_characteristic_found(mac_addr_t mac, ble_uuid_t service_uuid,
    ble_uuid_t characteristic_uuid, uint8_t index, uint8_t properties)
{
    ESP_LOGD(TAG, "Found new characteristic: service: " UUID_FMT
      ", characteristic: " UUID_FMT ", index: %u, properties: 0x%x",
      UUID_PARAM(service_uuid), UUID_PARAM(characteristic_uuid), index,
      properties);
    char *topic;

    if (!config_ble_service_should_include(uuidtoa(service_uuid)) ||
        !config_ble_characteristic_should_include(uuidtoa(characteristic_uuid)))
    {
        return;
    }

    topic = ble_topic(mac, service_uuid, characteristic_uuid, index);

    /* Characteristic is readable */
    if (properties & CHAR_PROP_READ)
    {
        mqtt_subscribe(ble_topic_suffix(topic, 1), config_mqtt_qos_get(),
            _ble_on_mqtt_get, ble_ctx_gen(mac, service_uuid,
            characteristic_uuid, index), free);
        ble_characteristic_read(mac, service_uuid, characteristic_uuid, index);
    }

    /* Characteristic is writable */
    if (properties & (CHAR_PROP_WRITE | CHAR_PROP_WRITE_NR))
    {
        mqtt_subscribe(ble_topic_suffix(topic, 0), config_mqtt_qos_get(),
            _ble_on_mqtt_set, ble_ctx_gen(mac, service_uuid,
            characteristic_uuid, index), free);
    }

    /* Characteristic can notify / indicate on changes */
    if (properties & (CHAR_PROP_NOTIFY | CHAR_PROP_INDICATE))
    {
        ble_characteristic_notify_register(mac, service_uuid,
            characteristic_uuid, index);
    }

    /*Если это устройство качания, подписываемся на дополнительные топики*/
    if ((memcmp(&swbd_service, service_uuid, 16)==0)&&((memcmp(&swbd_characteristic, characteristic_uuid, 16)==0))) 
    {
        // enable
        char *topic_enable = ble_topic(mac, swbd_service, enable_charact, index);
        mqtt_subscribe(ble_topic_suffix(topic_enable, 0), config_mqtt_qos_get(),
            _ble_on_mqtt_set, ble_ctx_gen(mac, swbd_service,
            enable_charact, index), free);

        // speed
        char *topic_speed = ble_topic(mac, swbd_service, speed_charact, index);
        mqtt_subscribe(ble_topic_suffix(topic_speed, 0), config_mqtt_qos_get(),
            _ble_on_mqtt_set, ble_ctx_gen(mac, swbd_service,
            speed_charact, index), free);

        // sensivity
        char *topic_sensivity = ble_topic(mac, swbd_service, sensivity_charact, index);
        mqtt_subscribe(ble_topic_suffix(topic_sensivity, 0), config_mqtt_qos_get(),
            _ble_on_mqtt_set, ble_ctx_gen(mac, swbd_service,
            sensivity_charact, index), free);    

        // move_sensor
        char *topic_move_sensor = ble_topic(mac, swbd_service, move_sensor_charact, index);
        mqtt_subscribe(ble_topic_suffix(topic_move_sensor, 0), config_mqtt_qos_get(),
            _ble_on_mqtt_set, ble_ctx_gen(mac, swbd_service,
            move_sensor_charact, index), free);    
        
        //hours
        char *topic_hours = ble_topic(mac, swbd_service, hours_charact, index);
        mqtt_subscribe(ble_topic_suffix(topic_hours, 0), config_mqtt_qos_get(),
            _ble_on_mqtt_set, ble_ctx_gen(mac, swbd_service,
            hours_charact, index), free);    

        //minutes
        char *topic_minutes = ble_topic(mac, swbd_service, minutes_charact, index);
        mqtt_subscribe(ble_topic_suffix(topic_minutes, 0), config_mqtt_qos_get(),
            _ble_on_mqtt_set, ble_ctx_gen(mac, swbd_service,
            minutes_charact, index), free);    

        
        
        // посылаем в устройство качания все нули, чтобы узнать его состояние
            uint8_t array[6];
         

            array[0]=0;
            array[1]=0;
            array[2]=0;
            array[3]=0;
            array[4]=0;
            array[5]=chsum(array);

            ESP_LOGI(TAG, "Write to: " MAC_FMT " , service: " UUID_FMT ", characteristic: " UUID_FMT " buf: " SWBD_FMT "", MAC_PARAM(mac), UUID_PARAM(swbd_service), UUID_PARAM(swbd_characteristic), SWBD_PARAM(array));
            ble_characteristic_write(mac, swbd_service, swbd_characteristic, index, array, 6);        
            

    }


}

static void ble_on_device_services_discovered(mac_addr_t mac)
{
    ESP_LOGD(TAG, "Services discovered on device: " MAC_FMT, MAC_PARAM(mac));
    ble_foreach_characteristic(mac, ble_on_characteristic_found);
}

static void ble_on_device_characteristic_value(mac_addr_t mac,
    ble_uuid_t service, ble_uuid_t characteristic, uint8_t index,
    uint8_t *value, size_t value_len)
{
    char *topic = ble_topic(mac, service, characteristic, index);
    char *payload = chartoa(characteristic, value, value_len);
    size_t payload_len = strlen(payload);

    ESP_LOGI(TAG, "Publishing: %s = %s", topic, payload);
    mqtt_publish(topic, (uint8_t *)payload, payload_len, config_mqtt_qos_get(),
        config_mqtt_retained_get());
}

static uint32_t ble_on_passkey_requested(mac_addr_t mac)
{
    char *s = mactoa(mac);
    uint32_t passkey = config_ble_passkey_get(s);

    ESP_LOGI(TAG, "Initiating pairing with %s using the passkey %" PRIu32, s,
        passkey);

    return passkey;
}

/* BLE2MQTT Task and event callbacks */
typedef enum {
    EVENT_TYPE_HEARTBEAT_TIMER,
    EVENT_TYPE_NETWORK_CONNECTED,
    EVENT_TYPE_NETWORK_DISCONNECTED,
    EVENT_TYPE_OTA_MQTT,
    EVENT_TYPE_OTA_COMPLETED,
    EVENT_TYPE_MANAGEMENT_RESTART_MQTT,
    EVENT_TYPE_MQTT_CONNECTED,
    EVENT_TYPE_MQTT_DISCONNECTED,
    EVENT_TYPE_BLE_BROADCASTER_DISCOVERED,
    EVENT_TYPE_BLE_DEVICE_DISCOVERED,
    EVENT_TYPE_BLE_DEVICE_CONNECTED,
    EVENT_TYPE_BLE_DEVICE_DISCONNECTED,
    EVENT_TYPE_BLE_DEVICE_SERVICES_DISCOVERED,
    EVENT_TYPE_BLE_DEVICE_CHARACTERISTIC_VALUE,
    EVENT_TYPE_BLE_MQTT_CONNECTED,
    EVENT_TYPE_BLE_MQTT_GET,
    EVENT_TYPE_BLE_MQTT_SET,
} event_type_t;

typedef struct {
    event_type_t type;
    union {
        struct {
            ota_type_t type;
            ota_err_t err;
        } ota_completed;
        struct {
            char *topic;
            uint8_t *payload;
            size_t len;
            void *ctx;
        } mqtt_message;
        struct {
            mac_addr_t mac;
            uint8_t *adv_data;
            size_t adv_data_len;
            int rssi;
            broadcaster_ops_t *ops;
        } ble_broadcaster_discovered;
        struct {
            mac_addr_t mac;
            char * name;                 // by MK
            size_t name_len;  //by MK
            int rssi;
        } ble_device_discovered;
        struct {
            mac_addr_t mac;
        } ble_device_connected;
        struct {
            mac_addr_t mac;
        } ble_device_disconnected;
        struct {
            mac_addr_t mac;
        } ble_device_services_discovered;
        struct {
            mac_addr_t mac;
            ble_uuid_t service;
            ble_uuid_t characteristic;
            uint8_t index;
            uint8_t *value;
            size_t value_len;
        } ble_device_characteristic_value;
    };
} event_t;

static QueueHandle_t event_queue;

static void ble2mqtt_handle_event(event_t *event)
{
    switch (event->type)
    {
    case EVENT_TYPE_HEARTBEAT_TIMER:
        uptime_publish();
        break;
    case EVENT_TYPE_NETWORK_CONNECTED:
        network_on_connected();
        break;
    case EVENT_TYPE_NETWORK_DISCONNECTED:
        network_on_disconnected();
        break;
    case EVENT_TYPE_OTA_MQTT:
        ota_on_mqtt(event->mqtt_message.topic, event->mqtt_message.payload,
            event->mqtt_message.len, event->mqtt_message.ctx);
        free(event->mqtt_message.topic);
        free(event->mqtt_message.payload);
        break;
    case EVENT_TYPE_OTA_COMPLETED:
        ota_on_completed(event->ota_completed.type, event->ota_completed.err);
        break;
    case EVENT_TYPE_MANAGEMENT_RESTART_MQTT:
        management_on_restart_mqtt(event->mqtt_message.topic,
            event->mqtt_message.payload, event->mqtt_message.len,
            event->mqtt_message.ctx);
        free(event->mqtt_message.topic);
        free(event->mqtt_message.payload);
        break;
    case EVENT_TYPE_MQTT_CONNECTED:
        mqtt_on_connected();
        break;
    case EVENT_TYPE_MQTT_DISCONNECTED:
        mqtt_on_disconnected();
        break;
    case EVENT_TYPE_BLE_BROADCASTER_DISCOVERED:
        ble_on_broadcaster_discovered(event->ble_broadcaster_discovered.mac,
            event->ble_broadcaster_discovered.adv_data,
            event->ble_broadcaster_discovered.adv_data_len,
            event->ble_broadcaster_discovered.rssi,
            event->ble_broadcaster_discovered.ops);
        free(event->ble_broadcaster_discovered.adv_data);
        break;
    case EVENT_TYPE_BLE_DEVICE_DISCOVERED:
        ble_on_device_discovered(event->ble_device_discovered.mac, 
            event->ble_device_discovered.name, // by MK
            event->ble_device_discovered.name_len,
            event->ble_device_discovered.rssi);
        break;
    case EVENT_TYPE_BLE_DEVICE_CONNECTED:
        ble_on_device_connected(event->ble_device_connected.mac);
        break;
    case EVENT_TYPE_BLE_DEVICE_DISCONNECTED:
        ble_on_device_disconnected(event->ble_device_disconnected.mac);
        break;
    case EVENT_TYPE_BLE_DEVICE_SERVICES_DISCOVERED:
        ble_on_device_services_discovered(
            event->ble_device_services_discovered.mac);
        break;
    case EVENT_TYPE_BLE_DEVICE_CHARACTERISTIC_VALUE:
        ble_on_device_characteristic_value(
            event->ble_device_characteristic_value.mac,
            event->ble_device_characteristic_value.service,
            event->ble_device_characteristic_value.characteristic,
            event->ble_device_characteristic_value.index,
            event->ble_device_characteristic_value.value,
            event->ble_device_characteristic_value.value_len);
        free(event->ble_device_characteristic_value.value);
        break;
    case EVENT_TYPE_BLE_MQTT_CONNECTED:
        ble_on_mqtt_connected_cb(event->mqtt_message.topic, event->mqtt_message.payload,
            event->mqtt_message.len, event->mqtt_message.ctx);
        free(event->mqtt_message.topic);
        free(event->mqtt_message.payload);
        break;
    case EVENT_TYPE_BLE_MQTT_GET:
        ble_on_mqtt_get(event->mqtt_message.topic, event->mqtt_message.payload,
            event->mqtt_message.len, event->mqtt_message.ctx);
        free(event->mqtt_message.topic);
        free(event->mqtt_message.payload);
        break;
    case EVENT_TYPE_BLE_MQTT_SET:
        ble_on_mqtt_set(event->mqtt_message.topic, event->mqtt_message.payload,
            event->mqtt_message.len, event->mqtt_message.ctx);
        free(event->mqtt_message.topic);
        free(event->mqtt_message.payload);
        break;
    }

    free(event);
}

static void ble2mqtt_task(void *pvParameter)
{
    event_t *event;

    while (1)
    {
        if (xQueueReceive(event_queue, &event, portMAX_DELAY) != pdTRUE)
            continue;

        ble2mqtt_handle_event(event);
    }

    vTaskDelete(NULL);
}

static void heartbeat_timer_cb(TimerHandle_t xTimer)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_HEARTBEAT_TIMER;

    ESP_LOGD(TAG, "Queuing event HEARTBEAT_TIMER");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static int start_ble2mqtt_task(void)
{
    TimerHandle_t hb_timer;

    if (!(event_queue = xQueueCreate(10, sizeof(event_t *))))
        return -1;

    if (xTaskCreatePinnedToCore(ble2mqtt_task, "ble2mqtt_task", 4096, NULL, 5,
        NULL, 1) != pdPASS)
    {
        return -1;
    }


    hb_timer = xTimerCreate("heartbeat", pdMS_TO_TICKS(60 * 1000), pdTRUE,
        NULL, heartbeat_timer_cb);
    xTimerStart(hb_timer, 0);

    return 0;
}

static void _mqtt_on_message(event_type_t type, const char *topic,
    const uint8_t *payload, size_t len, void *ctx)
{
    event_t *event = malloc(sizeof(*event));

    event->type = type;
    event->mqtt_message.topic = strdup(topic);
    event->mqtt_message.payload = malloc(len);
    memcpy(event->mqtt_message.payload, payload, len);
    event->mqtt_message.len = len;
    event->mqtt_message.ctx = ctx;

    ESP_LOGD(TAG, "Queuing event MQTT message %d (%s, %p, %u, %p)", type, topic,
        payload, len, ctx);
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _network_on_connected(void)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_NETWORK_CONNECTED;

    ESP_LOGD(TAG, "Queuing event NETWORK_CONNECTED");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _network_on_disconnected(void)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_NETWORK_DISCONNECTED;

    ESP_LOGD(TAG, "Queuing event NETWORK_DISCONNECTED");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _ota_on_mqtt(const char *topic, const uint8_t *payload, size_t len,
    void *ctx)
{
    _mqtt_on_message(EVENT_TYPE_OTA_MQTT, topic, payload, len, ctx);
}

static void _ota_on_completed(ota_type_t type, ota_err_t err)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_OTA_COMPLETED;
    event->ota_completed.type = type;
    event->ota_completed.err = err;

    ESP_LOGD(TAG, "Queuing event HEARTBEAT_TIMER (%d, %d)", type, err);
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _management_on_restart_mqtt(const char *topic,
    const uint8_t *payload, size_t len, void *ctx)
{
    _mqtt_on_message(EVENT_TYPE_MANAGEMENT_RESTART_MQTT, topic, payload, len,
        ctx);
}

static void _mqtt_on_connected(void)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_MQTT_CONNECTED;

    ESP_LOGD(TAG, "Queuing event MQTT_CONNECTED");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _mqtt_on_disconnected(void)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_MQTT_DISCONNECTED;

    ESP_LOGD(TAG, "Queuing event MQTT_DISCONNECTED");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _ble_on_broadcaster_discovered(mac_addr_t mac, uint8_t *adv_data,
    size_t adv_data_len, int rssi, broadcaster_ops_t *ops)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_BLE_BROADCASTER_DISCOVERED;
    memcpy(event->ble_broadcaster_discovered.mac, mac, sizeof(mac_addr_t));
    event->ble_broadcaster_discovered.adv_data = malloc(adv_data_len);
    memcpy(event->ble_broadcaster_discovered.adv_data, adv_data, adv_data_len);
    event->ble_broadcaster_discovered.adv_data_len = adv_data_len;
    event->ble_broadcaster_discovered.rssi = rssi;
    event->ble_broadcaster_discovered.ops = ops;

    ESP_LOGD(TAG, "Queuing event BLE_BROADCASTER_DISCOVERED (" MAC_FMT ", "
        "%p, %u, %d)", MAC_PARAM(mac), adv_data, adv_data_len, rssi);
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _ble_on_device_discovered(mac_addr_t mac, char * name, size_t name_len, int rssi) // by MK
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_BLE_DEVICE_DISCOVERED;
    memcpy(event->ble_device_discovered.mac, mac, sizeof(mac_addr_t));
    
   if (name) {
    ESP_LOGI(TAG, "name: %s, name_len: %d", name, name_len); // by MK
    event->ble_device_discovered.name=malloc(name_len+1); 
    memcpy(event->ble_device_discovered.name, name, name_len+1); 
   } else {
    event->ble_device_discovered.name="";
   }
    event->ble_device_discovered.name_len=name_len;

    event->ble_device_discovered.rssi = rssi;

    ESP_LOGD(TAG, "Queuing event BLE_DEVICE_DISCOVERED (" MAC_FMT ", %d)",
        MAC_PARAM(mac), rssi);
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _ble_on_device_connected(mac_addr_t mac)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_BLE_DEVICE_CONNECTED;
    memcpy(event->ble_device_connected.mac, mac, sizeof(mac_addr_t));

    ESP_LOGD(TAG, "Queuing event BLE_DEVICE_CONNECTED (" MAC_FMT ")",
        MAC_PARAM(mac));
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _ble_on_device_disconnected(mac_addr_t mac)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_BLE_DEVICE_DISCONNECTED;
    memcpy(event->ble_device_disconnected.mac, mac, sizeof(mac_addr_t));

    ESP_LOGD(TAG, "Queuing event BLE_DEVICE_DISCONNECTED (" MAC_FMT ")",
        MAC_PARAM(mac));
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _ble_on_device_services_discovered(mac_addr_t mac)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_BLE_DEVICE_SERVICES_DISCOVERED;
    memcpy(event->ble_device_services_discovered.mac, mac, sizeof(mac_addr_t));

    ESP_LOGD(TAG, "Queuing event BLE_DEVICE_SERVICES_DISCOVERED (" MAC_FMT ")",
        MAC_PARAM(mac));
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _ble_on_device_characteristic_value(mac_addr_t mac,
    ble_uuid_t service, ble_uuid_t characteristic, uint8_t index,
    uint8_t *value, size_t value_len)
{
    
    ble_device_t *device;                
    extern ble_device_t *devices_list;
    device = ble_device_find_by_mac(devices_list, mac);     // by MK
    
    
    
    /*От устройства качания пришло его состояние, сохраняем это состояние и публикуем его отдельные параметры*/                                            
    if ((memcmp(&swbd_service, service, 16)==0)&&((memcmp(&swbd_characteristic, characteristic, 16)==0))) {
        // сохраняем состояние устройства качания
        memcpy(device->swbd_condition, value, sizeof(device->swbd_condition));
        ESP_LOGI(TAG, "Saved SWBD cond: " SWBD_FMT "", SWBD_PARAM(device->swbd_condition));

        // ставим в очередь публикацию состояние "запущено ли качание"
        event_t *enable_event = malloc(sizeof(*enable_event));
        enable_event->type = EVENT_TYPE_BLE_DEVICE_CHARACTERISTIC_VALUE;
        memcpy(enable_event->ble_device_characteristic_value.mac, mac, sizeof(mac_addr_t));
        memcpy(enable_event->ble_device_characteristic_value.service, service, sizeof(ble_uuid_t)); 
        memcpy(enable_event->ble_device_characteristic_value.characteristic, enable_charact, sizeof(ble_uuid_t));

        uint8_t enable_value=get_swbd_enable(value);
        enable_event->ble_device_characteristic_value.value = malloc(1);
        memcpy(enable_event->ble_device_characteristic_value.value, &enable_value, 1);
        enable_event->ble_device_characteristic_value.value_len = 1;
        enable_event->ble_device_characteristic_value.index = index;

        xQueueSend(event_queue, &enable_event, portMAX_DELAY);


        // ставим в очередь публикацию скорости
        event_t *speed_event = malloc(sizeof(*speed_event));
        speed_event->type = EVENT_TYPE_BLE_DEVICE_CHARACTERISTIC_VALUE;
        memcpy(speed_event->ble_device_characteristic_value.mac, mac, sizeof(mac_addr_t));
        memcpy(speed_event->ble_device_characteristic_value.service, service, sizeof(ble_uuid_t)); 
        memcpy(speed_event->ble_device_characteristic_value.characteristic, speed_charact, sizeof(ble_uuid_t));

        uint8_t speed_value=get_swbd_speed(value);
        speed_event->ble_device_characteristic_value.value = malloc(1);
        memcpy(speed_event->ble_device_characteristic_value.value, &speed_value, 1);
        speed_event->ble_device_characteristic_value.value_len = 1;
        speed_event->ble_device_characteristic_value.index = index;

        xQueueSend(event_queue, &speed_event, portMAX_DELAY);

        // ставим в очередь публикацию чувствительности микрофона
        event_t *sensivity_event = malloc(sizeof(*sensivity_event));
        sensivity_event->type = EVENT_TYPE_BLE_DEVICE_CHARACTERISTIC_VALUE;
        memcpy(sensivity_event->ble_device_characteristic_value.mac, mac, sizeof(mac_addr_t));
        memcpy(sensivity_event->ble_device_characteristic_value.service, service, sizeof(ble_uuid_t)); 
        memcpy(sensivity_event->ble_device_characteristic_value.characteristic, sensivity_charact, sizeof(ble_uuid_t));

        uint8_t sensivity_value=get_swbd_sensivity(value);
        sensivity_event->ble_device_characteristic_value.value = malloc(1);
        memcpy(sensivity_event->ble_device_characteristic_value.value, &sensivity_value, 1);
        sensivity_event->ble_device_characteristic_value.value_len = 1;
        sensivity_event->ble_device_characteristic_value.index = index;

        xQueueSend(event_queue, &sensivity_event, portMAX_DELAY);

        // ставим в очередь публикацию состояния датчика движения
        event_t *move_sensor_event = malloc(sizeof(*move_sensor_event));
        move_sensor_event->type = EVENT_TYPE_BLE_DEVICE_CHARACTERISTIC_VALUE;
        memcpy(move_sensor_event->ble_device_characteristic_value.mac, mac, sizeof(mac_addr_t));
        memcpy(move_sensor_event->ble_device_characteristic_value.service, service, sizeof(ble_uuid_t)); 
        memcpy(move_sensor_event->ble_device_characteristic_value.characteristic, move_sensor_charact, sizeof(ble_uuid_t));

        uint8_t move_sensor_value=get_swbd_move_sensor(value);
        move_sensor_event->ble_device_characteristic_value.value = malloc(1);
        memcpy(move_sensor_event->ble_device_characteristic_value.value, &move_sensor_value, 1);
        move_sensor_event->ble_device_characteristic_value.value_len = 1;
        move_sensor_event->ble_device_characteristic_value.index = index;

        xQueueSend(event_queue, &move_sensor_event, portMAX_DELAY);    

        // ставим в очередь публикацию количества часов
        event_t *hours_event = malloc(sizeof(*hours_event));
        hours_event->type = EVENT_TYPE_BLE_DEVICE_CHARACTERISTIC_VALUE;
        memcpy(hours_event->ble_device_characteristic_value.mac, mac, sizeof(mac_addr_t));
        memcpy(hours_event->ble_device_characteristic_value.service, service, sizeof(ble_uuid_t)); 
        memcpy(hours_event->ble_device_characteristic_value.characteristic, hours_charact, sizeof(ble_uuid_t));

        uint8_t hours_value=get_swbd_hours(value);
        hours_event->ble_device_characteristic_value.value = malloc(1);
        memcpy(hours_event->ble_device_characteristic_value.value, &hours_value, 1);
        hours_event->ble_device_characteristic_value.value_len = 1;
        hours_event->ble_device_characteristic_value.index = index;

        xQueueSend(event_queue, &hours_event, portMAX_DELAY);    
        
        // ставим в очередь публикацию количества минут
        event_t *minutes_event = malloc(sizeof(*minutes_event));
        minutes_event->type = EVENT_TYPE_BLE_DEVICE_CHARACTERISTIC_VALUE;
        memcpy(minutes_event->ble_device_characteristic_value.mac, mac, sizeof(mac_addr_t));
        memcpy(minutes_event->ble_device_characteristic_value.service, service, sizeof(ble_uuid_t)); 
        memcpy(minutes_event->ble_device_characteristic_value.characteristic, minutes_charact, sizeof(ble_uuid_t));

        uint8_t minutes_value=get_swbd_minutes(value);
        minutes_event->ble_device_characteristic_value.value = malloc(1);
        memcpy(minutes_event->ble_device_characteristic_value.value, &minutes_value, 1);
        minutes_event->ble_device_characteristic_value.value_len = 1;
        minutes_event->ble_device_characteristic_value.index = index;

        xQueueSend(event_queue, &minutes_event, portMAX_DELAY);    
    }

    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_BLE_DEVICE_CHARACTERISTIC_VALUE;
    memcpy(event->ble_device_characteristic_value.mac, mac, sizeof(mac_addr_t));
    memcpy(event->ble_device_characteristic_value.service, service,
        sizeof(ble_uuid_t));
    memcpy(event->ble_device_characteristic_value.characteristic,
        characteristic, sizeof(ble_uuid_t));
    event->ble_device_characteristic_value.value = malloc(value_len);
    memcpy(event->ble_device_characteristic_value.value, value, value_len);
    event->ble_device_characteristic_value.value_len = value_len;
    event->ble_device_characteristic_value.index = index;

    ESP_LOGD(TAG, "Queuing event BLE_DEVICE_CHARACTERISTIC_VALUE (" MAC_FMT ", "
        UUID_FMT ", %p, %u)", MAC_PARAM(mac), UUID_PARAM(characteristic), value,
        value_len);
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _ble_on_mqtt_connected_cb(const char *topic, const uint8_t *payload,
    size_t len, void *ctx)
{
    _mqtt_on_message(EVENT_TYPE_BLE_MQTT_CONNECTED, topic, payload, len, ctx);
}

static void _ble_on_mqtt_get(const char *topic, const uint8_t *payload,
    size_t len, void *ctx)
{
    _mqtt_on_message(EVENT_TYPE_BLE_MQTT_GET, topic, payload, len, ctx);
}

static void _ble_on_mqtt_set(const char *topic, const uint8_t *payload,
    size_t len, void *ctx)
{
    _mqtt_on_message(EVENT_TYPE_BLE_MQTT_SET, topic, payload, len, ctx);
}

void app_main()
{
    int config_failed;

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Version: %s", BLE2MQTT_VER);

    /* Init configuration */
    config_failed = config_initialize();

    /* Init remote logging */
    ESP_ERROR_CHECK(log_initialize());

    /* Init OTA */
    ESP_ERROR_CHECK(ota_initialize());

    /* Init Network */
    switch (config_network_type_get())
    {
    case NETWORK_TYPE_ETH:
        /* Init Ethernet */
        ESP_ERROR_CHECK(eth_initialize());
        eth_hostname_set(device_name_get());
        eth_set_on_connected_cb(_network_on_connected);
        eth_set_on_disconnected_cb(_network_on_disconnected);
        break;
    case NETWORK_TYPE_WIFI:
        /* Init Wi-Fi */
        ESP_ERROR_CHECK(wifi_initialize());
        wifi_hostname_set(device_name_get());
        wifi_set_on_connected_cb(_network_on_connected);
        wifi_set_on_disconnected_cb(_network_on_disconnected);
        break;
    }

    /* Init mDNS */
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(device_name_get());

    /* Init name resolver */
    ESP_ERROR_CHECK(resolve_initialize());

    /* Init MQTT */
    ESP_ERROR_CHECK(mqtt_initialize());
    mqtt_set_on_connected_cb(_mqtt_on_connected);
    mqtt_set_on_disconnected_cb(_mqtt_on_disconnected);

    /* Init BLE */
    ESP_ERROR_CHECK(ble_initialize());
    ble_set_on_broadcaster_discovered_cb(_ble_on_broadcaster_discovered);
    ble_set_on_device_discovered_cb(_ble_on_device_discovered);
    ble_set_on_device_connected_cb(_ble_on_device_connected);
    ble_set_on_device_disconnected_cb(_ble_on_device_disconnected);
    ble_set_on_device_services_discovered_cb(
        _ble_on_device_services_discovered);
    ble_set_on_device_characteristic_value_cb(
        _ble_on_device_characteristic_value);
    ble_set_on_passkey_requested_cb(ble_on_passkey_requested);

    /* Init web server */
    ESP_ERROR_CHECK(httpd_initialize());
    httpd_set_on_ota_completed_cb(_ota_on_completed);

    /* Start BLE2MQTT task */
    ESP_ERROR_CHECK(start_ble2mqtt_task());

    /* кнопка */
    gpio_reset_pin(AP_BUTTON);
    // Настраиваем вывод AP_BUTTON на вход с подтяжкой к +3.3
    gpio_set_direction(AP_BUTTON, GPIO_MODE_INPUT);
    gpio_set_pull_mode(AP_BUTTON, GPIO_PULLUP_ONLY);
    int isPressed = !gpio_get_level(AP_BUTTON);

    /* Failed to load configuration or it wasn't set, create access point */
    if (isPressed || config_failed || !strcmp(config_network_wifi_ssid_get() ? : "", "MY_SSID"))
    {
        wifi_start_ap(device_name_get(), NULL);
        return;
    }

    switch (config_network_type_get())
    {
    case NETWORK_TYPE_ETH:
        eth_connect(eth_phy_atophy(config_network_eth_phy_get()),
            config_network_eth_phy_power_pin_get());
        break;
    case NETWORK_TYPE_WIFI:
        /* Start by connecting to network */
        wifi_connect(config_network_wifi_ssid_get(), config_network_wifi_password_get(),
            wifi_eap_atomethod(config_eap_method_get()),
            config_eap_identity_get(),
            config_eap_username_get(), config_eap_password_get(),
            config_eap_ca_cert_get(), config_eap_client_cert_get(),
            config_eap_client_key_get());
        break;
    }
}
