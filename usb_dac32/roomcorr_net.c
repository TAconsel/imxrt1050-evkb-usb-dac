/*
 * See roomcorr_net.h.
 *
 * The HTTP layer is written straight onto lwIP's raw TCP API rather than using the
 * bundled httpd. The reason is the IR upload: a quarter-megabyte POST has to be streamed
 * into SDRAM as it arrives, and owning the recv callback makes that a few lines instead
 * of a fight with httpd's file abstraction. It also avoids generating an fsdata blob.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/sys.h"
#include "lwip/timeouts.h"
#include "lwip/dhcp.h"
#include "netif/ethernet.h"
#include "ethernetif.h"

#include "board.h"
#include "fsl_debug_console.h"
#include "fsl_enet.h"
#include "fsl_gpio.h"
#include "fsl_iomuxc.h"
#include "fsl_phy.h"
#include "fsl_phyksz8081.h"
#include "fsl_silicon_id.h"

#include "roomcorr.h"
#include "roomcorr_eq.h"
#include "roomcorr_filter.h"
#include "roomcorr_net.h"
#include "roomcorr_stream.h"
#include "roomcorr_web.h"

/* lwIP's bare-metal port owns SysTick for its 1 ms tick. */
extern void time_init(void);
extern void time_isr(void);

void SysTick_Handler(void)
{
    time_isr();
}

/* ------------------------------------------------------------- ethernet ---- */

#define RC_NET_ENET       ENET
#define RC_NET_PHY_ADDR   BOARD_ENET0_PHY_ADDRESS
#define RC_NET_CLOCK_FREQ CLOCK_GetFreq(kCLOCK_IpgClk)

static phy_handle_t             s_phyHandle;
static phy_ksz8081_resource_t   s_phyResource;
static struct netif             s_netif;
static char                     s_addrText[16] = "0.0.0.0";
static bool                     s_up;
static bool                     s_linkUp;
static uint32_t                 s_lastProbe;
static uint32_t                 s_lastReport;
static uint32_t                 s_linkUpAt;
static bool                     s_fellBack;

static status_t mdio_write(uint8_t phyAddr, uint8_t regAddr, uint16_t data)
{
    return ENET_MDIOWrite(RC_NET_ENET, phyAddr, regAddr, data);
}

static status_t mdio_read(uint8_t phyAddr, uint8_t regAddr, uint16_t *pData)
{
    return ENET_MDIORead(RC_NET_ENET, phyAddr, regAddr, pData);
}

static void net_init_pins(void)
{
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_04_ENET_RX_DATA00, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_05_ENET_RX_DATA01, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_06_ENET_RX_EN, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_07_ENET_TX_DATA00, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_08_ENET_TX_DATA01, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_09_ENET_TX_EN, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_10_ENET_REF_CLK, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_11_ENET_RX_ER, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_EMC_40_ENET_MDC, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_EMC_41_ENET_MDIO, 0U);

    IOMUXC_SetPinConfig(IOMUXC_GPIO_B1_04_ENET_RX_DATA00, 0xB0E9U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_B1_05_ENET_RX_DATA01, 0xB0E9U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_B1_06_ENET_RX_EN, 0xB0E9U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_B1_07_ENET_TX_DATA00, 0xB0E9U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_B1_08_ENET_TX_DATA01, 0xB0E9U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_B1_09_ENET_TX_EN, 0xB0E9U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_B1_10_ENET_REF_CLK, 0x31U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_B1_11_ENET_RX_ER, 0xB0E9U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_EMC_40_ENET_MDC, 0xB0E9U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_EMC_41_ENET_MDIO, 0xB829U);

    /*
     * GPIO_AD_B0_09 is the PHY reset -- and the same net as the D18 user LED, which is
     * why this board cannot have both. Ethernet wins here, so the pin is driven high and
     * left there; the correction state is reported over HTTP and the console instead.
     */
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_09_GPIO1_IO09, 0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B0_09_GPIO1_IO09, 0xB0A9U);

    /*
     * GPIO_AD_B0_10 is ENET_INT, which on the KSZ8081 is INTRP/NAND_TREE#. It is sampled
     * when reset is released: low puts the PHY into NAND-tree test mode, where MDIO still
     * answers normally but the analog side is dead, so autonegotiation never completes
     * and the link never comes up. Leaving the pin unconfigured lets it float into
     * exactly that state, so give it the same 100K pull-up the SDK example uses.
     */
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_10_GPIO1_IO10, 0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B0_10_GPIO1_IO10, 0xB0A9U);
}

/* ------------------------------------------------------------------ http ---- */

#define RC_HTTP_CONNS   (4U)
#define RC_HTTP_REQMAX  (768U)

typedef enum { kIdle = 0, kHeaders, kBody, kSending } http_state_t;

typedef struct
{
    struct tcp_pcb *pcb;
    http_state_t    state;
    char            req[RC_HTTP_REQMAX];
    uint32_t        reqLen;
    bool            isIrPost;      /* body goes to the filter staging buffer */
    uint32_t        bodyLen;       /* Content-Length */
    uint32_t        bodyGot;
    const char     *tx;            /* remaining response bytes */
    uint32_t        txLen;
    bool            txStatic;      /* tx points at flash, no copy needed */
    char            txBuf[1024];   /* generated responses live here */
} http_conn_t;

static http_conn_t *s_conns; /* ~7 KB, allocated in SDRAM */

static void http_flush(http_conn_t *c)
{
    while ((c->txLen > 0U) && (tcp_sndbuf(c->pcb) > 0U))
    {
        uint16_t n = (uint16_t)((c->txLen > tcp_sndbuf(c->pcb)) ? tcp_sndbuf(c->pcb) : c->txLen);
        const u8_t flags = c->txStatic ? 0U : TCP_WRITE_FLAG_COPY;

        if (tcp_write(c->pcb, c->tx, n, (u8_t)(flags | ((c->txLen > n) ? TCP_WRITE_FLAG_MORE : 0U))) != ERR_OK)
        {
            break;
        }
        c->tx    += n;
        c->txLen -= n;
    }
    (void)tcp_output(c->pcb);

    if (c->txLen == 0U)
    {
        tcp_close(c->pcb);
        c->pcb   = NULL;
        c->state = kIdle;
    }
}

static void http_respond(http_conn_t *c, const char *body, uint32_t len, const char *type,
                         bool isStatic)
{
    /* headers first, into txBuf, then the body either copied after them or streamed */
    const int n = snprintf(c->txBuf, sizeof(c->txBuf),
                           "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                           "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                           type, (unsigned)len);
    if (isStatic)
    {
        /* send the header now, then point at the flash body */
        (void)tcp_write(c->pcb, c->txBuf, (uint16_t)n, TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);
        c->tx       = body;
        c->txLen    = len;
        c->txStatic = true;
    }
    else
    {
        if ((uint32_t)n + len < sizeof(c->txBuf))
        {
            (void)memcpy(&c->txBuf[n], body, len);
            c->txLen = (uint32_t)n + len;
        }
        else
        {
            c->txLen = (uint32_t)n;
        }
        c->tx       = c->txBuf;
        c->txStatic = false;
    }
    c->state = kSending;
    http_flush(c);
}

static void http_json_status(http_conn_t *c)
{
    char j[640];
    int  n = snprintf(j, sizeof(j),
                      "{\"bypass\":%d,\"preamp\":%.1f,\"taps\":%u,\"srcRate\":%u,"
                      "\"filter\":\"%s\",\"cpu\":%u,\"underruns\":%u,\"clips\":%u,"
                      "\"blocks\":%u,\"ip\":\"%s\",\"eq\":[",
                      RC_GetBypass() ? 1 : 0, (double)RC_EQ_GetPreamp(),
                      (unsigned)RC_FILTER_Taps(), (unsigned)RC_FILTER_SourceRate(),
                      RC_FILTER_Describe(),
                      (unsigned)((RC_PeakMicros() * 100U) / ((RC_BLOCK * 1000000U) / RC_SAMPLE_RATE)),
                      (unsigned)RCS_Underruns(), (unsigned)RC_ClipCount(),
                      (unsigned)RC_BlockCount(), s_addrText);

    for (uint32_t b = 0U; b < RC_EQ_BANDS; b++)
    {
        n += snprintf(&j[n], sizeof(j) - (size_t)n, "%s%.1f", (b == 0U) ? "" : ",",
                      (double)RC_EQ_GetBand(b));
    }
    n += snprintf(&j[n], sizeof(j) - (size_t)n, "]}");
    http_respond(c, j, (uint32_t)n, "application/json", false);
}

/* tiny query-string helper: returns true and fills out if key=<number> is present */
static bool query_num(const char *q, const char *key, float *out)
{
    const size_t klen = strlen(key);
    const char  *p    = q;

    while ((p != NULL) && (*p != '\0'))
    {
        if ((strncmp(p, key, klen) == 0) && (p[klen] == '='))
        {
            *out = (float)atof(&p[klen + 1U]);
            return true;
        }
        p = strchr(p, '&');
        if (p != NULL)
        {
            p++;
        }
    }
    return false;
}

static void http_handle_set(http_conn_t *c, const char *query)
{
    float v;
    char  key[8];

    if (query_num(query, "bypass", &v))
    {
        RC_SetBypass(v != 0.0f);
    }
    if (query_num(query, "preamp", &v))
    {
        RC_EQ_SetPreamp(v);
    }
    for (uint32_t b = 0U; b < RC_EQ_BANDS; b++)
    {
        (void)snprintf(key, sizeof(key), "eq%u", (unsigned)b);
        if (query_num(query, key, &v))
        {
            RC_EQ_SetBand(b, v);
        }
    }
    http_json_status(c);
}

static void http_dispatch(http_conn_t *c)
{
    char *sp1 = strchr(c->req, ' ');
    char *sp2 = (sp1 != NULL) ? strchr(sp1 + 1, ' ') : NULL;

    if ((sp1 == NULL) || (sp2 == NULL))
    {
        http_respond(c, "bad request", 11U, "text/plain", false);
        return;
    }
    *sp2 = '\0';
    char *uri   = sp1 + 1;
    char *query = strchr(uri, '?');
    if (query != NULL)
    {
        *query = '\0';
        query++;
    }

    if (strcmp(uri, "/api/status") == 0)
    {
        http_json_status(c);
    }
    else if (strcmp(uri, "/api/set") == 0)
    {
        http_handle_set(c, (query != NULL) ? query : "");
    }
    else if ((strcmp(uri, "/") == 0) || (strcmp(uri, "/index.html") == 0))
    {
        http_respond(c, rc_web_page, rc_web_page_len, "text/html", true);
    }
    else
    {
        http_respond(c, "not found", 9U, "text/plain", false);
    }
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    http_conn_t *c = (http_conn_t *)arg;

    if ((p == NULL) || (err != ERR_OK))
    {
        if (p != NULL)
        {
            pbuf_free(p);
        }
        tcp_close(pcb);
        if (c != NULL)
        {
            c->pcb   = NULL;
            c->state = kIdle;
        }
        return ERR_OK;
    }

    tcp_recved(pcb, p->tot_len);

    for (struct pbuf *q = p; q != NULL; q = q->next)
    {
        const uint8_t *d = (const uint8_t *)q->payload;
        uint32_t       n = q->len;

        if (c->state == kBody)
        {
            const uint32_t room = RC_UPLOAD_MAX_BYTES - c->bodyGot;
            const uint32_t take = (n < room) ? n : room;
            (void)memcpy(&RC_FILTER_UploadBuffer()[c->bodyGot], d, take);
            c->bodyGot += take;
            continue;
        }

        /* still collecting the request head */
        const uint32_t room = RC_HTTP_REQMAX - 1U - c->reqLen;
        const uint32_t take = (n < room) ? n : room;
        (void)memcpy(&c->req[c->reqLen], d, take);
        c->reqLen += take;
        c->req[c->reqLen] = '\0';

        char *end = strstr(c->req, "\r\n\r\n");
        if (end != NULL)
        {
            const char *cl = strstr(c->req, "Content-Length:");
            c->bodyLen  = (cl != NULL) ? (uint32_t)atoi(cl + 15) : 0U;
            c->isIrPost = (strncmp(c->req, "POST /api/ir", 12) == 0);

            if (c->isIrPost && (c->bodyLen > 0U) && (c->bodyLen <= RC_UPLOAD_MAX_BYTES))
            {
                /* whatever of the body already arrived in this pbuf run */
                const uint32_t consumed = (uint32_t)(end + 4 - c->req);
                const uint32_t spill    = c->reqLen - consumed;
                (void)memcpy(RC_FILTER_UploadBuffer(), &c->req[consumed], spill);
                c->bodyGot = spill;
                c->state   = kBody;
                /* remaining bytes of this pbuf, if the head ended mid-buffer */
                if (take < n)
                {
                    const uint32_t extra = n - take;
                    (void)memcpy(&RC_FILTER_UploadBuffer()[c->bodyGot], &d[take], extra);
                    c->bodyGot += extra;
                }
            }
            else
            {
                http_dispatch(c);
                break;
            }
        }
    }
    pbuf_free(p);

    if ((c->state == kBody) && (c->bodyGot >= c->bodyLen))
    {
        const rc_filter_status_t st = RC_FILTER_LoadWav(c->bodyGot);
        char body[192];
        const int n = snprintf(body, sizeof(body), "{\"ok\":%d,\"msg\":\"%s\",\"filter\":\"%s\"}",
                               (st == kRC_FilterOk) ? 1 : 0, RC_FILTER_StatusText(st),
                               RC_FILTER_Describe());
        c->state = kSending;
        http_respond(c, body, (uint32_t)n, "application/json", false);
    }
    return ERR_OK;
}

static err_t http_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    http_conn_t *c = (http_conn_t *)arg;
    (void)pcb;
    (void)len;
    if ((c != NULL) && (c->state == kSending) && (c->pcb != NULL))
    {
        http_flush(c);
    }
    return ERR_OK;
}

static void http_err(void *arg, err_t err)
{
    http_conn_t *c = (http_conn_t *)arg;
    (void)err;
    if (c != NULL)
    {
        c->pcb   = NULL;
        c->state = kIdle;
    }
}

static err_t http_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if ((err != ERR_OK) || (pcb == NULL))
    {
        return ERR_VAL;
    }
    for (uint32_t i = 0U; i < RC_HTTP_CONNS; i++)
    {
        if (s_conns[i].state == kIdle)
        {
            http_conn_t *c = &s_conns[i];
            (void)memset(c, 0, sizeof(*c));
            c->pcb   = pcb;
            c->state = kHeaders;
            tcp_arg(pcb, c);
            tcp_recv(pcb, http_recv);
            tcp_sent(pcb, http_sent);
            tcp_err(pcb, http_err);
            return ERR_OK;
        }
    }
    tcp_abort(pcb);
    return ERR_ABRT;
}

/* ---------------------------------------------------------------- public ---- */

void RC_NET_Init(void)
{
    const clock_enet_pll_config_t pll = {.enableClkOutput = true, .enableClkOutput25M = false,
                                         .loopDivider = 1};
    static ethernetif_config_t cfg = {.phyHandle   = &s_phyHandle,
                               .phyAddr     = RC_NET_PHY_ADDR,
                               .phyOps      = &phyksz8081_ops,
                               .phyResource = &s_phyResource};

    s_conns = (http_conn_t *)RCS_SdramAlloc(RC_HTTP_CONNS * sizeof(http_conn_t));
    (void)memset(s_conns, 0, RC_HTTP_CONNS * sizeof(http_conn_t));

    CLOCK_InitEnetPll(&pll);
    net_init_pins();
    IOMUXC_EnableMode(IOMUXC_GPR, kIOMUXC_GPR_ENET1TxClkOutputDir, true);

    /*
     * The reset line has to be an actual output first. BOARD_ENET_PHY_RESET only writes
     * DR; without GDIR set the pin stays an input and the KSZ8081's RST# floats, which
     * looks exactly like "link never comes up".
     */
    const gpio_pin_config_t rstConfig = {kGPIO_DigitalOutput, 1, kGPIO_NoIntmode};
    const gpio_pin_config_t intConfig = {kGPIO_DigitalInput, 0, kGPIO_NoIntmode};
    GPIO_PinInit(BOARD_ENET_PHY_RESET_GPIO, BOARD_ENET_PHY_RESET_GPIO_PIN, &rstConfig);
    GPIO_PinInit(GPIO1, 10U, &intConfig); /* keep NAND_TREE# an input, pulled high */
    BOARD_ENET_PHY_RESET;

    (void)CLOCK_EnableClock(kCLOCK_Enet);
    ENET_SetSMI(RC_NET_ENET, RC_NET_CLOCK_FREQ, false);
    s_phyResource.read  = mdio_read;
    s_phyResource.write = mdio_write;

    (void)SILICONID_ConvertToMacAddr(&cfg.macAddress);
    cfg.srcClockHz = RC_NET_CLOCK_FREQ;

    time_init();
    lwip_init();

    /* read the PHY straight over MDIO before lwIP touches it: this separates "MDIO
     * dead" from "PHY alive but never negotiates", which look identical from lwIP */
    uint16_t id1 = 0U, id2 = 0U, bmcr = 0U, bmsr = 0U;
    (void)mdio_read(RC_NET_PHY_ADDR, 2U, &id1);
    (void)mdio_read(RC_NET_PHY_ADDR, 3U, &id2);
    (void)mdio_read(RC_NET_PHY_ADDR, 0U, &bmcr);
    (void)mdio_read(RC_NET_PHY_ADDR, 1U, &bmsr);
    PRINTF("net: phy addr %d id %04x:%04x bmcr %04x bmsr %04x\r\n",
           (int)RC_NET_PHY_ADDR, id1, id2, bmcr, bmsr);

    if (netif_add(&s_netif, NULL, NULL, NULL, &cfg, ethernetif0_init, ethernet_input) == NULL)
    {
        PRINTF("net: netif_add FAILED - ethernetif0_init rejected the PHY\r\n");
    }
    netif_set_default(&s_netif);
    netif_set_up(&s_netif);

    /*
     * Give autonegotiation the same blocking window the SDK examples use. Nothing is
     * streaming yet at this point, so a few seconds here costs nothing, and it settles
     * whether the link needs servicing more aggressively than a 500 ms poll provides.
     */
    for (uint32_t tries = 0U; tries < 3U; tries++)
    {
        if (ethernetif_wait_linkup(&s_netif, 4000) == ERR_OK)
        {
            PRINTF("net: link up after %u s\r\n", (unsigned)((tries * 4U) + 1U));
            break;
        }
        PRINTF("net: autonegotiation still pending...\r\n");
    }

    (void)dhcp_start(&s_netif);

    struct tcp_pcb *listen = tcp_new();
    if (listen != NULL)
    {
        (void)tcp_bind(listen, IP_ANY_TYPE, 80);
        listen = tcp_listen(listen);
        tcp_accept(listen, http_accept);
    }

    PRINTF("net: MAC %02x:%02x:%02x:%02x:%02x:%02x, waiting for DHCP...\r\n",
           cfg.macAddress[0], cfg.macAddress[1], cfg.macAddress[2],
           cfg.macAddress[3], cfg.macAddress[4], cfg.macAddress[5]);
}

void RC_NET_Task(void)
{
    ethernetif_input(&s_netif);
    sys_check_timeouts();

    /*
     * The PHY has to be polled for us to notice the cable. The blocking helper the SDK
     * examples use (ethernetif_wait_linkup) would stall the audio, so probe on a timer
     * instead. Without this, netif never goes link-up and DHCP never even transmits.
     */
    const uint32_t now = sys_now();
    if ((uint32_t)(now - s_lastProbe) >= 500U)
    {
        s_lastProbe = now;
        ethernetif_probe_link(&s_netif);

        const bool link = (netif_is_link_up(&s_netif) != 0);
        if (link != s_linkUp)
        {
            s_linkUp = link;
            PRINTF("net: link %s\r\n", link ? "up" : "down");
        }
    }

    /*
     * Fall back to a fixed address if nobody answers DHCP. A board plugged straight into
     * a PC has no DHCP server at all, and waiting forever in that case is useless -- give
     * it a link-local-ish address so the web UI is still reachable once the PC end is
     * configured on the same subnet.
     */
    if (s_linkUp && (s_linkUpAt == 0U))
    {
        s_linkUpAt = (now != 0U) ? now : 1U;
    }
    if (!s_up && !s_fellBack && (s_linkUpAt != 0U) && ((uint32_t)(now - s_linkUpAt) >= 15000U))
    {
        ip4_addr_t ip, mask, gw;

        s_fellBack = true;
        (void)dhcp_stop(&s_netif);
        IP4_ADDR(&ip, 192, 168, 77, 2);
        IP4_ADDR(&mask, 255, 255, 255, 0);
        IP4_ADDR(&gw, 192, 168, 77, 1);
        netif_set_addr(&s_netif, &ip, &mask, &gw);
        PRINTF("net: no DHCP offer in 15 s, falling back to a static address\r\n");
    }

    if (!s_up && ((uint32_t)(now - s_lastReport) >= 4000U))
    {
        struct dhcp *d = netif_dhcp_data(&s_netif);
        s_lastReport   = now;

        /* kick autonegotiation again: BMCR bit 12 = AN enable, bit 9 = AN restart */
        if (!netif_is_link_up(&s_netif))
        {
            (void)mdio_write(RC_NET_PHY_ADDR, 0U, 0x1200U);
        }
        uint16_t bmsr = 0U, ctl1 = 0U;
        (void)mdio_read(RC_NET_PHY_ADDR, 1U, &bmsr);
        (void)mdio_read(RC_NET_PHY_ADDR, 0x1EU, &ctl1); /* KSZ8081 PHY Control 1 */
        PRINTF("net: waiting - link %s, dhcp %d, bmsr %04x (link=%d anegDone=%d), ctl1 %04x\r\n",
               netif_is_link_up(&s_netif) ? "UP" : "DOWN", (d != NULL) ? (int)d->state : -1,
               bmsr, (bmsr >> 2) & 1U, (bmsr >> 5) & 1U, ctl1);
    }

    if (!ip4_addr_isany_val(*netif_ip4_addr(&s_netif)))
    {
        const char *now = ip4addr_ntoa(netif_ip4_addr(&s_netif));
        if (strcmp(now, s_addrText) != 0)
        {
            (void)snprintf(s_addrText, sizeof(s_addrText), "%s", now);
            s_up = true;
            PRINTF("net: http://%s/  (netmask %s, gw %s)\r\n", s_addrText,
                   ip4addr_ntoa(netif_ip4_netmask(&s_netif)),
                   ip4addr_ntoa(netif_ip4_gw(&s_netif)));
        }
    }
}

bool        RC_NET_IsUp(void)        { return s_up; }
const char *RC_NET_AddressText(void) { return s_addrText; }
