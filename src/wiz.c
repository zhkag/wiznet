/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-09-26     chenyong     first version
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wiz.h>
#include <wiz_socket.h>

#include <W5500/w5500.h>
#include <DNS/dns.h>
#ifdef WIZ_USING_DHCP
#include <DHCP/dhcp.h>
#endif

#if !defined(WIZ_SPI_DEVICE) || !defined(WIZ_RST_PIN) || !defined(WIZ_IRQ_PIN)
#error "please config SPI device name, reset pin and irq pin in menuconfig."
#endif

#define DBG_ENABLE
#define DBG_SECTION_NAME               "wiz"
#ifdef WIZ_DEBUG
#define DBG_LEVEL                      DBG_LOG
#else
#define DBG_LEVEL                      DBG_INFO
#endif /* WIZ_DEBUG */
#define DBG_COLOR
#include <rtdbg.h>

#define IMR_SENDOK                     0x10
#define IMR_TIMEOUT                    0x08
#define IMR_RECV                       0x04
#define IMR_DISCON                     0x02
#define IMR_CON                        0x01
#define WIZ_DEFAULT_MAC                "44:39:c4:7f:e0:59"

extern struct rt_spi_device *wiz_device;
extern int wiz_device_init(const char *spi_dev_name, rt_base_t rst_pin, rt_base_t isr_pin);

rt_bool_t wiz_init_status = RT_FALSE;
static wiz_NetInfo wiz_net_info;

static void _delay_us(uint32_t us)
{
    volatile uint32_t len;
    for (; us > 0; us --)
    {
        for (len = 0; len < 20; len++);
    }
}

static void spi_write_byte(uint8_t data)
{
    struct rt_spi_message spi_msg;

    rt_memset(&spi_msg, 0x00, sizeof(spi_msg));

    spi_msg.send_buf = &data;
    spi_msg.length = 1;

    rt_spi_transfer_message(wiz_device, &spi_msg);
}

static uint8_t spi_read_byte(void)
{
    struct rt_spi_message spi_msg;
    uint8_t data;

    rt_memset(&spi_msg, 0x00, sizeof(spi_msg));

    spi_msg.recv_buf = &data;
    spi_msg.length = 1;

    rt_spi_transfer_message(wiz_device, &spi_msg);

    return data;
}

static void spi_cris_enter(void)
{
    rt_enter_critical();
}

static void spi_cris_exit(void)
{
    rt_exit_critical();
}

static void spi_cs_select(void)
{
    rt_spi_take(wiz_device);
}

static void spi_cs_deselect(void)
{
    rt_spi_release(wiz_device);
}

/* register TCP communication related callback function */
static int wiz_callback_register(void)
{
    /* register critical section callback function */
    reg_wizchip_cris_cbfunc(spi_cris_enter, spi_cris_exit);

#if (_WIZCHIP_IO_MODE_ == _WIZCHIP_IO_MODE_SPI_VDM_) || (_WIZCHIP_IO_MODE_ == _WIZCHIP_IO_MODE_SPI_FDM_)
    /* register SPI device CS select callback function */
    reg_wizchip_cs_cbfunc(spi_cs_select, spi_cs_deselect);
#else
#if (_WIZCHIP_IO_MODE_ & _WIZCHIP_IO_MODE_SIP_) != _WIZCHIP_IO_MODE_SIP_
#error "Unknown _WIZCHIP_IO_MODE_"
#else
    reg_wizchip_cs_cbfunc(wizchip_select, wizchip_deselect);
#endif
#endif
    /* register SPI device read/write one byte callback function */
    reg_wizchip_spi_cbfunc(spi_read_byte, spi_write_byte);

    return RT_EOK;
}

/* initialize WIZnet chip configures */
static int wiz_chip_cfg_init(void)
{
#define    CW_INIT_MODE         2
#define    CW_INIT_SOCKETS      8
#define    CW_INIT_TIMEOUT      (5 * RT_TICK_PER_SECOND)

    rt_tick_t start_tick, now_tick;
    uint8_t phy_status;
    uint8_t memsize[CW_INIT_MODE][CW_INIT_SOCKETS] = { 0 };

    /* reset WIZnet chip internal PHY, configures PHY mode. */
    if (ctlwizchip(CW_INIT_WIZCHIP, (void*) memsize) == -1)
    {
        rt_kprintf("WIZCHIP initialize failed.\n");
        return -RT_ERROR;
    }

    start_tick = rt_tick_get();
    do
    {
        now_tick = rt_tick_get();
        if (now_tick - start_tick > CW_INIT_TIMEOUT)
        {
            LOG_E("WIZnet chip configure initialize timeout.");
            return -RT_ETIMEOUT;
        }

        /* waiting for link status online */
        if (ctlwizchip(CW_GET_PHYLINK, (void*) &phy_status) == -1)
        {
            rt_kprintf("Unknown PHY Link stauts.\n");
        }

        rt_thread_mdelay(100);
    } while (phy_status == PHY_LINK_OFF);

    return RT_EOK;
}

/* WIZnet chip hardware reset */
static void wiz_reset(void)
{
    rt_pin_write(WIZ_IRQ_PIN, PIN_LOW);
    _delay_us(50);

    rt_pin_write(WIZ_IRQ_PIN, PIN_HIGH);
    _delay_us(100);
}

#ifdef WIZ_USING_DHCP
static void wiz_ip_assign(void)
{
    /* get the assigned IP address and reconfigure the IP address of the chip */
    getIPfromDHCP(wiz_net_info.ip);
    getGWfromDHCP(wiz_net_info.gw);
    getSNfromDHCP(wiz_net_info.sn);
    getDNSfromDHCP(wiz_net_info.dns);
    wiz_net_info.dhcp = NETINFO_DHCP;

    ctlnetwork(CN_SET_NETINFO, (void*) &wiz_net_info);
//    LOG_D("DHCP LEASED TIME : %d Sec.", getDHCPLeasetime());
}

static void wiz_ip_conflict(void)
{
    /* deal with conflict IP for WIZnet DHCP  */
    rt_kprintf("conflict IP from DHCP\r\n");
    RT_ASSERT(0);
}

static int wiz_network_dhcp(void)
{
#define WIZ_DHCP_SOCKET      0
#define WIZ_DHCP_TIMEOUT     (5 * RT_TICK_PER_SECOND)

    rt_tick_t start_tick, now_tick;
    uint8_t dhcp_status;
    uint8_t data_buffer[1024];

    /* set default MAC address for DHCP */
    setSHAR(wiz_net_info.mac);
    /* DHCP configure initialize, clear information other than MAC address */
    setSn_RXBUF_SIZE(WIZ_DHCP_SOCKET, 0x02);
    setSn_TXBUF_SIZE(WIZ_DHCP_SOCKET, 0x02);
    DHCP_init(WIZ_DHCP_SOCKET, data_buffer);
    /* register to assign IP address and conflict callback */
    reg_dhcp_cbfunc(wiz_ip_assign, wiz_ip_assign, wiz_ip_conflict);

    start_tick = rt_tick_get();
    while (1)
    {
        /* check DHCP timeout */
        now_tick = rt_tick_get();
        if (now_tick - start_tick > WIZ_DHCP_TIMEOUT)
        {
            DHCP_stop();
            return -RT_ETIMEOUT;
        }

        /* DHCP start, return DHCP_IP_LEASED is success. */
        dhcp_status = DHCP_run();
        switch (dhcp_status)
        {
        case DHCP_IP_ASSIGN:
        case DHCP_IP_CHANGED:
        {
            // TODO: DHCP configure
            break;
        }
        case DHCP_IP_LEASED:
        {
            DHCP_stop();
            return RT_EOK;
        }
        case DHCP_FAILED:
        {
            DHCP_stop();
            return -RT_ERROR;
        }
        default:
            continue;
        }

        rt_thread_mdelay(100);
    }
}
#endif /* WIZ_USING_DHCP */

static int wiz_netstr_to_array(const char *net_str, uint8_t (*net_array)[])
{
    int ret = 0;

    RT_ASSERT(net_str);
    RT_ASSERT(net_array);

    if(strstr(net_str, ":"))
    {
        ret = sscanf(net_str, "%02x:%02x:%02x:%02x:%02x:%02x", &(*net_array)[0], &(*net_array)[1], &(*net_array)[2],
                &(*net_array)[3],  &(*net_array)[4],  &(*net_array)[5]);
        if(ret != 6)
        {
            return -RT_ERROR;
        }
    }
    else if (strstr(net_str, "."))
    {
        ret = sscanf(net_str, "%hh.%hh.%hh.%hh", &(*net_array)[0], &(*net_array)[1], &(*net_array)[2], &(*net_array)[3]);
        if(ret != 4)
        {
            return -RT_ERROR;
        }
    }

    return RT_EOK;
}

/* initialize WIZnet network configures */
static int wiz_network_init(void)
{
#ifndef WIZ_USING_DHCP
    if(wiz_netstr_to_array(WIZ_IPADDR, &(wiz_net_info.ip)) != RT_EOK ||
            wiz_netstr_to_array(WIZ_MSKADDR, &(wiz_net_info.sn)) != RT_EOK ||
                wiz_netstr_to_array(WIZ_GWADDR, &(wiz_net_info.gw)) != RT_EOK)
    {
        return -RT_ERROR;
    }
    wiz_net_info.dhcp = NETINFO_STATIC;
#endif

    /* set static WIZnet network information */
    ctlnetwork(CN_SET_NETINFO, (void*) &wiz_net_info);
    ctlnetwork(CN_GET_NETINFO, (void*) &wiz_net_info);

#ifdef WIZ_USING_DHCP
    /* alloc IP address through DHCP */
    {
        int result = RT_EOK;
        result = wiz_network_dhcp();
        if (result != RT_EOK)
        {
            LOG_E("WIZnet network initialize failed, DHCP timeout.");
            return result;
        }
    }
#endif

    LOG_D("WIZnet network initialize success.");
    return RT_EOK;
}

/* wizenet socket initialize */
static int wiz_socket_init(void)
{
    int idx = 0;

    /* socket(0-7) initialize */
    setSIMR(0xff);

    /* set socket receive/send buffer size */
    for (idx = 0; idx < WIZ_SOCKETS_NUM; idx++)
    {
        setSn_RXBUF_SIZE(idx, 0x02);
        setSn_TXBUF_SIZE(idx, 0x02);
    }

    /* set socket ISR state support */
    for (idx = 0; idx < WIZ_SOCKETS_NUM; idx++)
    {
        setSn_IMR(idx, (IMR_TIMEOUT | IMR_RECV | IMR_DISCON));
    }

    return RT_EOK;
}

int wiz_ifconfig(void)
{
#define WIZ_ID_LEN           6
#define WIZ_MAC_LEN          6

    uint8_t interfacce[WIZ_ID_LEN];
    wiz_NetInfo net_info;

    ctlwizchip(CW_GET_ID, (void*) interfacce);
    ctlnetwork(CN_GET_NETINFO, (void*) &net_info);

    /* display network information */
    {
        uint8_t index = 0;

        rt_kprintf("network interface: %s\n", interfacce);
        rt_kprintf("MTU: %d\n", getSn_MSSR(0));
        rt_kprintf("MAC: ");
        for (index = 0; index < WIZ_MAC_LEN; index ++)
        {
            rt_kprintf("%02x ", net_info.mac[index]);
        }
        rt_kprintf("\n");

        rt_kprintf("ip address: %d.%d.%d.%d\n", net_info.ip[0], net_info.ip[1],
                net_info.ip[2], net_info.ip[3]);
        rt_kprintf("gw address: %d.%d.%d.%d\n", net_info.gw[0], net_info.gw[1],
                net_info.gw[2], net_info.gw[3]);
        rt_kprintf("net mask  : %d.%d.%d.%d\n", net_info.sn[0], net_info.sn[1],
                net_info.sn[2], net_info.sn[3]);
        rt_kprintf("dns server : %d.%d.%d.%d\n", net_info.dns[0], net_info.dns[1],
                net_info.dns[2], net_info.dns[3]);
    }

    return RT_EOK;
}

/* set WIZnet device MAC address */
int wiz_set_mac(const char *mac)
{
    int result = RT_EOK;

    RT_ASSERT(mac);

    if (wiz_init_status == RT_FALSE)
    {
        result = wiz_netstr_to_array(mac, &(wiz_net_info.mac));
        if (result != RT_EOK)
        {
            LOG_E("Input MAC address process failed.");
            return result;
        }
    }
    else
    {
        /* set default MAC address for DHCP */
        setSHAR(wiz_net_info.mac);
    }

    return RT_EOK;
}

/* WIZnet initialize device and network */
int wiz_init(void)
{
    int result = RT_EOK;

    /* WIZnet SPI device and pin initialize */
    result = wiz_device_init(WIZ_SPI_DEVICE, WIZ_RST_PIN, WIZ_IRQ_PIN);
    if (result != RT_EOK)
    {
        LOG_E("WIZnet SPI or PIN device initialize failed.");
        goto __exit;
    }

    /* WIZnet SPI device reset */
    wiz_reset();
    /* set WIZnet device read/write data callback */
    wiz_callback_register();
    /* WIZnet chip configure initialize */
    result = wiz_chip_cfg_init();
    if(result != RT_EOK)
    {
        goto __exit;
    }
    /* WIZnet network initialize */
    result = wiz_network_init();
    if (result != RT_EOK)
    {
        goto __exit;
    }
    /* WIZnet socket initialize */
    wiz_socket_init();

__exit:
    if (result == RT_EOK)
    {
        wiz_init_status = RT_TRUE;
        LOG_I("RT-Thread WIZnet package (V%s) initialize success.", WIZ_SW_VERSION);
    }
    else
    {
        LOG_E("RT-Thread WIZnet package (V%s) initialize failed.", WIZ_SW_VERSION);
    }

    return result;
}

int wiz_start(int argc, char **argv)
{
    int result = RT_EOK;

    if (argc == 1)
    {
        result = wiz_set_mac(WIZ_DEFAULT_MAC);
        if(result != RT_EOK)
        {
            return result;
        }

        wiz_init();
    }
    else if(argc == 2)
    {
        result = wiz_set_mac(argv[1]);
        if(result != RT_EOK)
        {
            return result;
        }

        wiz_init();
    }
    else
    {
        rt_kprintf("wiz_start [mac]  -- WIZnet device network start.");
        return -RT_ERROR;
    }

    return RT_EOK;
}

#ifdef FINSH_USING_MSH
MSH_CMD_EXPORT(wiz_start, WIZnet device network start);
MSH_CMD_EXPORT(wiz_ifconfig, WIZnet ifconfig);
#endif