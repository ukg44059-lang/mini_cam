##############################################################################
# LAUNCHXL-CC1310 (CC1310F128RGZ, Cortex-M3) 裸机工程 Makefile
#
#   make              编译
#   make flash        编译并用 OpenOCD + XDS110 烧录
#   make erase        整片擦除（芯片被锁时的救命稻草）
#   make gdb-server    起 OpenOCD gdb server，配 VSCode 调试
#   make size         看 Flash / RAM 占用
#   make clean        清理
#
# 工具全部复用上一级 STM32 工程已经装好的那套，没有额外依赖：
#   编译器  STM32CubeIDE 1.5.0 自带的 arm-none-eabi-gcc 7.3.1（Cortex-M3 够用）
#   烧录    xpack-openocd 0.12（自带 interface/xds110.cfg + board/ti_cc13x0_launchpad.cfg）
##############################################################################

APP        ?= blink

# 射频频段（只对 APP=rftx 之类用到射频的 demo 有意义）
#   868  LAUNCHXL-CC1310 的原生频段，配置齐全，可直接跑
#   433  已包含 433.92 MHz / 50 kbps 配置，双板 demo 见 RF433_LINK_TEST.md
BAND       ?= 868
RF_RATE    ?= 50
RF_UART_BAUD ?= 115200
RF_ACK_GUARD_US ?= 2000
ifneq ($(filter $(APP),rf_image_tx rf_image_rx),)
RF_RATE = 500
RF_ACK_GUARD_US = 300
ifeq ($(APP),rf_image_rx)
RF_UART_BAUD = 921600
endif
endif
TARGET      = cc1310_$(APP)
DEBUG       = 1
OPT         = -Og
ifneq ($(filter $(APP),rf_image_tx rf_image_rx),)
OPT         = -O2
endif
BUILD_DIR   = build

# ---------------------------------------------------------------- 工具链 ----
GCC_PATH   ?= F:/stm32cube_ide/STM32CubeIDE_1.5.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.7-2018-q2-update.win32_1.5.0.202011040924/tools/bin
PREFIX      = arm-none-eabi-
CC          = "$(GCC_PATH)/$(PREFIX)gcc"
AS          = "$(GCC_PATH)/$(PREFIX)gcc" -x assembler-with-cpp
CP          = "$(GCC_PATH)/$(PREFIX)objcopy"
SZ          = "$(GCC_PATH)/$(PREFIX)size"
HEX         = $(CP) -O ihex
BIN         = $(CP) -O binary -S

OPENOCD    ?= ../../tools/openocd/bin/openocd.exe
OOCD_SCR   ?= ../../tools/openocd/openocd/scripts
# 这块板子 OpenOCD 自带现成配置，不用自己写 interface/target
OOCD_CFG    = -s $(OOCD_SCR) -f board/ti_cc13x0_launchpad.cfg

# ---------------------------------------------------------------- 源文件 ----
DRVLIB      = Drivers/cc13xxware

C_SOURCES   = \
Core/Src/app_$(APP).c \
$(wildcard Core/Src/rf_*.c) \
$(DRVLIB)/startup_files/startup_gcc.c \
$(DRVLIB)/startup_files/ccfg.c \
$(wildcard $(DRVLIB)/driverlib/*.c)

# ---------------------------------------------------------------- 编译选项 --
# CC1310 是 Cortex-M3：没有 FPU，也没有 DSP 指令，别照抄 M4/M33 的选项
CPU         = -mcpu=cortex-m3
MCU         = $(CPU) -mthumb

C_DEFS      = -DRFP_BAND=$(BAND) -DRFP_RATE_KBPS=$(RF_RATE) -DRF_IMAGE_UART_BAUD=$(RF_UART_BAUD) -DRFI_ACK_GUARD_US=$(RF_ACK_GUARD_US)
ifneq ($(RF_UART_TX),)
C_DEFS     += -DRF_IMAGE_UART_TX=$(RF_UART_TX)
endif
ifneq ($(RF_UART_RX),)
C_DEFS     += -DRF_IMAGE_UART_RX=$(RF_UART_RX)
endif

C_INCLUDES  = \
-ICore/Inc \
-I$(DRVLIB)

CFLAGS      = $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -Wall \
              -fdata-sections -ffunction-sections \
              -std=gnu99

ifeq ($(DEBUG), 1)
CFLAGS     += -g -gdwarf-2
endif

CFLAGS     += -MMD -MP -MF"$(@:%.o=%.d)"

# ---------------------------------------------------------------- 链接 ------
LDSCRIPT    = cc1310f128.ld
LIBS        = -lc -lm -lnosys
LDFLAGS     = $(MCU) -specs=nano.specs -T$(LDSCRIPT) $(LIBS) \
              -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections

# 改了 APP/OPT/DEBUG 时编译选项变了但源文件时间戳没动，make 看不出来。
# 存一份配置指纹，对不上就整个重编（和 STM32 工程同一套做法）。
CONFIG_SIG := $(APP)|$(BAND)|$(OPT)|$(DEBUG)|$(RF_UART_TX)|$(RF_UART_RX)|$(RF_RATE)|$(RF_UART_BAUD)|$(RF_ACK_GUARD_US)
ifneq ($(CONFIG_SIG),$(shell cat $(BUILD_DIR)/.config 2>/dev/null))
  $(shell rm -rf $(BUILD_DIR))
  $(shell mkdir -p $(BUILD_DIR))
  $(shell echo "$(CONFIG_SIG)" > $(BUILD_DIR)/.config)
endif

# ---------------------------------------------------------------- 构建 ------
all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))

$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SZ) $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(HEX) $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(BIN) $< $@

$(BUILD_DIR):
	mkdir -p $@

# ---------------------------------------------------------------- 烧录 ------
flash: all
	$(OPENOCD) $(OOCD_CFG) -c "program $(BUILD_DIR)/$(TARGET).elf verify reset exit"

# 芯片被错误的 CCFG 锁住时用这个。注意整片擦除会连 CCFG 一起擦掉，
# 擦完是空片（0xFF），调试口重新可用
erase:
	$(OPENOCD) $(OOCD_CFG) -c "init; halt; flash erase_sector 0 0 last; exit"

gdb-server:
	$(OPENOCD) $(OOCD_CFG)

size: $(BUILD_DIR)/$(TARGET).elf
	$(SZ) $<

clean:
	-rm -rf $(BUILD_DIR)

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all flash erase gdb-server size clean
