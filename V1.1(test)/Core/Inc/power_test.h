#ifndef POWER_TEST_H
#define POWER_TEST_H
#include <stdint.h>
/* Protocol 4: E1 full, E2 capture only, E3 camera standby/run, E4 MCU Sleep.
 * E0 safe pause (same hardware state as E4). Radio control always remains live. */
void Test_LedTick(void);
#endif
