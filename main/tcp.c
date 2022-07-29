#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "lwip/sockets.h"
#include "esp_netif_types.h"

#include "message.h"
#include "tcp.h"

#define TCP_PORT (8080)
#define MAX_LATENCY (100 / portTICK_PERIOD_MS)
#define RXBUF_SIZE (256 * 1024)

static const char *TAG = "tcp";

static char *rxbuf = NULL;
static size_t rx_len;

static void tcp(void *arg);
static void parse(const char *buf, size_t len);

void create_tcp_task(void) { xTaskCreate(tcp, "tcp", 8192, NULL, 15, NULL); }

/*
 * states: NO_IP, NO_SOCK, SOCK_OK
 *
 * init state: NO_IP
 *
 *          NET_IP      CONNECT     DISCONNECT      NET_IP_LOST
 * NO_IP    NO_SOCK     n/a         n/a             ?
 * NO_SOCK  ?           SOCK_OK     n/a             NO_IP
 * SOCK_OK  ?           ?           NO_SOCK         NO_IP
 */
static void tcp(void *arg) {
  msg_t msg;
  esp_netif_ip_info_t *ip_info = NULL;
  rxbuf = (char *)malloc(RXBUF_SIZE);
  int sock = -1;

no_ip:
  while (1) {
    recv_msg(TCP_QUEUE, &msg, true); // blocking
    if (msg.type == NETIF) {
      if (msg.data) {
        ip_info = (esp_netif_ip_info_t *)msg.data;
        goto no_sock;
      }
    } else {
      if (msg.data)
        free(msg.data);
    }
  }

no_sock:
  while (1) {
    if (recv_msg(TCP_QUEUE, &msg, false)) {
      if (msg.type == NETIF) {
        free(ip_info);
        ip_info = NULL;
        if (msg.data) {
          ip_info = (esp_netif_ip_info_t *)msg.data;
        } else {
          goto no_ip;
        }
      } else {
        if (msg.data) // drop message if no sock available
          free(msg.data);
      }
    }

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = ip_info->gw.addr;
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(TCP_PORT);
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
      ESP_LOGI(TAG, "failed to create socket");
      goto err_closed;
    }

    int flags = fcntl(sock, F_GETFL);
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
      ESP_LOGI(TAG, "failed to set socket non blocking (%d)", errno);
      goto err_close;
    }

    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
      if (errno == EINPROGRESS) {
        ESP_LOGI(TAG, "sock connection in progress");

        fd_set fdset;
        FD_ZERO(&fdset);
        FD_SET(sock, &fdset);

        int ret = select(sock + 1, NULL, &fdset, NULL, NULL);
        if (ret < 0) {
          ESP_LOGI(TAG, "failed to wait socket to be writable");
          goto err_close;
        } else if (ret == 0) {
          ESP_LOGI(TAG, "connection timeout");
          goto err_close;
        } else {
          int sockerr;
          socklen_t slen = (socklen_t)sizeof(int);
          if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (void *)(&sockerr),
                         &slen) < 0) {
            ESP_LOGI(TAG, "error getsockopt()");
            goto err_close;
          }
          if (sockerr) {
            ESP_LOGI(TAG, "connection error");
            goto err_close;
          }
        }
      } else {
        ESP_LOGI(TAG, "failed to connect");
        goto err_close;
      }
    }
    ESP_LOGI(TAG, "socket connected");
    goto sock_ok;

  err_close:
    close(sock);
  err_closed:
    vTaskDelay(MAX_LATENCY);
  }

sock_ok:
  msg.type = TCP_CONNECT;
  msg.data = NULL;
  send_msg(MAIN_QUEUE, &msg);

  rx_len = 0;
  while (1) {
    if (recv_msg(TCP_QUEUE, &msg, false)) {
      if (msg.type == NETIF) {
        if (msg.data == NULL) {

        } else {
        }
      } else if (msg.type == TCP_SEND) {
      }
    }

    int len = recv(sock, &rxbuf[rx_len], RXBUF_SIZE - rx_len, 0);
    if (len < 0) {
      if (errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK) {
        vTaskDelay(MAX_LATENCY);
        continue;
      }

      if (errno == ENOTCONN) {
        ESP_LOGI(TAG, "sock already closed");
      } else {
        close(sock);
      }

      sock = -1;
      vTaskDelay(MAX_LATENCY);
      goto no_sock;
    }

    size_t i = rx_len;
    int lf_idx = -1;
    rx_len += len;
    for (; i < rx_len; i++) {
      if (rxbuf[i] == '\n') {
        lf_idx = i;
        break;
      }
    }

    if (lf_idx != -1) {
      rxbuf[lf_idx] = '\0';
      parse(rxbuf, lf_idx);
      memmove(rxbuf, &rxbuf[lf_idx + 1], rx_len - lf_idx - 1);
    }
  }
}

static void parse(const char *buf, size_t len) { 
  ESP_LOGI(TAG, "%s", buf); 
}
