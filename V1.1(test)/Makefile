##############################################################################
# NUCLEO-U575ZI-Q  (STM32U575ZIT6Q)  HAL 工程 Makefile
#
#   make              编译
#   make flash        编译并通过 ST-LINK 大容量存储盘拖拽烧录（最省事）
#   make flash-ocd    用 OpenOCD 烧录（需要 OpenOCD >= 0.12）
#   make size         查看 Flash / RAM 占用
#   make clean        清理
#
#   工具链路径可以在命令行覆盖：  make GCC_PATH=D:/gcc-arm/bin
##############################################################################

# 要编哪个 demo：
#   camera = OV3660 逐阶段验证（抓一帧就停）
#   stream = OV3660 实时串口图传（连续抓发，配 tools/stream_server.py）
#   led    = LED 闪烁
APP         ?= stream

# --- 只对 APP=stream 生效 ---
# BAUD       串口波特率。ST-LINK V3 的虚拟串口能跑到 2M，帧率直接正比于它
# FRAMESIZE  QQVGA(160x120，默认，约 5fps) 或 QVGA(320x240，约 1.3fps)
BAUD        ?= 2000000
FRAMESIZE   ?= 480X360

# TRANSPORT  图像走哪条通道传到 PC
#   usb   USB CDC 虚拟串口（CN15 Type-C）。默认。实测 ~700 KB/s，快得多
#   uart  ST-LINK 虚拟串口。2 Mbaud 封顶 200 KB/s，USB 有问题时的退路
#   dual  同一帧经 UART 和 SPI2(PB13/14/15) -> CC1310 UART 镜像发送
#         这三个引脚实际是 SPI2 AF5；接线标签中的 SPI3 不对应芯片复用表。
# 不管选哪个，printf 的文字日志始终走 ST-LINK 串口，方便边看日志边收图
TRANSPORT   ?= dual
ifeq ($(TRANSPORT),dual)
# Custom USB-UART bridge loses bytes on sustained 2 Mbaud VGA mirrors.
# Image radio throughput is unaffected when the optional mirror is off.
BAUD = 1000000
endif

TARGET       = STM32U575_$(APP)
DEBUG        = 1
OPT          = -Og
ifeq ($(APP),stream)
OPT          = -O2
endif
BUILD_DIR    = build

# ---------------------------------------------------------------- 工具链 ----
# 默认用 STM32CubeIDE 1.5.0 自带的 GNU Tools for STM32 (gcc 7.3.1)
GCC_PATH    ?= F:/stm32cube_ide/STM32CubeIDE_1.5.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.7-2018-q2-update.win32_1.5.0.202011040924/tools/bin
PREFIX       = arm-none-eabi-

ifdef GCC_PATH
  CC  = "$(GCC_PATH)/$(PREFIX)gcc"
  AS  = "$(GCC_PATH)/$(PREFIX)gcc" -x assembler-with-cpp
  CP  = "$(GCC_PATH)/$(PREFIX)objcopy"
  SZ  = "$(GCC_PATH)/$(PREFIX)size"
else
  CC  = $(PREFIX)gcc
  AS  = $(PREFIX)gcc -x assembler-with-cpp
  CP  = $(PREFIX)objcopy
  SZ  = $(PREFIX)size
endif
HEX = $(CP) -O ihex
BIN = $(CP) -O binary -S

# ST-LINK 虚拟 U 盘盘符（板子插上后在“此电脑”里那个 NOD_U575ZI 盘）
DND_DRIVE   ?= E:

# ---------------------------------------------------------------- 源文件 ----
APP_SOURCE  = Core/Src/app_$(APP).c
ifeq ($(APP),test_pwm)
  APP_SOURCE = test_pwm/app_pwm.c
endif

C_SOURCES =  \
$(APP_SOURCE) \
Core/Src/bsp.c \
Core/Src/camera_port.c \
Drivers/BSP/OV3660/ov3660.c \
Core/Src/stm32u5xx_it.c \
Core/Src/stm32u5xx_hal_msp.c \
Core/Src/system_stm32u5xx.c \
Core/Src/syscalls.c \
Core/Src/sysmem.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_cortex.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_dcmi.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_dma.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_dma_ex.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_exti.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_flash.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_flash_ex.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_gpio.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_i2c.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_i2c_ex.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_icache.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_pwr.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_pwr_ex.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_rcc.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_rcc_ex.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_spi.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_spi_ex.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_tim.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_tim_ex.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_uart.c \
Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_uart_ex.c

# USB CDC 相关。只有 TRANSPORT=usb 时才加进来，
# 免得跑 UART 版本的时候白白多编 6 个文件、多占十几 KB flash
USB_SOURCES = Core/Src/usbd_conf.c Core/Src/usbd_desc.c Core/Src/usbd_cdc_if.c Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_core.c Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ctlreq.c Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ioreq.c Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src/usbd_cdc.c Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_pcd.c Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_hal_pcd_ex.c Drivers/STM32U5xx_HAL_Driver/Src/stm32u5xx_ll_usb.c

ASM_SOURCES = Core/Startup/startup_stm32u575xx.s

# ------------------------------------------------------------- 编译选项 ----
CPU        = -mcpu=cortex-m33
FPU        = -mfpu=fpv5-sp-d16
FLOAT-ABI  = -mfloat-abi=hard
MCU        = $(CPU) -mthumb $(FPU) $(FLOAT-ABI)

C_DEFS     = -DUSE_HAL_DRIVER -DSTM32U575xx

ifeq ($(APP),stream)
  C_DEFS += -DVCP_BAUDRATE=$(BAUD)U
  ifeq ($(FRAMESIZE),QVGA)
    C_DEFS += -DSTREAM_FRAMESIZE=OV3660_FRAMESIZE_QVGA -DSTREAM_W=320U -DSTREAM_H=240U
  endif
  ifeq ($(FRAMESIZE),480X360)
    C_DEFS += -DSTREAM_FRAMESIZE=OV3660_FRAMESIZE_480X360 -DSTREAM_W=480U -DSTREAM_H=360U
  endif
  ifeq ($(TRANSPORT),usb)
    C_DEFS    += -DSTREAM_TRANSPORT_USB -DUSE_USB_CDC
    C_SOURCES += $(USB_SOURCES)
  endif
  ifeq ($(TRANSPORT),dual)
    C_DEFS += -DSTREAM_TRANSPORT_DUAL
    C_SOURCES += Core/Src/cc1310_link.c
  endif
endif

C_INCLUDES = \
-ICore/Inc \
-IDrivers/BSP/OV3660 \
-IDrivers/STM32U5xx_HAL_Driver/Inc \
-IDrivers/STM32U5xx_HAL_Driver/Inc/Legacy \
-IDrivers/CMSIS/Device/ST/STM32U5xx/Include \
-IDrivers/CMSIS/Include -IMiddlewares/ST/STM32_USB_Device_Library/Core/Inc -IMiddlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc

ifeq ($(APP),spitest)
  C_SOURCES += Core/Src/cc1310_link.c
endif

ASFLAGS = $(MCU) $(OPT) -Wall -fdata-sections -ffunction-sections
CFLAGS  = $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections

ifeq ($(DEBUG), 1)
CFLAGS += -g -gdwarf-2
endif

# 自动生成 .d 依赖文件，改了头文件也会重新编译
CFLAGS += -MMD -MP -MF"$(@:%.o=%.d)"

# ------------------------------------------------------------- 链接选项 ----
LDSCRIPT = STM32U575ZITXQ_FLASH.ld
LIBS     = -lc -lm -lnosys
LIBDIR   =
LDFLAGS  = $(MCU) -specs=nano.specs -T$(LDSCRIPT) $(LIBDIR) $(LIBS) \
           -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections

# printf 想打印 %f 浮点，把下面这行的注释去掉
# LDFLAGS += -u _printf_float

# ---- 配置指纹 ----
# 改了 APP / BAUD / FRAMESIZE 只会改变编译选项，源文件时间戳没动，
# make 看不出来、不会重编，结果就是「改了参数烧上去还是旧的」。
# 这里存一份指纹，对不上就把 build 整个删掉重编。
CONFIG_SIG := $(APP)|$(BAUD)|$(FRAMESIZE)|$(TRANSPORT)|$(OPT)|$(DEBUG)
ifneq ($(CONFIG_SIG),$(shell cat $(BUILD_DIR)/.config 2>/dev/null))
  $(shell rm -rf $(BUILD_DIR))
  $(shell mkdir -p $(BUILD_DIR))
  $(shell echo "$(CONFIG_SIG)" > $(BUILD_DIR)/.config)
endif

# --------------------------------------------------------------- 构建 ------
all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

OBJECTS  = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(notdir $(<:.c=.lst)) $< -o $@

$(BUILD_DIR)/%.o: %.s Makefile | $(BUILD_DIR)
	$(AS) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SZ) $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(HEX) $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(BIN) $< $@

$(BUILD_DIR):
	mkdir $@

# --------------------------------------------------------------- 烧录 ------
# 方式一：拖拽到 ST-LINK 虚拟 U 盘（最稳，不依赖任何调试器软件）
flash: all
	@echo 正在拷贝 $(BUILD_DIR)/$(TARGET).bin 到 $(DND_DRIVE) ...
	cp $(BUILD_DIR)/$(TARGET).bin $(DND_DRIVE)/
	@echo 烧录完成，板子会自动复位运行。

# 方式二：OpenOCD（需要 0.12 及以上版本，自带 stm32u5x.cfg）
OPENOCD ?= openocd
flash-ocd: all
	$(OPENOCD) -f interface/stlink.cfg -f target/stm32u5x.cfg \
	  -c "program $(BUILD_DIR)/$(TARGET).elf verify reset exit"

# CMSIS-DAP / DAPLink / Horco 等 DAP 下载器
flash-dap: all
	$(OPENOCD) -f interface/cmsis-dap.cfg -f target/stm32u5x.cfg \
	  -c "program $(BUILD_DIR)/$(TARGET).elf verify reset exit"

# 方式三：STM32CubeProgrammer CLI
CUBEPROG ?= STM32_Programmer_CLI
flash-cube: all
	$(CUBEPROG) -c port=SWD mode=UR -w $(BUILD_DIR)/$(TARGET).hex -rst

size: $(BUILD_DIR)/$(TARGET).elf
	$(SZ) -A -x $<
	$(SZ) $<

clean:
	-rm -rf $(BUILD_DIR)

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean flash flash-ocd flash-dap flash-cube size
