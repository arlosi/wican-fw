/*
 * This file is part of the WiCAN project.
 *
 * Copyright (C) 2022  Meatpi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include  "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>
#include "comm_server.h"
#include "lwip/sockets.h"
#include "driver/twai.h"
#include "types.h"
#include "config_server.h"
#include "realdash.h"
#include "slcan.h"
#include "can.h"
#include "ble.h"
#include "wifi_network.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "sleep_mode.h"
#include "ble.h"
#include "esp_sleep.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "ver.h"
#include "cJSON.h"
#include "wifi_network.h"
#include "mqtt.h"

#define TAG 		__func__

static EventGroupHandle_t s_mqtt_event_group = NULL;
#define MQTT_CONNECTED_BIT 			BIT0
#define PUB_SUCCESS_BIT     		BIT1
static esp_mqtt_client_handle_t client = NULL;
static char *device_id;
static char mqtt_sub_topic[128];
static char mqtt_status_topic[128];
static uint8_t mqtt_led = 0;

static QueueHandle_t *xmqtt_tx_queue;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
//    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id)
    {
		case MQTT_EVENT_CONNECTED:
			ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
			xEventGroupSetBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);

			esp_mqtt_client_subscribe(client, mqtt_sub_topic, 0);
			gpio_set_level(mqtt_led, 0);
			esp_mqtt_client_publish(client, mqtt_status_topic, "{\"status\": \"online\"}", 0, 0, 0);
			break;
		case MQTT_EVENT_DISCONNECTED:
			ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
			xEventGroupClearBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
			gpio_set_level(mqtt_led, 1);
	//        esp_mqtt_client_stop(client);
			break;

		case MQTT_EVENT_SUBSCRIBED:
			ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
			break;
		case MQTT_EVENT_UNSUBSCRIBED:
			ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
			break;
		case MQTT_EVENT_PUBLISHED:
			ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
			xEventGroupSetBits(s_mqtt_event_group, PUB_SUCCESS_BIT);
			break;
		case MQTT_EVENT_DATA:
			ESP_LOGI(TAG, "MQTT_EVENT_DATA");
//			printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
//			printf("DATA=%.*s\r\n", event->data_len, event->data);
			break;
		case MQTT_EVENT_ERROR:
			ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
	//        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
	//            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
	//            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
	//            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
	//            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
	//
	//        }
			break;
		default:
			ESP_LOGI(TAG, "Other event id:%d", event->event_id);
			break;
    }
}

//receive can topic:wican/84f703406f75/can/rx
///received message json format:{"bus":"0","type":"rx","ts":34610,"frame":[{"id":123,"dlc":8,"rtr":false,"extd":false,"data":[1,2,3,4,5,6,7,8]},{"id":124,"dlc":8,"rtr":false,"extd":true,"data":[1,2,3,4,5,6,7,8]}]}

//send to can topic:wican/84f703406f75/can/tx
//send message json format: {"bus":0,"type":"tx","frame":[{"id":123,"dlc":8,"rtr":false,"extd":true,"data":[1,2,3,4,5,6,7,8]},{"id":124,"dlc":8,"rtr":false,"extd":true,"data":[1,2,3,4,5,6,7,8]}]}

//															 	    id:0x7E0                                          PID: 47 or0x2F
//get fuel level send: {"bus":0,"type":"tx","ts":35519,"frame":[{"id":2016,"dlc":8,"rtr":false,"extd":false,"data":[2,1,47,170,170,170,170,170]}]}

static void mqtt_parse_data(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
	twai_message_t can_frame;
	cJSON * root;
	cJSON *frame = NULL;
	esp_mqtt_event_handle_t event = event_data;

	if(strncmp(event->topic, mqtt_sub_topic, strlen(mqtt_sub_topic)) == 0)
	{
		root = cJSON_Parse(event->data);

		if (root == NULL)
		{

			goto end;
		}

		cJSON *frame_ary = cJSON_GetObjectItem(root, "frame");

		if(frame_ary ==  NULL || !cJSON_IsArray(frame_ary))
		{
			ESP_LOGE(TAG, "error parsing json frame");
			goto end;
		}

		for (int i = 0 ; i < cJSON_GetArraySize(frame_ary) ; i++)
		{

			frame = cJSON_GetArrayItem(frame_ary, i);

			if(frame ==  NULL)
			{
				ESP_LOGE(TAG, "error parsing json here");
				goto end;
			}

			cJSON * frame_data = cJSON_GetObjectItem(frame, "data");

			if (frame_data == NULL || !cJSON_IsArray(frame_data))
			{
				goto end;
			}

			cJSON * frame_id = cJSON_GetObjectItem(frame, "id");
			if (frame_id == NULL || !cJSON_IsNumber(frame_id))
			{
				goto end;
			}

			cJSON * frame_dlc = cJSON_GetObjectItem(frame, "dlc");
			if (frame_dlc == NULL || !cJSON_IsNumber(frame_dlc))
			{
				goto end;
			}

			cJSON * frame_extd = cJSON_GetObjectItem(frame, "extd");
			if (frame_extd == NULL || !cJSON_IsBool(frame_extd))
			{
				goto end;
			}

			cJSON * frame_rtr = cJSON_GetObjectItem(frame, "rtr");
			if (frame_rtr == NULL || !cJSON_IsBool(frame_rtr))
			{
				goto end;
			}

			can_frame.identifier = cJSON_IsTrue(frame_rtr) ? frame_id->valueint&TWAI_EXTD_ID_MASK:frame_id->valueint&TWAI_STD_ID_MASK;//(frame_id->valuedouble > 0x1FFFFFFF) ? 0x1FFFFFFF:frame_id->valuedouble;
			can_frame.data_length_code = (frame_dlc->valuedouble > 8) ? 8:frame_dlc->valuedouble;
			can_frame.extd = cJSON_IsTrue(frame_extd)?1:0;
			can_frame.rtr = cJSON_IsTrue(frame_rtr)?1:0;;

			ESP_LOGI(TAG, " frame_id: %u, frame_dlc: %u ", (uint32_t)frame_id->valuedouble, (uint32_t)frame_dlc->valuedouble);

			for (int i = 0 ; i < cJSON_GetArraySize(frame_data) ; i++)
			{
				ESP_LOGI(TAG, " data: %d ",cJSON_GetArrayItem(frame_data, i)->valueint);
				can_frame.data[i] = cJSON_GetArrayItem(frame_data, i)->valueint;
			}
			can_frame.self = 0;
			can_enable();
			can_send(&can_frame, 1);
		}
	}
    return;
end:
	ESP_LOGE(TAG, "error parsing json");
	cJSON_Delete(root);
}

int mqtt_connected(void)
{
	EventBits_t uxBits;
	if(s_mqtt_event_group != NULL)
	{
		uxBits = xEventGroupGetBits(s_mqtt_event_group);

		return (uxBits & MQTT_CONNECTED_BIT)?1:0;
	}
	else return 0;
}

#define JSON_BUF_SIZE		2048
static void mqtt_task(void *pvParameters)
{
	static xdev_buffer tx_buffer;
	static char json_buffer[JSON_BUF_SIZE] = {0};
	static char tmp[150];
	twai_message_t tx_frame;
	static char mqtt_topic[128];

	sprintf(mqtt_topic, "wican/%s/can/rx", device_id);

	while(!wifi_network_is_connected())
	{
		vTaskDelay(pdMS_TO_TICKS(1000));
	}

	/* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
	esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
	esp_mqtt_client_register_event(client, MQTT_EVENT_DATA, mqtt_parse_data, NULL);
	esp_mqtt_client_start(client);

	while(1)
	{
		xQueuePeek(*xmqtt_tx_queue, ( void * ) &tx_frame, portMAX_DELAY);
		if(mqtt_connected())
		{
			json_buffer[0] = 0;

	//		sprintf(mqtt_data, "{\"bus\":\"0\",\"type\":\"rx\",\"frame\":{\"id\":%u,\"dlc\":%u,\"rtr\":%s,\"extd\":%s,\"data\":[%u,%u,%u,%u,%u,%u,%u,%u]}}",
	//				message->identifier, message->data_length_code, message->rtr?"true":"false", message->extd?"true":"false",message->data[0], message->data[1], message->data[2], message->data[3],
	//				message->data[4], message->data[5], message->data[6], message->data[7]);
			sprintf(json_buffer, "{\"bus\":\"0\",\"type\":\"rx\",\"ts\":%u,\"frame\":[", (pdTICKS_TO_MS(xTaskGetTickCount())%60000));
	//pdTICKS_TO_MS(xTaskGetTickCount())%60000

			if(strlen(json_buffer) < (sizeof(json_buffer) -128))
			{
				while(xQueuePeek(*xmqtt_tx_queue, ( void * ) &tx_buffer, 0) == pdTRUE)
				{
					xQueueReceive(*xmqtt_tx_queue, ( void * ) &tx_buffer, 0);
					sprintf(tmp, "{\"id\":%u,\"dlc\":%u,\"rtr\":%s,\"extd\":%s,\"data\":[%u,%u,%u,%u,%u,%u,%u,%u]},",tx_frame.identifier, tx_frame.data_length_code, tx_frame.rtr?"true":"false",
																												tx_frame.extd?"true":"false",tx_frame.data[0], tx_frame.data[1], tx_frame.data[2], tx_frame.data[3],
																												tx_frame.data[4], tx_frame.data[5], tx_frame.data[6], tx_frame.data[7]);
					strcat((char*)json_buffer, (char*)tmp);

					if(strlen(json_buffer) > (JSON_BUF_SIZE - 100))
					{
						break;
					}
				}
				json_buffer[strlen(json_buffer)-1] = 0;
			}
			strcat((char*)json_buffer, "]}");

			esp_mqtt_client_publish(client, mqtt_topic, json_buffer, 0, 0, 0);

		}
		vTaskDelay(pdMS_TO_TICKS(1));
	}

}

//wifi_network_is_connected
void mqtt_init(char* id, uint8_t connected_led, QueueHandle_t *xtx_queue)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = config_server_get_mqtt_url(),
		.port = config_server_get_mqtt_port(),
		.username = config_server_get_mqtt_user(),
		.password = config_server_get_mmqtt_pass(),
		.disable_auto_reconnect = false,
		.reconnect_timeout_ms = 5000,
		.out_buffer_size = 1024*5,
        .buffer_size = 1024*5,
		.lwt_topic = mqtt_status_topic,
		.lwt_msg = "{\"status\": \"offline\"}",
		.keepalive = 30
    };
    xmqtt_tx_queue = xtx_queue;
    mqtt_led = connected_led;
    device_id = id;
    sprintf(mqtt_sub_topic, "wican/%s/can/tx", device_id);
    sprintf(mqtt_status_topic, "wican/%s/status", device_id);

    ESP_LOGI(TAG, "device_id: %s, mqtt_cfg.uri: %s", device_id, mqtt_cfg.uri);


    s_mqtt_event_group = xEventGroupCreate();
    client = esp_mqtt_client_init(&mqtt_cfg);

    xTaskCreate(mqtt_task, "mqtt_task", 4096, (void*)AF_INET, 5, NULL);
}

