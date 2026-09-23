#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_wifi.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#if !defined(CONFIG_HTTPD_WS_SUPPORT) || !CONFIG_HTTPD_WS_SUPPORT
#error "CONFIG_HTTPD_WS_SUPPORT must be defined and enabled"
#endif

/******************************************************************************************************************************
 *                             WebSocket Server
 ******************************************************************************************************************************/

/*
 * Structure holding server handle
 * and internal socket fd in order
 * to use out of request send
 */
struct async_resp_arg {
  httpd_handle_t hd;
  int fd;
};

/*
 * async send function, which we put into the httpd work queue
 */
static void ws_async_send(void *arg) {
  static const char *data = "Async data";
  struct async_resp_arg *resp_arg = arg;
  httpd_handle_t hd = resp_arg->hd;
  int fd = resp_arg->fd;
  httpd_ws_frame_t ws_pkt;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
  ws_pkt.payload = (uint8_t *)data;
  ws_pkt.len = strlen(data);
  ws_pkt.type = HTTPD_WS_TYPE_TEXT;

  httpd_ws_send_frame_async(hd, fd, &ws_pkt);
  free(resp_arg);
}

static void ws_ping_send(void *arg) {
  struct async_resp_arg *resp_arg = arg;
  httpd_handle_t hd = resp_arg->hd;
  int fd = resp_arg->fd;
  httpd_ws_frame_t ping_pkt;
  memset(&ping_pkt, 0, sizeof(httpd_ws_frame_t));
  ping_pkt.type = HTTPD_WS_TYPE_PING;
  httpd_ws_send_frame_async(hd, fd, &ping_pkt);
  free(resp_arg);
}

static esp_err_t trigger_async_send(httpd_handle_t handle, httpd_req_t *req) {
  struct async_resp_arg *resp_arg = malloc(sizeof(struct async_resp_arg));
  if (resp_arg == NULL) {
    return ESP_ERR_NO_MEM;
  }
  resp_arg->hd = req->handle;
  resp_arg->fd = httpd_req_to_sockfd(req);
  esp_err_t ret = httpd_queue_work(handle, ws_async_send, resp_arg);
  if (ret != ESP_OK) {
    free(resp_arg);
  }
  return ret;
}

static esp_err_t trigger_ping_send(httpd_handle_t handle, httpd_req_t *req) {
  struct async_resp_arg *resp_arg = malloc(sizeof(struct async_resp_arg));
  if (resp_arg == NULL) {
    return ESP_ERR_NO_MEM;
  }
  resp_arg->hd = req->handle;
  resp_arg->fd = httpd_req_to_sockfd(req);
  esp_err_t ret = httpd_queue_work(handle, ws_ping_send, resp_arg);
  if (ret != ESP_OK) {
    free(resp_arg);
  }
  return ret;
}

/*
 * This handler echos back the received ws data
 * and triggers an async send if certain message received
 */
static esp_err_t echo_handler(httpd_req_t *req) {
  httpd_ws_frame_t ws_pkt;
  uint8_t *buf = NULL;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
  ws_pkt.type = HTTPD_WS_TYPE_TEXT;
  /* Set max_len = 0 to get the frame len */
  esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
  if (ret != ESP_OK) {
    ESP_LOGE(__func__, "httpd_ws_recv_frame failed to get frame len with %d",
             ret);
    return ret;
  }
  ESP_LOGI(__func__, "frame len is %d", ws_pkt.len);
  if (ws_pkt.len) {
    /* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
    buf = calloc(1, ws_pkt.len + 1);
    if (buf == NULL) {
      ESP_LOGE(__func__, "Failed to calloc memory for buf");
      return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = buf;
    /* Set max_len = ws_pkt.len to get the frame payload */
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
      ESP_LOGE(__func__, "httpd_ws_recv_frame failed with %d", ret);
      free(buf);
      return ret;
    }
    ESP_LOGI(__func__, "Got packet with message: %s", ws_pkt.payload);
  }
  ESP_LOGI(__func__, "Packet type: %d", ws_pkt.type);
  if (ws_pkt.type == HTTPD_WS_TYPE_TEXT && ws_pkt.payload != NULL) {
    if (strncmp((char *)ws_pkt.payload, "Trigger async",
                strlen("Trigger async")) == 0) {
      free(buf);
      return trigger_async_send(req->handle, req);
    } else if (strncmp((char *)ws_pkt.payload, "Ping", strlen("Ping")) == 0) {
      free(buf);
      return trigger_ping_send(req->handle, req);
    }
  }

  ret = httpd_ws_send_frame(req, &ws_pkt);
  if (ret != ESP_OK) {
    ESP_LOGE(__func__, "httpd_ws_send_frame failed with %d", ret);
  }
  free(buf);
  return ret;
}

static const httpd_uri_t ws = {.uri = "/ws",
                               .method = HTTP_GET,
                               .handler = echo_handler,
                               .user_ctx = NULL,
                               .is_websocket = true};

static void start_websocket_server(void) {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();

  // Start the httpd server
  ESP_LOGI(__func__, "Starting server on port: '%d'", config.server_port);
  if (ESP_OK != httpd_start(&server, &config)) {
    ESP_LOGI(__func__, "Error starting server!");
    return;
  }

  // Registering the ws handler
  ESP_LOGI(__func__, "Registering URI handlers");
  httpd_register_uri_handler(server, &ws);
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
  ip_event_got_ip_t *got_ip_event_data = (ip_event_got_ip_t *)event_data;
  ESP_LOGI(__func__, "ip_changed: %d", got_ip_event_data->ip_changed);

  /// TODO: 通过MDNS发布服务

  // 启动 WebSocket 服务器
  start_websocket_server();
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
