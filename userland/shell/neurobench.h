/* Neuromorphic bench command surface. */
#ifndef RXOS_NEUROBENCH_H
#define RXOS_NEUROBENCH_H

#include "rogex.h"

/* Runs every experiment and writes the report through the shell's output
   builder (o_str/o_dec), so it works in the browser shell and the window
   manager's terminal alike. */
void neurobench_run(void);

#endif /* RXOS_NEUROBENCH_H */
