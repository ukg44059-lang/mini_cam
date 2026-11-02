#ifndef POWER_TEST_H
#define POWER_TEST_H
#include <stdint.h>
/* E1..E8 select case, E0 stops; L0..100 sets LED duty, I1..3600 seconds. */
void Test_LedTick(void);
#endif
