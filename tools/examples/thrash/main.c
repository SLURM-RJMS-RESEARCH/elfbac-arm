#include <stdio.h>

#include "libc.h"

__attribute__((section(".data.buf")))
char buf[256];

void
_start(void)
{
    int i;
    for (i = 0; i < 5; i++) {
        read(0, buf, sizeof(buf));
        write(1, buf, sizeof(buf));
    }
    _exit(0);
}

