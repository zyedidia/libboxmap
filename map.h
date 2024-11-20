#pragma once

#include "boxmap.h"
#include "buddy.h"

enum {
    ADDR_REGION_MAX = 16,
};

struct AddrRegion {
    void* base;
    size_t size;
    size_t active;
    struct buddy* alloc;
};

struct BoxMap {
    struct AddrRegion regions[ADDR_REGION_MAX];
    size_t nregions;

    struct BoxMapOptions opts;
};
