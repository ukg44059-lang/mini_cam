#include <driverlib/ioc.h>

/* Confirmed on the COM19/COM20 board: COM20 receives 1920 at 115200. */
#define UART_TEST_TX_DIO  IOID_1
#define UART_TEST_RX_DIO  IOID_2
#define UART_TEST_TEXT    "1920"
#include "uart_test_impl.h"
