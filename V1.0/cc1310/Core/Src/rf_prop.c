/**
  ******************************************************************************
  * @file    rf_prop.c
  * @brief   CC1310 prop-mode 裸机射频层实现
  *
  * 参考 TI driverlib 的 rfc.c 和 Contiki-NG 的 rf-core 实现思路，
  * 但不引入 TI-RTOS，命令直接通过门铃(doorbell)同步提交。
  *
  * ---------------------------------------------------------------------------
  * CC13xx 射频的启动顺序（顺序错了就起不来，而且现象各不相同）
  * ---------------------------------------------------------------------------
  *   1. RF core 电源域上电         少了：写 RF core 寄存器全无效
  *   2. 打开 RF core 时钟          少了：同上
  *   3. ★ 切到 24MHz 晶振 ★       少了：合成器锁不上（CMD_FS 报错）
  *      复位后跑的是 RCOSC_HF（48MHz RC 振荡器），精度只有百分之几，
  *      射频合成器必须要晶振级的精度。这一步最容易漏，
  *      因为不切晶振时 CPU 和串口都工作正常，只有射频不行。
  *   4. 唤醒 RF core（CMD_PING）
  *   5. 打 CPE/RFE 补丁            少了：某些 PHY 行为不对，或直接卡死
  *   6. CMD_PROP_RADIO_DIV_SETUP   配调制方式、速率、频段
  *   7. RFCRfTrimSet + RFCRTrim    ★ 用芯片出厂 trim 校准射频 ★
  *      少了：频偏和功率会偏，且每颗芯片偏得不一样
  *   8. CMD_FS                     锁定到目标频率
  *   9. CMD_PROP_TX                发包
  ******************************************************************************
  */

#include <string.h>

#include <driverlib/prcm.h>
#include <driverlib/cpu.h>
#include <driverlib/rfc.h>
#include <driverlib/osc.h>
#include <driverlib/aon_rtc.h>
#include <driverlib/rf_common_cmd.h>
#include <driverlib/rf_prop_cmd.h>
#include <driverlib/rf_prop_mailbox.h>
#include <driverlib/rf_mailbox.h>
#include <driverlib/rf_data_entry.h>
#include <inc/hw_memmap.h>
#include <inc/hw_rfc_pwr.h>
#include <inc/hw_rfc_dbell.h>
#include <inc/hw_prcm.h>

#include <rf_patches/rf_patch_cpe_genfsk.h>
#include <rf_patches/rf_patch_rfe_genfsk.h>

#include "rf_prop.h"
#include "rf_prop_band.h"

/* 门铃返回值：低 8 位是 CMDSTA */
#define CMDSTA_DONE         0x01U

static uint16_t s_last_status;
/* JTAG-visible startup marker; useful when UART pins differ between boards. */
volatile uint32_t g_rfp_debug_stage;

/* 发送缓冲要常驻（RF core 会 DMA 读它），不能放栈上 */
static uint8_t  s_tx_buf[RFP_MAX_PAYLOAD];

#define RFP_RX_CAPACITY RFP_MAX_PAYLOAD
static uint32_t s_rx_storage[(8U + 1U + RFP_RX_CAPACITY + 3U) / 4U];
static dataQueue_t s_rx_queue;

/* ------------------------------------------------------------ 命令结构 ---- */

static rfc_CMD_PROP_RADIO_DIV_SETUP_t s_cmd_setup =
{
    .commandNo                = CMD_PROP_RADIO_DIV_SETUP,
    .status                   = 0x0000,
    .pNextOp                  = 0,
    .startTime                = 0x00000000,
    .startTrigger.triggerType = TRIG_NOW,
    .condition.rule           = COND_NEVER,

    .modulation.modType       = RFP_MOD_TYPE,
    .modulation.deviation     = RFP_MOD_DEVIATION,
    .symbolRate.preScale      = RFP_SYMRATE_PRESCALE,
    .symbolRate.rateWord      = RFP_SYMRATE_RATEWORD,
    .rxBw                     = RFP_RX_BW,

    .preamConf.nPreamBytes    = 4,          /* 4 字节前导 */
    .formatConf.nSwBits       = 32,         /* 32 位同步字 */
    .formatConf.bMsbFirst     = 1,
    .formatConf.whitenMode    = RFP_WHITEN_MODE,

    .config.frontEndMode      = 0,          /* 差分 */
    .config.biasMode          = 1,          /* 外部偏置 */
    .txPower                  = RFP_TX_POWER,
    .pRegOverride             = (uint32_t *)rfp_overrides,
    .centerFreq               = RFP_CENTER_FREQ,
    .intFreq                  = 0x8000,     /* 用推荐中频 */
    .loDivider                = RFP_LO_DIVIDER,
};

static rfc_CMD_FS_t s_cmd_fs =
{
    .commandNo                = CMD_FS,
    .status                   = 0x0000,
    .pNextOp                  = 0,
    .startTime                = 0x00000000,
    .startTrigger.triggerType = TRIG_NOW,
    .condition.rule           = COND_NEVER,
    .frequency                = RFP_CENTER_FREQ,
    .fractFreq                = RFP_FRACT_FREQ,
    .synthConf.bTxMode        = RFP_FS_TX_MODE,
    .synthConf.refFreq        = 0,
};

static rfc_CMD_PROP_TX_t s_cmd_tx =
{
    .commandNo                = CMD_PROP_TX,
    .status                   = 0x0000,
    .pNextOp                  = 0,
    .startTime                = 0x00000000,
    .startTrigger.triggerType = TRIG_NOW,
    .condition.rule           = COND_NEVER,
    .pktConf.bFsOff           = 0,          /* 发完不关合成器，下一包更快 */
    .pktConf.bUseCrc          = 1,
    .pktConf.bVarLen          = 1,          /* 变长包，长度字节在前 */
    .pktLen                   = 0,
    .syncWord                 = RFP_SYNC_WORD,
    .pPkt                     = s_tx_buf,
};

static rfc_CMD_PROP_RX_t s_cmd_rx =
{
    .commandNo                = CMD_PROP_RX,
    .startTrigger.triggerType = TRIG_NOW,
    .condition.rule           = COND_NEVER,
    .pktConf.bRepeatNok       = 1,
    .pktConf.bUseCrc          = 1,
    .pktConf.bVarLen          = 1,
    .rxConf.bAutoFlushCrcErr  = 1,
    .rxConf.bIncludeHdr       = 1,
    .syncWord                 = RFP_SYNC_WORD,
    .maxPktLen                = RFP_RX_CAPACITY,
    .endTrigger.triggerType   = TRIG_REL_START,
    .pQueue                   = &s_rx_queue,
};

/* --------------------------------------------------------------- 内部 ---- */

static void delay_us(uint32_t us)
{
    /* 48MHz，CPUdelay 每次 3 周期 */
    if (us) CPUdelay((48U / 3U) * us);
}

/**
  * @brief 同步提交一条 RF 命令并等它做完
  * @note  两级判断，缺一不可：
  *          1) 门铃返回的 CMDSTA —— 命令有没有【被接收】
  *          2) 命令结构里的 status —— 命令有没有【执行成功】
  *        只看第一个的话，命令被接收但执行失败会被当成成功，
  *        表现为"初始化全过了但就是发不出去"。
  */
static bool cmd_execute_sync(void *cmd, uint16_t done_status, uint32_t timeout_us)
{
    rfc_radioOp_t *op = (rfc_radioOp_t *)cmd;
    uint32_t cmdsta;
    uint32_t waited = 0;

    op->status = IDLE;

    cmdsta = RFCDoorbellSendTo((uint32_t)cmd);
    if ((cmdsta & 0xFFU) != CMDSTA_DONE)
    {
        s_last_status = (uint16_t)(cmdsta & 0xFFU);
        return false;
    }

    while ((op->status & 0x0F00U) == 0x0000U)     /* 高 4 位=0 表示还在跑 */
    {
        delay_us(10);
        waited += 10U;
        if (waited > timeout_us)
        {
            s_last_status = op->status;
            return false;
        }
    }

    s_last_status = op->status;
    return (op->status == done_status);
}

/* --------------------------------------------------------------- 对外 ---- */

rfp_status_t RFP_Init(void)
{
    rfTrim_t rf_trim;
    uint32_t cmdsta;
    uint32_t guard;

    g_rfp_debug_stage = 1U;
    /* OpenOCD's vectreset only resets Cortex-M. Stop any RF operation left
     * running by the previous firmware before booting the RF core again. */
    PRCMDomainDisable(PRCM_DOMAIN_RFCORE);
    PRCMLoadSet();
    while (!PRCMLoadGet()) { }
    PRCMPowerDomainOff(PRCM_DOMAIN_RFCORE);
    guard = 0U;
    while (PRCMPowerDomainStatus(PRCM_DOMAIN_RFCORE) != PRCM_DOMAIN_POWER_OFF)
    {
        if (++guard > 1000000U) return RFP_ERR_POWER;
    }

    g_rfp_debug_stage = 2U;
    /* ---- 1. RF core 电源域 ---- */
    PRCMPowerDomainOn(PRCM_DOMAIN_RFCORE);
    guard = 0;
    while (PRCMPowerDomainStatus(PRCM_DOMAIN_RFCORE) != PRCM_DOMAIN_POWER_ON)
    {
        if (++guard > 1000000U)
        {
            return RFP_ERR_POWER;
        }
    }

    g_rfp_debug_stage = 3U;
    /* ---- 2. RF core 时钟 ---- */
    PRCMDomainEnable(PRCM_DOMAIN_RFCORE);
    PRCMLoadSet();
    while (!PRCMLoadGet()) { }
    HWREG(RFC_DBELL_NONBUF_BASE + RFC_DBELL_O_RFCPEIFG) = 0U;
    HWREG(RFC_DBELL_NONBUF_BASE + RFC_DBELL_O_RFCPEIEN) = 0U;
    RFCClockEnable();

    /* CC13x0 ROM boot sequence: retain modem and RFE RAM while the CPE
     * starts. Without this RF_CMD0 request, radio operations may remain
     * ACTIVE forever even though CMD_PING succeeds. */
    HWREG(RFC_DBELL_BASE + RFC_DBELL_O_RFACKIFG) = 0U;
    HWREG(RFC_DBELL_BASE + RFC_DBELL_O_CMDR) =
        CMDR_DIR_CMD_2BYTE(0x0607U, RFC_PWR_PWMCLKEN_MDMRAM |
                                     RFC_PWR_PWMCLKEN_RFERAM);

    /*
     * ---- 3. 切到 24MHz 晶振 ----
     * 复位后跑的是 RCOSC_HF（RC 振荡器），精度百分之几级别，
     * 射频合成器锁不住。这一步不做的话 CPU/串口全正常，只有射频不工作，
     * 极易误判成配置问题。
     */
    g_rfp_debug_stage = 4U;
    OSCHF_TurnOnXosc();
    guard = 0;
    while (!OSCHF_AttemptToSwitchToXosc())
    {
        delay_us(100);
        if (++guard > 20000U)               /* 约 2 秒 */
        {
            return RFP_ERR_XOSC;
        }
    }

    g_rfp_debug_stage = 5U;
    /* ---- 4. Wake RF core. Keep RFACKIFG set for patch hand-off, matching
     * TI's RF core boot sequence instead of using RFCDoorbellSendTo(). ---- */
    HWREG(RFC_DBELL_BASE + RFC_DBELL_O_CMDR) = CMDR_DIR_CMD(CMD_PING);
    guard = 0U;
    do {
        cmdsta = HWREG(RFC_DBELL_BASE + RFC_DBELL_O_CMDSTA);
        if (++guard > 100000U) return RFP_ERR_POWER;
    } while ((cmdsta & 0xFFU) == CMDSTA_Pending);
    if ((cmdsta & 0xFFU) != CMDSTA_DONE) return RFP_ERR_POWER;

    g_rfp_debug_stage = 6U;
    /* CC1310 uses RF core mode 3 (proprietary Sub-1 GHz). */
    HWREG(PRCM_BASE + PRCM_O_RFCMODESEL) = PRCM_RFCMODESEL_CURR_MODE3;

    /* ---- 5. 打补丁 ---- */
    g_rfp_debug_stage = 7U;
#if 1 /* Required generic-FSK patches for proprietary Sub-1 GHz operation. */
    RFCCPEPatchReset();
    rf_patch_cpe_genfsk();
    /* Patch helpers copy code to RF RAM; they do not issue a mailbox command
     * and therefore do not create an ACK event to wait for. */
    g_rfp_debug_stage = 8U;
    rf_patch_rfe_genfsk();
#else
    g_rfp_debug_stage = 8U;
#endif

    /* Factory trim and RTRIM override must be applied before radio setup. */
    g_rfp_debug_stage = 9U;
    cmdsta = RFCDoorbellSendTo(CMDR_DIR_CMD_1BYTE(CMD_BUS_REQUEST, 1));
    if ((cmdsta & 0xFFU) != CMDSTA_DONE)
    {
        s_last_status = (uint16_t)(cmdsta & 0xFFU);
        return RFP_ERR_SETUP;
    }
    RFCAdi3VcoLdoVoltageMode(true);
    RFCRfTrimRead((rfc_radioOp_t *)&s_cmd_setup, &rf_trim);
    RFCRfTrimSet(&rf_trim);
    RFCRTrim((rfc_radioOp_t *)&s_cmd_setup);
    (void)RFCDoorbellSendTo(CMDR_DIR_CMD_1BYTE(CMD_BUS_REQUEST, 0));

    g_rfp_debug_stage = 10U;
    /* CMD_START_RAT is a direct command.  The synchronous variant is a
     * radio operation that waits for the RTC synchronisation instant and can
     * legitimately remain ACTIVE when no RF scheduler is running.  This
     * bare-metal application needs the immediate, unsynchronised start. */
    cmdsta = RFCDoorbellSendTo(CMDR_DIR_CMD(CMD_START_RAT));
    if ((cmdsta & 0xFFU) != CMDSTA_DONE)
    {
        s_last_status = (uint16_t)(cmdsta & 0xFFU);
        return RFP_ERR_SETUP;
    }

    g_rfp_debug_stage = 11U;
    /* ---- 6. 电台配置：上电后的第一条 radio operation ---- */
    if (!cmd_execute_sync(&s_cmd_setup, PROP_DONE_OK, 500000U))
    {
        /* setup 用的是通用完成码，PROP_DONE_OK 对不上时再认 DONE_OK */
        if (s_last_status != DONE_OK)
        {
            return RFP_ERR_SETUP;
        }
    }

    g_rfp_debug_stage = 12U;
    /* ---- 8. 锁频 ---- */
    if (!cmd_execute_sync(&s_cmd_fs, DONE_OK, 500000U))
    {
        return RFP_ERR_FS;
    }

    g_rfp_debug_stage = 13U;
    return RFP_OK;
}

rfp_status_t RFP_Send(const uint8_t *data, uint8_t len)
{
    if ((data == NULL) || (len == 0U) || (len > RFP_MAX_PAYLOAD))
    {
        return RFP_ERR_PARAM;
    }

    memcpy(s_tx_buf, data, len);
    s_cmd_tx.pktLen = len;
    s_cmd_tx.pPkt   = s_tx_buf;

    /* 一包 255 字节 @50kbps 约 41ms，超时给 500ms 足够 */
    if (!cmd_execute_sync(&s_cmd_tx, PROP_DONE_OK, 500000U))
    {
        return RFP_ERR_TX;
    }
    return RFP_OK;
}

uint16_t RFP_LastCmdStatus(void)
{
    return s_last_status;
}

rfp_status_t RFP_Receive(uint8_t *data, uint8_t capacity,
                         uint8_t *received_len, uint16_t timeout_ms)
{
    rfc_dataEntryGeneral_t *entry = (rfc_dataEntryGeneral_t *)s_rx_storage;
    uint8_t packet_len;

    if ((data == NULL) || (received_len == NULL) || (capacity == 0U) ||
        (capacity > RFP_RX_CAPACITY) || (timeout_ms == 0U) ||
        (timeout_ms > 10000U))
    {
        return RFP_ERR_PARAM;
    }

    *received_len = 0U;
    memset(s_rx_storage, 0, sizeof(s_rx_storage));
    entry->pNextEntry = (uint8_t *)entry;
    entry->status = DATA_ENTRY_PENDING;
    entry->config.type = DATA_ENTRY_TYPE_GEN;
    entry->length = 1U + RFP_RX_CAPACITY;
    s_rx_queue.pCurrEntry = (uint8_t *)entry;
    s_rx_queue.pLastEntry = NULL;

    s_cmd_rx.endTime = (uint32_t)timeout_ms * 4000U;
    if (!cmd_execute_sync(&s_cmd_rx, PROP_DONE_OK,
                          (uint32_t)timeout_ms * 1000U + 200000U))
    {
        if (s_last_status == PROP_DONE_RXTIMEOUT)
        {
            return RFP_ERR_RX_TIMEOUT;
        }
        return RFP_ERR_RX;
    }

    if (entry->status != DATA_ENTRY_FINISHED)
    {
        return RFP_ERR_RX;
    }
    packet_len = entry->data;
    if ((packet_len == 0U) || (packet_len > RFP_RX_CAPACITY) ||
        (packet_len > capacity))
    {
        return RFP_ERR_RX;
    }
    memcpy(data, (const uint8_t *)&entry->data + 1U, packet_len);
    *received_len = packet_len;
    return RFP_OK;
}

const char *RFP_StatusStr(rfp_status_t s)
{
    switch (s)
    {
        case RFP_OK:         return "OK";
        case RFP_ERR_POWER:  return "RF core 电源域/唤醒失败";
        case RFP_ERR_XOSC:   return "切 24MHz 晶振失败（查晶振焊接）";
        case RFP_ERR_SETUP:  return "电台配置失败（覆盖表或频段参数不对）";
        case RFP_ERR_FS:     return "锁频失败（合成器没锁上）";
        case RFP_ERR_TX:     return "发包失败";
        case RFP_ERR_RX:     return "RX failed";
        case RFP_ERR_RX_TIMEOUT: return "RX timeout";
        default:             return "参数错";
    }
}
