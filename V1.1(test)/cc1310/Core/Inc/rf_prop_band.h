/* V2 RF PHY settings migrated from sofeware_V1/cc1310.
 * 868 MHz and 433.92 MHz / 50 kbps 2-GFSK are both configured below.
 * Original 433 MHz export reference is recorded in the 433 MHz branch.
 * The board RF matching network and antenna must support the selected band.
 * Array is writable because RFCRTrim updates the factory RTRIM override.
 */
#ifndef __RF_PROP_BAND_H
#define __RF_PROP_BAND_H

#include <stdint.h>
#include <driverlib/rf_mailbox.h>

/* ---------------- 选频段 ---------------- */
#define RFP_BAND_868        868
#define RFP_BAND_433        433

#ifndef RFP_BAND
#define RFP_BAND            RFP_BAND_868
#endif
#ifndef RFP_RATE_KBPS
#define RFP_RATE_KBPS 50
#endif

/* 覆盖表的编码宏【直接用 TI 在 rf_mailbox.h 里的定义】，不要自己重写。
   我一开始自己写了一套，位序是反的（TI 是「地址在低位、值在高位」，
   我写成了反过来），结果整张表会写到完全错误的寄存器上 ——
   而且不会报错，只会表现为射频性能莫名其妙。
   编译器的 "redefined" 警告帮忙抓到了这个，教训：
   ★ 厂商头文件里已有的编码宏，一律直接用，不要照着文档重新推导 ★ */

/* ==========================================================================
   868 MHz / 50 kbps 2-GFSK
   来源：Contiki-NG smartrf-settings.c（BSD-3），SmartRF Studio 导出
   ========================================================================== */
#if RFP_BAND == RFP_BAND_868

#define RFP_CENTER_FREQ         868U
#define RFP_LO_DIVIDER          0x05U       /* 779–930MHz 用 div5 */
#define RFP_MOD_TYPE            0x1U        /* 2-GFSK */
#define RFP_MOD_DEVIATION       0x64U       /* 100 * 250Hz = 25kHz */
#define RFP_SYMRATE_PRESCALE    0x0FU
#define RFP_SYMRATE_RATEWORD    0x8000U     /* 合起来 = 50 kbps */
#define RFP_RX_BW               0x24U
#define RFP_WHITEN_MODE         7U
#define RFP_TX_POWER            0x9330U
#define RFP_FRACT_FREQ          0x0000U
#define RFP_FS_TX_MODE          1U
#define RFP_SYNC_WORD           0x0055904EU /* 802.15.4g 标准同步字 */
#define RFP_BAND_LABEL          "868 MHz / 50 kbps"

static uint32_t rfp_overrides[] = {
    /* PHY: 用 MCE ROM bank 4 + RFE RAM patch */
    MCE_RFE_OVERRIDE(0, 4, 0, 1, 0, 0),
    /* --- 合成器（863–930MHz div5 那一套） --- */
    HW_REG_OVERRIDE(0x4038, 0x0037),        /* RTRIM = 7 */
    (uint32_t)0x000684A3,                   /* Fref = 4MHz */
    HW_REG_OVERRIDE(0x4020, 0x7F00),        /* 精校准 */
    HW_REG_OVERRIDE(0x4064, 0x0040),        /* 精校准 */
    (uint32_t)0xB1070503,                   /* 精校准 */
    (uint32_t)0x05330523,                   /* 精校准 */
    (uint32_t)0x0A480583,                   /* 锁定后环路带宽 20kHz */
    (uint32_t)0x7AB80603,                   /* 锁定后环路带宽 20kHz */
    ADI_REG_OVERRIDE(1, 4, 0x9F),           /* VCO LDO */
    ADI_HALFREG_OVERRIDE(1, 7, 0x4, 0x4),   /* 合成器 LDO */
    (uint32_t)0x02010403,                   /* 用 24MHz XOSC 当合成器时钟 */
    (uint32_t)0x00108463,                   /* 额外 PLL 滤波 */
    (uint32_t)0x04B00243,                   /* 合成器编程超时放宽到 300us */
    /* --- 接收 --- */
    ADI_HALFREG_OVERRIDE(0, 61, 0xF, 0xD),  /* 抗混叠滤波带宽 */
    (uint32_t)0x00038883,                   /* LNA 偏置电流 trim */
    HW_REG_OVERRIDE(0x6084, 0x35F1),        /* 同步命中时冻结 RSSI */
    /* --- 发射 --- */
    HW_REG_OVERRIDE(0x6088, 0x411A),        /* 使能 PA 斜坡 + AGC 参考 0x1A */
    HW_REG_OVERRIDE(0x608C, 0x8213),        /* PA 斜坡参数 */
    (uint32_t)0x00FB88A3,                   /* RSSI 偏移（779–930 频段） */
    ADI_REG_OVERRIDE(0, 12, 0xF8),          /* PA trim 拉满 */
    /* --- CRC16 --- */
    (uint32_t)0x00000943,
    (uint32_t)0x00000963,
    (uint32_t)0xFFFFFFFF,                   /* 结束标记 */
};

/* 433 MHz / 50 kbps 2-GFSK: TI SmartRF Studio export. */
#elif RFP_BAND == RFP_BAND_433

/* TI SmartRF Studio export, 433.92 MHz / 50 kbps / 2-GFSK.
 * https://e2e.ti.com/support/wireless-connectivity/sub-1-ghz-group/sub-1-ghz/f/sub-1-ghz-forum/769016/rtos-cc1310-cc1310-433mhz
 * RF matching network and antenna must also support 433 MHz. */
#define RFP_CENTER_FREQ         433U
#define RFP_LO_DIVIDER          0x0AU
#define RFP_MOD_TYPE            0x1U
#if RFP_RATE_KBPS == 50
#define RFP_MOD_DEVIATION       0x64U
#define RFP_SYMRATE_PRESCALE    0x0FU
#define RFP_SYMRATE_RATEWORD    0x8000U
#define RFP_RX_BW               0x24U
#define RFP_RX_AAF              0xDU
#define RFP_PA_AGC              0x411AU
#define RFP_PA_RAMP             0x8213U
#elif RFP_RATE_KBPS == 500
/* Board-tested 433 MHz high-throughput profile. Keep the 433 MHz synthesizer,
 * matching and power overrides; use TI's 500 kbaud / 175 kHz / 1243 kHz
 * modem settings from the complete TI export.
 * The wide RX filter is intentional; range/sensitivity must be characterized.
 * TI export (SmartRF Studio 2.13.1): E2E thread 801214, captured in
 * rf_validation/performance/ti_500kbps_source.txt. 433 MHz approach: 1177612. */
#define RFP_MOD_DEVIATION       0x2BCU
#define RFP_SYMRATE_PRESCALE    0x06U
#define RFP_SYMRATE_RATEWORD    0x20000U
#define RFP_RX_BW               0x2FU
#define RFP_RX_AAF              0x0U
#define RFP_PA_AGC              0x1024U
#define RFP_PA_RAMP             0x0813U
#else
#error Supported board-tested rates are 50 and 500 kbps
#endif
#define RFP_SYNC_WORD           0x930B51DEU
#define RFP_WHITEN_MODE         0U
#define RFP_TX_POWER            0x913FU
#define RFP_FRACT_FREQ          0xEB9AU
#define RFP_FS_TX_MODE          0U
#define RFP_RATE_STRING_(x) #x
#define RFP_RATE_STRING(x) RFP_RATE_STRING_(x)
#define RFP_BAND_LABEL          "433.92 MHz / " RFP_RATE_STRING(RFP_RATE_KBPS) " kbps"

static uint32_t rfp_overrides[] = {
    MCE_RFE_OVERRIDE(0, 4, 0, 1, 0, 0),
    HW_REG_OVERRIDE(0x4038, 0x0037),
    (uint32_t)0x000684A3,
    HW_REG_OVERRIDE(0x4020, 0x7F00),
    HW_REG_OVERRIDE(0x4064, 0x0040),
    (uint32_t)0xB1070503,
    (uint32_t)0x05330523,
    (uint32_t)0x0A480583,
    (uint32_t)0x7AB80603,
    ADI_REG_OVERRIDE(1, 4, 0x9F),
    ADI_HALFREG_OVERRIDE(1, 7, 0x4, 0x4),
    (uint32_t)0x02010403,
    (uint32_t)0x00108463,
    (uint32_t)0x04B00243,
    HW32_ARRAY_OVERRIDE(0x405C, 1),
    (uint32_t)0x18000280,
    ADI_HALFREG_OVERRIDE(0, 61, 0xF, RFP_RX_AAF),
    (uint32_t)0x00038883,
    HW_REG_OVERRIDE(0x6084, 0x35F1),
    HW_REG_OVERRIDE(0x6088, RFP_PA_AGC),
    HW_REG_OVERRIDE(0x608C, RFP_PA_RAMP),
    (uint32_t)0x000288A3,
    (uint32_t)0xFFFFFFFF,
};

#else
#error "RFP_BAND 只支持 RFP_BAND_868 / RFP_BAND_433"
#endif

#endif /* __RF_PROP_BAND_H */
