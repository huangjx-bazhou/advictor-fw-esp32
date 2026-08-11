#include "esp_clk_tree.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

/// 普通情形
/// 在芯片上电之前，路由器正常，这时给芯片上电，会先触发WIFI_EVENT_STA_CONNECTED事件，然后触发IP_EVENT_STA_GOT_IP事件，ip_changed=true

/// 黑名单相关
/// 1.芯片上电之前已被加入黑名单，会先触发WIFI_EVENT_STA_DISCONNECTED事件，再过ESP_NETIF_IP_LOST_TIMER_INTERVAL秒，触发IP_EVENT_STA_LOST_IP事件，reason=WIFI_REASON_AUTH_FAIL
/// 2.在正常连接的情况下加入黑名单，会先触发WIFI_EVENT_STA_DISCONNECTED事件，再过ESP_NETIF_IP_LOST_TIMER_INTERVAL秒，触发IP_EVENT_STA_LOST_IP事件，reason=WIFI_REASON_CLASS2_FRAME_FROM_NONAUTH_STA

/// DHCP相关
/// 1.在芯片拿到IP地址之后，路由器关闭DHCP服务器，等租期到期时，再过ESP_NETIF_IP_LOST_TIMER_INTERVAL秒，会触发IP_EVENT_STA_LOST_IP事件。
/// 2.再次开启路由器的DHCP服务器，芯片会重新获取IP地址，会触发IP_EVENT_STA_GOT_IP事件，ip_changed=true。
/// 3.在芯片拿到IP地址之后，路由器修改DHCP服务器地址池，地址池不包括芯片拿到的IP地址，IP地址租期过一半时，芯片会触发IP_EVENT_STA_GOT_IP事件，ip_changed=true。
/// 4.芯片上电之前，路由器DHCP的地址池已耗尽，芯片上电后会触发WIFI_EVENT_STA_CONNECTED事件，触发IP_EVENT_STA_LOST_IP事件，这时芯片会有一个默认的IP地址
///   当路由器DHCP的地址池恢复正常后，芯片会重新获取IP地址，如果获取的IP地址和默认的IP地址不一样，会触发IP_EVENT_STA_GOT_IP事件，ip_changed=true。

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
    ESP_LOGI(__func__, "connected to ap");
    /// TODO: 通知MCU连接AP成功
    break;
  case WIFI_EVENT_STA_DISCONNECTED:
    wifi_event_sta_disconnected_t *sta_disconnected_event_data =
        (wifi_event_sta_disconnected_t *)event_data;

    ESP_LOGI(__func__, "disconnected from ap, reason: %d",
             sta_disconnected_event_data->reason);
    /// TODO: 通知MCU已断开连接

    /// TODO: 根据sta_disconnected_event_data->reason判断是否需要重连
    switch (sta_disconnected_event_data->reason) {
    default:
      break;
    }
    break;
  case WIFI_EVENT_STA_STOP:
    break;
  case WIFI_EVENT_STA_BEACON_OFFSET_UNSTABLE:
    wifi_event_sta_beacon_offset_unstable_t
        *sta_beacon_offset_unstable_event_data =
            (wifi_event_sta_beacon_offset_unstable_t *)event_data;
    ESP_LOGI(__func__, "unstable sample, beacon success rate: %.4f",
             sta_beacon_offset_unstable_event_data->beacon_success_rate);
#if CONFIG_ESP_WIFI_SLP_SAMPLE_BEACON_FEATURE
    esp_wifi_beacon_offset_sample_beacon();
#endif
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
  /// TODO: 通知MCU已获取IP地址

  ip_event_got_ip_t *got_ip_event_data = (ip_event_got_ip_t *)event_data;
  ESP_LOGI(__func__, "ip_changed: %d", got_ip_event_data->ip_changed);
}

/**
 * @brief STA丢失IP地址事件处理函数
 */
void sta_lost_ip_handler(void *arg, esp_event_base_t event_base,
                         int32_t event_id, void *event_data) {
  ESP_LOGI(__func__, "lost address");
  /// TODO: 通知MCU已丢失IP地址
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

  // Configure dynamic frequency scaling:
  // maximum and minimum frequencies are set in sdkconfig,
  // automatic light sleep is enabled if tickless idle support is enabled.
#if CONFIG_PM_ENABLE & CONFIG_FREERTOS_USE_TICKLESS_IDLE
  esp_pm_config_t pm_config;
  if (ESP_OK == esp_pm_get_configuration(&pm_config)) {
    pm_config.light_sleep_enable = true;
    esp_pm_configure(&pm_config);
  }
#endif // CONFIG_PM_ENABLE & CONFIG_FREERTOS_USE_TICKLESS_IDLE

  // Initialize default event loop (shared by all components)
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Initialize wifi station
  wifi_init_sta();
}
