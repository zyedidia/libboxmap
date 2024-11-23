#include <assert.h>
#include <stdio.h>

#include "boxmap.h"

static size_t gb(size_t x) {
    return x * 1024 * 1024 * 1024;
}

int main(void) {
    struct BoxMap* map = boxmap_new((struct BoxMapOptions) {
        .minalign = gb(4),
        .maxalign = gb(4),
        .guardsize = gb(4),
    });
    assert(map);

    boxmap_reserve(map, 0);
    printf("total size: %ld\n", boxmap_size(map));

    uintptr_t space = boxmap_addspace(map, gb(4));
    printf("space: %lx\n", space);

    boxmap_rmspace(map, space, gb(4));

    return 0;
}
