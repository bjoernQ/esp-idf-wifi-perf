/*
 * Wi-Fi bench client.
 *
 * Same sequence as esp-hal embassy-wifi-bench: 15 second TCP download
 * (port 4321), upload (port 4322), then both at once (port 4323).
 * Each direction uses a 1024-byte buffer. The socket timeout is 10 seconds.
 * TCP send and receive buffers are 16 KiB (see sdkconfig.defaults).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define TEST_DURATION_S      15
#define SOCKET_TIMEOUT_MS    10000
#define RX_BUFFER_SIZE       16384
#define TX_BUFFER_SIZE       16384
#define IO_BUFFER_SIZE       1024
#define DOWNLOAD_PORT        4321
#define UPLOAD_PORT          4322
#define UPLOAD_DOWNLOAD_PORT 4323
#define WIFI_CONNECTED_BIT   BIT0
#define BIDI_TASK_STACK      5120

static EventGroupHandle_t s_wifi_event_group;
static TaskHandle_t s_reconnect_task;

/* Ends a test by shutting the socket down, which wakes any blocked send/recv. */
typedef struct {
    esp_timer_handle_t timer;
    int sock;
    volatile bool expired;
    volatile bool cb_done;
} deadline_t;

static deadline_t s_deadline;

typedef struct {
    int sock;
    volatile bool tx_done;
    volatile bool rx_done;
    volatile size_t total;
} bidi_ctx_t;

static void reconnect_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t value = 0;
        xTaskNotifyWait(0, ULONG_MAX, &value, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(5000));
        xTaskNotifyWait(0, ULONG_MAX, &value, 0);

        printf("About to connect...\n");
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            printf("Failed to connect to wifi: %s\n", esp_err_to_name(err));
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        printf("About to connect...\n");
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            printf("Failed to connect to wifi: %s\n", esp_err_to_name(err));
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *connected = event_data;
        printf("Wifi connected to %.*s\n", (int)connected->ssid_len, (const char *)connected->ssid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc = event_data;
        printf("Disconnected: reason %d\n", disc->reason);
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        xTaskNotify(s_reconnect_task, 1, eSetValueWithOverwrite);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        printf("Got IP: " IPSTR "\n", IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(s_wifi_event_group != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_BENCH_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_BENCH_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode =
        CONFIG_BENCH_WIFI_PASSWORD[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    printf("Wifi configured and started!\n");

    printf("start connection task\n");
    BaseType_t created = xTaskCreate(reconnect_task, "wifi_reconnect", 3072, NULL, 5, &s_reconnect_task);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

static void set_socket_timeout(int sock, int timeout_ms)
{
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int sock_wait(int sock, bool for_write, int timeout_ms)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    fd_set *read_fds = for_write ? NULL : &fds;
    fd_set *write_fds = for_write ? &fds : NULL;
    return select(sock + 1, read_fds, write_fds, NULL, &tv);
}

static void deadline_cb(void *arg)
{
    (void)arg;
    s_deadline.expired = true;
    shutdown(s_deadline.sock, SHUT_RDWR);
    s_deadline.cb_done = true;
}

static void deadline_init(void)
{
    const esp_timer_create_args_t args = {
        .callback = deadline_cb,
        .name = "bench_deadline",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_deadline.timer));
}

static void deadline_start(int sock)
{
    s_deadline.sock = sock;
    s_deadline.expired = false;
    s_deadline.cb_done = false;
    ESP_ERROR_CHECK(esp_timer_start_once(s_deadline.timer, (uint64_t)TEST_DURATION_S * 1000000));
}

/* Must return before the socket is closed: the callback may already be running. */
static void deadline_stop(void)
{
    if (esp_timer_stop(s_deadline.timer) != ESP_OK) {
        while (!s_deadline.cb_done) {
            vTaskDelay(1);
        }
    }
}

static void close_socket(int sock)
{
    shutdown(sock, SHUT_RDWR);
    close(sock);
}

static int connect_server(uint16_t port)
{
    printf("connecting to %s:%u...\n", CONFIG_BENCH_HOST_IP, (unsigned)port);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        printf("connect error: %s\n", strerror(errno));
        return -1;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) != 0) {
        printf("connect error: %s\n", strerror(errno));
        close_socket(sock);
        return -1;
    }

    struct sockaddr_in dest = { 0 };
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (inet_pton(AF_INET, CONFIG_BENCH_HOST_IP, &dest.sin_addr) != 1) {
        printf("connect error: invalid address\n");
        close_socket(sock);
        return -1;
    }

    int rc = connect(sock, (struct sockaddr *)&dest, sizeof(dest));
    if (rc < 0 && errno != EINPROGRESS) {
        printf("connect error: %s\n", strerror(errno));
        close_socket(sock);
        return -1;
    }
    if (rc < 0) {
        rc = sock_wait(sock, true, SOCKET_TIMEOUT_MS);
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (rc > 0) {
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
        }
        if (rc <= 0 || so_error != 0) {
            if (rc == 0) {
                printf("connect error: timeout\n");
            } else if (so_error != 0) {
                printf("connect error: %s\n", strerror(so_error));
            } else {
                printf("connect error: %s\n", strerror(errno));
            }
            close_socket(sock);
            return -1;
        }
    }

    if (fcntl(sock, F_SETFL, flags) != 0) {
        printf("connect error: %s\n", strerror(errno));
        close_socket(sock);
        return -1;
    }

    set_socket_timeout(sock, SOCKET_TIMEOUT_MS);
    printf("connected, testing...\n");
    return sock;
}

static unsigned kb_per_s(size_t total)
{
    return (unsigned)((total + 512U) / 1024U / TEST_DURATION_S);
}

/* Blocking recv until EOF, error, or the deadline timer shuts the socket down. */
static size_t download_loop(int sock)
{
    uint8_t buf[IO_BUFFER_SIZE];
    size_t total = 0;

    for (;;) {
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        if (s_deadline.expired) {
            break;
        }
        if (n == 0) {
            printf("read EOF\n");
        } else {
            printf("read error: %s\n", strerror(errno));
        }
        break;
    }
    return total;
}

static size_t upload_loop(int sock)
{
    uint8_t buf[IO_BUFFER_SIZE] = { 0 };
    size_t total = 0;

    for (;;) {
        int n = send(sock, buf, sizeof(buf), 0);
        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        if (s_deadline.expired) {
            break;
        }
        if (n == 0) {
            /* Split so "??!" is not a C trigraph. */
            printf("write zero?!?" "?!" "?!\n");
        } else {
            printf("write error: %s\n", strerror(errno));
        }
        break;
    }
    return total;
}

static size_t test_download(void)
{
    printf("Testing download...\n");

    int sock = connect_server(DOWNLOAD_PORT);
    if (sock < 0) {
        return 0;
    }

    deadline_start(sock);
    size_t total = download_loop(sock);
    deadline_stop();

    close_socket(sock);
    unsigned kbps = kb_per_s(total);
    printf("download: %u kB/s\n", kbps);
    return kbps;
}

static size_t test_upload(void)
{
    printf("Testing upload...\n");

    int sock = connect_server(UPLOAD_PORT);
    if (sock < 0) {
        return 0;
    }

    deadline_start(sock);
    size_t total = upload_loop(sock);
    deadline_stop();

    close_socket(sock);
    unsigned kbps = kb_per_s(total);
    printf("upload: %u kB/s\n", kbps);
    return kbps;
}

static void upload_task(void *arg)
{
    bidi_ctx_t *ctx = arg;
    (void)upload_loop(ctx->sock);
    ctx->tx_done = true;
    vTaskDelete(NULL);
}

static void download_task(void *arg)
{
    bidi_ctx_t *ctx = arg;
    ctx->total = download_loop(ctx->sock);
    ctx->rx_done = true;
    vTaskDelete(NULL);
}

static size_t test_upload_download(void)
{
    printf("Testing upload+download...\n");

    int sock = connect_server(UPLOAD_DOWNLOAD_PORT);
    if (sock < 0) {
        return 0;
    }

    bidi_ctx_t ctx = {
        .sock = sock,
    };
    deadline_start(sock);
    BaseType_t tx_ok = xTaskCreate(upload_task, "bench_tx", BIDI_TASK_STACK, &ctx, 1, NULL);
    BaseType_t rx_ok = xTaskCreate(download_task, "bench_rx", BIDI_TASK_STACK, &ctx, 1, NULL);
    if (tx_ok != pdPASS) {
        ctx.tx_done = true;
    }
    if (rx_ok != pdPASS) {
        ctx.rx_done = true;
    }
    if (tx_ok != pdPASS || rx_ok != pdPASS) {
        printf("write error: failed to start transfer\n");
        shutdown(sock, SHUT_RDWR);
    }

    while (!ctx.tx_done || !ctx.rx_done) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    deadline_stop();

    close_socket(sock);
    unsigned kbps = kb_per_s(ctx.total);
    printf("upload+download: %u kB/s\n", kbps);
    return kbps;
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    struct in_addr host;
    if (inet_pton(AF_INET, CONFIG_BENCH_HOST_IP, &host) != 1) {
        printf("Invalid HOST_IP address\n");
        return;
    }

    deadline_init();

    printf("Starting wifi\n");
    wifi_init();
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    _Static_assert(CONFIG_LWIP_TCP_WND_DEFAULT == RX_BUFFER_SIZE, "TCP window must match embassy-wifi-bench");
    _Static_assert(CONFIG_LWIP_TCP_SND_BUF_DEFAULT == TX_BUFFER_SIZE, "TCP send buffer must match embassy-wifi-bench");

    for (;;) {
        (void)test_download();
        (void)test_upload();
        (void)test_upload_download();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
