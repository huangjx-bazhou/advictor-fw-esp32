#include <esp_err.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_wifi.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <mdns.h>
#include <nvs_flash.h>
#include <unistd.h>

#if !defined(CONFIG_HTTPD_WS_SUPPORT) || !CONFIG_HTTPD_WS_SUPPORT
#error "CONFIG_HTTPD_WS_SUPPORT must be defined and enabled"
#endif

/******************************************************************************************************************************
 *                             WebSocket Server
 ******************************************************************************************************************************/

static esp_err_t websocket_open_cb(httpd_handle_t hd, int sockfd) {
  (void)hd;
  ESP_LOGI(__func__, "new session opened, fd=%d", sockfd);

  /// TODO: MDNS

  return ESP_OK;
}

static void websocket_close_cb(httpd_handle_t hd, int sockfd) {
  (void)hd;
  ESP_LOGI(__func__, "session closed, fd=%d", sockfd);
  if (sockfd >= 0) {
    close(sockfd);
  }

  /// TODO: MDNS
}

static esp_err_t websocket_handler(httpd_req_t *req) {
  httpd_ws_frame_t ws_pkt;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

  /* Set max_len = 0 to get the frame len */
  esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
  if (ESP_OK != ret) {
    ESP_LOGW(__func__, "httpd_ws_recv_frame failed to get frame len with %d",
             ret);
    return ret;
  }

  ESP_LOGI(__func__, "frame len is %d", ws_pkt.len);

  if (ws_pkt.len > 0) {
    /* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
    uint8_t *buf = NULL;
    buf = calloc(ws_pkt.len + 1, sizeof(uint8_t));
    if (NULL == buf) {
      ESP_LOGW(__func__, "Failed to calloc memory for buf");
      return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = buf;

    /* Set max_len = ws_pkt.len to get the frame payload */
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ESP_OK != ret) {
      ESP_LOGW(__func__, "httpd_ws_recv_frame failed with %d", ret);
      free(ws_pkt.payload);
      return ret;
    }
    ESP_LOGI(__func__, "Got packet with message: %s", ws_pkt.payload);
  }

  ESP_LOGI(__func__, "Packet type: %d", ws_pkt.type);

  // Handle the WebSocket frame based on its type
  switch (ws_pkt.type) {
  case HTTPD_WS_TYPE_TEXT:
    ESP_LOGI(__func__, "Received a text frame");
    break;
  case HTTPD_WS_TYPE_BINARY:
    ESP_LOGI(__func__, "Received a binary frame");
    break;
  case HTTPD_WS_TYPE_PING:
    ESP_LOGI(__func__, "Received a ping frame");
    ws_pkt.type = HTTPD_WS_TYPE_PONG;
    ret = httpd_ws_send_frame(req, &ws_pkt);
    if (ESP_OK != ret) {
      ESP_LOGW(__func__, "Failed to send pong frame with %d", ret);
    }
    break;
  case HTTPD_WS_TYPE_PONG:
    ESP_LOGI(__func__, "Received a pong frame");
    /// TODO:
    break;
  case HTTPD_WS_TYPE_CLOSE:
    ESP_LOGI(__func__, "Received a close frame");
    break;
  default:
    ESP_LOGI(__func__, "Received an unknown frame type");
    break;
  }

  // Free the allocated payload memory if it exists
  if (NULL != ws_pkt.payload) {
    free(ws_pkt.payload);
  }

  // Return ESP_OK to indicate successful handling of the WebSocket frame
  return ret;
}

static const httpd_uri_t root_handler = {.uri = "/",
                                         .method = HTTP_GET,
                                         .handler = websocket_handler,
                                         .user_ctx = NULL,
                                         .is_websocket = true,
                                         .handle_ws_control_frames = true};

static void start_websocket_server(void) {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.open_fn = websocket_open_cb;
  config.close_fn = websocket_close_cb;

  // Configure and start the HTTP server
  ESP_LOGI(__func__, "Starting server on port: '%d'", config.server_port);
  esp_err_t err = httpd_start(&server, &config);

  if (ESP_OK != err) {
    ESP_LOGW(__func__, "Failed to start the server: Code = %d, Message = %s",
             err, esp_err_to_name(err));
    return;
  }

  // Register the root URI handler which is also a WebSocket handler
  ESP_LOGI(__func__, "Registering URI handlers");
  err = httpd_register_uri_handler(server, &root_handler);

  if (ESP_OK != err) {
    ESP_LOGW(__func__,
             "Failed to register URI handlers: Code = %d, Message = %s", err,
             esp_err_to_name(err));
  }
}

/******************************************************************************************************************************
 *                              mDNS
 ******************************************************************************************************************************/

static bool mdns_started = false;
static bool mdns_http_service_added = false;

static void start_mdns_service(void) {
  if (!mdns_started) {
    esp_err_t err = mdns_init();
    if (ESP_OK != err) {
      ESP_LOGW(__func__, "Failed to init mdns: Code = %d, Message = %s", err,
               esp_err_to_name(err));
      return;
    }
    mdns_started = true;
  }

  ESP_ERROR_CHECK(mdns_hostname_set("nirs"));
  ESP_ERROR_CHECK(mdns_instance_name_set("Jhon's ESP32 Thing"));

  if (!mdns_http_service_added) {
    esp_err_t err = mdns_service_add("nirs-http", "_nirs", "_tcp", 80, NULL, 0);
    if (ESP_OK != err) {
      ESP_LOGW(__func__, "Failed to add mdns service: Code = %d, Message = %s",
               err, esp_err_to_name(err));
      return;
    }
    mdns_http_service_added = true;
  }
}

static void stop_mdns_service(void) {
  if (!mdns_started) {
    return;
  }

  mdns_free();
  mdns_started = false;
  mdns_http_service_added = false;
}

/******************************************************************************************************************************
 *                              Wifi Station
 ******************************************************************************************************************************/

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

    esp_wifi_connect();
    //stop_mdns_service();
    break;
  case WIFI_EVENT_STA_STOP:
    stop_mdns_service();
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
  ip_event_got_ip_t *got_ip_event_data = (ip_event_got_ip_t *)event_data;
  ESP_LOGI(__func__, "ip_changed: %d", got_ip_event_data->ip_changed);

  // Start the mDNS service
  start_mdns_service();

  // Start the WebSocket server
  // start_websocket_server();
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
