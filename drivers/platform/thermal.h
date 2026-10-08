/* Thermal sensor API. */
#ifndef RXOS_THERMAL_H
#define RXOS_THERMAL_H

#include "rogex.h"

void        thermal_init(void);
bool        thermal_available(void);
const char *thermal_status(void);          /* why, when unavailable          */
bool        thermal_core_c(uint32_t *celsius);
bool        thermal_package_c(uint32_t *celsius);
uint32_t    thermal_tjmax(void);           /* throttling point, degrees C    */
bool        thermal_tjmax_measured(void);  /* false = using the 100 C default */

#endif /* RXOS_THERMAL_H */
