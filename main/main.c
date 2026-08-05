#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

/**
 * @brief WIFI事件处理函数
 */
void wifi_event_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data) {
  switch (event_id) {
  case WIFI_EVENT_STA_START:
    esp_wifi_connect();
    break;
  case WIFI_EVENT_STA_CONNECTED:
    /// TODO: 通知MCU连接AP成功
    break;
  case WIFI_EVENT_STA_DISCONNECTED:
    /// TODO: 通知MCU已断开连接
    break;
  case WIFI_EVENT_STA_STOP:
    break;
  default:
    break;
  }
}

/**
 * @brief STA获取IP地址事件处理函数
 */
void sta_got_ip_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data) {

  ip_event_got_ip_t *got_ip_event_data = (ip_event_got_ip_t *)event_data;
  /// TODO: 通知MCU已获取IP地址

  /// 场景1：与AP处于连接状态，IP地址租约到期，DHCP服务端IP地址池发生变化，取得了新的IP地址。
}

void sta_lost_ip_handler(void *arg, esp_event_base_t event_base,
                         int32_t event_id, void *event_data) {
  ESP_LOGI("sta_lost_ip_handler", "Lost address");
  /// TODO: 通知MCU已丢失IP地址

  /// 场景1：与AP处于连接状态，IP地址租约到期，DHCP服务端关闭。
  /// 场景2：与AP处于连接状态，IP地址租约到期，DHCP服务端未关闭，但IP地址池已耗尽。
}

void wifi_init_sta(void) {
  // If you want to open more logs in the wifi module, you need to make
  // the max level greater than the default level, and call
  // esp_log_level_set() before esp_wifi_init() to improve the log level of
  // the wifi module.
  if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL) {
    esp_log_level_set("wifi", CONFIG_LOG_MAXIMUM_LEVEL);
  }

  // Initialize LwIP
  ESP_ERROR_CHECK(esp_netif_init());

  // Create default wifi station interface
  esp_netif_create_default_wifi_sta();

  // Initialize wifi
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  // Set wifi mode to station
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  wifi_config_t wifi_config = {
      .sta = {.ssid = CONFIG_AP_SSID,
              .password = CONFIG_AP_PASSWORD,
              .threshold.authmode = WIFI_AUTH_OPEN},
  };

  // Set wifi configuration
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

  // Register event handlers
  esp_event_handler_instance_t instance_wifi_event_any_id;
  esp_event_handler_instance_t instance_ip_event_sta_got_ip;
  esp_event_handler_instance_t instance_ip_event_sta_lost_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
      &instance_wifi_event_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_got_ip_handler, NULL,
      &instance_ip_event_sta_got_ip));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_LOST_IP, &sta_lost_ip_handler, NULL,
      &instance_ip_event_sta_lost_ip));

  // Start wifi station
  ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void) {
  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ESP_ERR_NVS_NO_FREE_PAGES == ret ||
      ESP_ERR_NVS_NEW_VERSION_FOUND == ret) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // Initialize default event loop (shared by all components)
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Initialize wifi station
  wifi_init_sta();
}
