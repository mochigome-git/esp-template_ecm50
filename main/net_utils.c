#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include "esp_task_wdt.h"
#include "lwip/ip4_addr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_eth_phy_w5500.h"
#include "esp_eth_mac_w5500.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "lwip/dns.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "net_utils.h"
#include "config.h"

static const char *TAG = "net";

/* Event bits */
#define LAN_GOT_IP_BIT BIT0
#define WIFI_GOT_IP_BIT BIT1
#define WIFI_FAIL_BIT BIT2

static EventGroupHandle_t s_net_eg = NULL;
static esp_netif_t *s_netif_eth = NULL;
static esp_netif_t *s_netif_wifi = NULL;
static esp_eth_handle_t s_eth_hdl = NULL; /* kept for ioctl (MAC set) */
static net_iface_t s_active = NET_IFACE_NONE;

/* ── Event handlers ─────────────────────────────────────── */

static void on_eth_event(void *arg, esp_event_base_t base,
                         int32_t id, void *data)
{
    if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED)
    {
        ESP_LOGI(TAG, "Ethernet link up");
    }
    else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED)
    {
        ESP_LOGW(TAG, "Ethernet link DOWN");
    }
    else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP)
    {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "LAN IP: " IPSTR, IP2STR(&e->ip_info.ip));

        esp_netif_dns_info_t dns;
        esp_netif_get_dns_info(s_netif_eth, ESP_NETIF_DNS_MAIN, &dns);
        ESP_LOGI(TAG, "DHCP DNS: " IPSTR, IP2STR(&dns.ip.u_addr.ip4));

        s_active = NET_IFACE_LAN;
        xEventGroupSetBits(s_net_eg, LAN_GOT_IP_BIT);
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(TAG, "WiFi disconnected");
        xEventGroupSetBits(s_net_eg, WIFI_FAIL_BIT);
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_active = NET_IFACE_WIFI;
        xEventGroupSetBits(s_net_eg, WIFI_GOT_IP_BIT);
    }
}

/* ── LAN (W5500) ────────────────────────────────────────── */

static esp_err_t apply_lan_static_ip(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_dhcpc_stop(s_netif_eth),
                        TAG, "DHCPC stop failed");

    ip4_addr_t ip, gw, mask, dns;
    ip4addr_aton(LAN_STATIC_IP, &ip);
    ip4addr_aton(LAN_STATIC_GATEWAY, &gw);
    ip4addr_aton(LAN_STATIC_NETMASK, &mask);
    ip4addr_aton(LAN_STATIC_DNS, &dns);

    esp_netif_ip_info_t ip_info = {0};
    ip_info.ip.addr = ip.addr;
    ip_info.gw.addr = gw.addr;
    ip_info.netmask.addr = mask.addr;
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_netif_eth, &ip_info),
                        TAG, "Set static IP failed");

    esp_netif_dns_info_t dns_info = {0};
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    dns_info.ip.u_addr.ip4.addr = dns.addr;
    ESP_RETURN_ON_ERROR(esp_netif_set_dns_info(s_netif_eth, ESP_NETIF_DNS_MAIN, &dns_info),
                        TAG, "Set static DNS failed");

    ESP_LOGI(TAG, "[LAN] Static IP: %s  GW: %s  Mask: %s  DNS: %s",
             LAN_STATIC_IP, LAN_STATIC_GATEWAY, LAN_STATIC_NETMASK, LAN_STATIC_DNS);
    return ESP_OK;
}

static esp_err_t start_lan(void)
{
    ESP_LOGI(TAG, "[LAN] Configuring W5500...");

    /* ── netif ───────────────────────────────────────────── */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif_eth = esp_netif_new(&netif_cfg);
    if (!s_netif_eth)
        return ESP_FAIL;

    /* ── SPI bus ─────────────────────────────────────────── */
    spi_bus_config_t buscfg = {
        .mosi_io_num = W5500_PIN_MOSI,
        .miso_io_num = W5500_PIN_MISO,
        .sclk_io_num = W5500_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(W5500_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    /* ── W5500 SPI device ────────────────────────────────── */
    spi_device_interface_config_t devcfg = {
        .command_bits = 16,
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = W5500_SPI_CLOCK,
        .spics_io_num = W5500_PIN_CS,
        .queue_size = 20,
    };

    eth_w5500_config_t w5500_cfg = {
        .int_gpio_num = W5500_PIN_INT,
        .poll_period_ms = 0, /* interrupt mode, not polling */
        .spi_host_id = W5500_SPI_HOST,
        .spi_devcfg = &devcfg,
    };

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_cfg, &mac_cfg);
    if (!mac)
        return ESP_FAIL;

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num = W5500_PIN_RST;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
    if (!phy)
        return ESP_FAIL;

    gpio_install_isr_service(0);

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_cfg, &s_eth_hdl),
                        TAG, "ETH driver install failed");

    /* ── Set MAC on the W5500 chip via the ETH driver ───────
     *
     * Python: lan.config(mac=mac)   sets MAC on the hardware chip.
     *
     * BUG in old C code: esp_netif_set_mac() only sets the MAC on the
     * lwIP netif layer — the W5500 chip itself kept its default/random
     * MAC, so ARP replies from the router went to the wrong address and
     * TCP/UDP packets above the link layer were silently dropped.
     *
     * Fix: use esp_eth_ioctl(ETH_CMD_S_MAC_ADDR) which writes the MAC
     * directly into the W5500 hardware registers, then also tell the
     * netif layer to match.
     * ─────────────────────────────────────────────────────── */
    uint8_t mac_addr[6];
    memcpy(mac_addr, W5500_MAC, 6);

    ESP_RETURN_ON_ERROR(
        esp_eth_ioctl(s_eth_hdl, ETH_CMD_S_MAC_ADDR, mac_addr),
        TAG, "Set W5500 MAC failed");

    ESP_LOGI(TAG, "[LAN] MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2],
             mac_addr[3], mac_addr[4], mac_addr[5]);

    /* ── Attach netif ────────────────────────────────────── */
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth_hdl);
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif_eth, glue),
                        TAG, "ETH netif attach failed");

    /* ── Static IP (optional) ────────────────────────────── */
    if (LAN_USE_STATIC_IP)
    {
        ESP_RETURN_ON_ERROR(apply_lan_static_ip(), TAG, "Static IP config failed");
    }

    /* ── Register events ─────────────────────────────────── */
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL),
        TAG, "ETH event register failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_eth_event, NULL),
        TAG, "IP event register failed");

    /* ── DO NOT call esp_netif_dhcpc_start() here ───────────
     *
     * BUG in old C code: ESP_NETIF_DEFAULT_ETH() already starts DHCP
     * automatically. Calling esp_netif_dhcpc_start() a second time
     * caused a state conflict that corrupted the DNS entries DHCP
     * provides — so DNS from DHCP was set but then overwritten/lost,
     * leaving the chip with no working resolver.
     *
     * DHCP (including DNS option 6) starts automatically when
     * esp_eth_start() fires the link-up event. Nothing extra needed.
     * ─────────────────────────────────────────────────────── */

    return esp_eth_start(s_eth_hdl);
}

/* ── WiFi ───────────────────────────────────────────────── */

static esp_err_t start_wifi(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "[WiFi] Connecting to %s...", ssid);

    if (!s_netif_wifi)
        s_netif_wifi = esp_netif_create_default_wifi_sta();

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&icfg), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL),
        TAG, "wifi event register");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL),
        TAG, "ip event register");

    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid, ssid, sizeof(wcfg.sta.ssid) - 1);
    strncpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password) - 1);
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wcfg.sta.pmf_cfg.capable = true;
    wcfg.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wcfg), TAG, "wifi cfg");
    return esp_wifi_start();
}

/* ══════════════════════════════════════════════════════════
 *  Internet reachability test  (WDT-safe, non-blocking)
 *
 *  Step 1 — DNS resolve google.com  (proves UDP/DNS works)
 *  Step 2 — Non-blocking TCP to 8.8.8.8:80 with 200 ms select()
 *            slices so WDT is fed between polls.
 *
 *  Returns true if internet is reachable.
 * ══════════════════════════════════════════════════════════ */
bool test_internet(void)
{
    /* ── Step 1: DNS ─────────────────────────────────────── */
    ESP_LOGI(TAG, "[Test] Resolving DNS...");
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    int gai = getaddrinfo("google.com", "80", &hints, &res);
    if (gai != 0 || !res)
    {
        ESP_LOGE(TAG, "[Test] DNS failed (%d) — no internet", gai);
        return false;
    }
    char ip_str[16];
    struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));
    ESP_LOGI(TAG, "[Test] DNS OK: google.com -> %s", ip_str);
    freeaddrinfo(res);

    /* ── Step 2: non-blocking TCP to 8.8.8.8:80 ─────────── */
    ESP_LOGI(TAG, "[Test] TCP connect to 8.8.8.8:80 (non-blocking)...");

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "[Test] socket() failed: errno %d", errno);
        return false;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(80),
    };
    inet_pton(AF_INET, "8.8.8.8", &dest.sin_addr);

    int ret = connect(sock, (struct sockaddr *)&dest, sizeof(dest));
    if (ret < 0 && errno != EINPROGRESS)
    {
        ESP_LOGE(TAG, "[Test] connect() immediate error: errno %d", errno);
        close(sock);
        return false;
    }

    bool ok = false;
    for (int i = 0; i < 25; i++)
    {
        esp_task_wdt_reset();

        fd_set wfds, efds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        FD_ZERO(&efds);
        FD_SET(sock, &efds);
        struct timeval slice = {.tv_sec = 0, .tv_usec = 200000};

        int sel = select(sock + 1, NULL, &wfds, &efds, &slice);
        if (sel < 0)
        {
            ESP_LOGE(TAG, "[Test] select() error: errno %d", errno);
            break;
        }
        if (sel == 0)
            continue;

        int sock_err = 0;
        socklen_t len = sizeof(sock_err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &sock_err, &len);
        if (sock_err == 0)
        {
            ESP_LOGI(TAG, "[Test] TCP to 8.8.8.8:80 OK — internet reachable");
            ok = true;
        }
        else
        {
            ESP_LOGE(TAG, "[Test] TCP to 8.8.8.8:80 FAILED (sock_err=%d)", sock_err);
        }
        break;
    }

    if (!ok)
        ESP_LOGW(TAG, "[Test] TCP connect timed out after 5 s");

    close(sock);
    return ok;
}

/* ── Public ─────────────────────────────────────────────── */

void hard_reset(const char *reason)
{
    ESP_LOGE(TAG, "RESET — %s", reason);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static bool wait_bits_wdt_safe(EventBits_t bits_to_wait, uint32_t total_ms)
{
    const uint32_t slice_ms = 500;
    uint32_t waited = 0;
    while (waited < total_ms)
    {
        esp_task_wdt_reset();
        EventBits_t bits = xEventGroupWaitBits(s_net_eg, bits_to_wait,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(slice_ms));
        if (bits & bits_to_wait)
            return true;
        waited += slice_ms;
    }
    return false;
}

void net_connect(const char *mode, const char *wifi_ssid, const char *wifi_pass)
{
    s_net_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    bool try_lan = (strcmp(mode, "lan") == 0 || strcmp(mode, "auto") == 0);
    bool try_wifi = (strcmp(mode, "wifi") == 0 || strcmp(mode, "auto") == 0);

    if (try_lan)
    {
        ESP_LOGI(TAG, "[NET] Trying LAN...");
        esp_task_wdt_reset();
        if (start_lan() == ESP_OK)
        {
            /* Managed switches (STP listening+learning) can take
             * 30-50s before the port forwards traffic. Poll in
             * slices so the WDT gets fed the whole time. */
            if (wait_bits_wdt_safe(LAN_GOT_IP_BIT, 45000))
            {
                ESP_LOGI(TAG, "[NET] LAN connected");
                return;
            }
            ESP_LOGW(TAG, "[NET] LAN timeout (45s) — check switch STP/VLAN config");
        }
    }

    if (try_wifi)
    {
        ESP_LOGI(TAG, "[NET] Trying WiFi...");
        esp_task_wdt_reset();
        if (start_wifi(wifi_ssid, wifi_pass) == ESP_OK)
        {
            if (wait_bits_wdt_safe(WIFI_GOT_IP_BIT | WIFI_FAIL_BIT, 20000))
            {
                ESP_LOGI(TAG, "[NET] WiFi connected");
                return;
            }
            ESP_LOGW(TAG, "[NET] WiFi failed/timeout");
        }
    }

    ESP_LOGE(TAG, "[NET] No network — rebooting in 2s");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

net_iface_t net_check(int *rssi_out)
{
    if (rssi_out)
        *rssi_out = 0;

    if (s_netif_eth && esp_netif_is_netif_up(s_netif_eth))
    {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(s_netif_eth, &info) == ESP_OK && info.ip.addr != 0)
        {
            ESP_LOGI(TAG, "[NET] LAN OK — IP: " IPSTR, IP2STR(&info.ip));
            return NET_IFACE_LAN;
        }
    }

    if (s_netif_wifi && esp_netif_is_netif_up(s_netif_wifi))
    {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(s_netif_wifi, &info) == ESP_OK && info.ip.addr != 0)
        {
            if (rssi_out)
            {
                wifi_ap_record_t ap;
                if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
                    *rssi_out = ap.rssi;
            }
            ESP_LOGI(TAG, "[NET] WiFi OK — IP: " IPSTR, IP2STR(&info.ip));
            return NET_IFACE_WIFI;
        }
    }

    ESP_LOGW(TAG, "[NET] No active interface");
    return NET_IFACE_NONE;
}
