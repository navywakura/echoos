/* Format a block device with an empty RXFS image (RXF1). */
#include "rxfs.h"

void rxfs_format(void) {
    rxfs_init();          /* all slots marked unused; contents irrelevant */
}
