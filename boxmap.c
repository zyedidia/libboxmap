#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "err.h"
#include "map.h"

static uintptr_t truncp(uintptr_t addr, size_t align) {
    size_t align_mask = align - 1;
    return addr & ~align_mask;
}

static uintptr_t ceilp(uintptr_t addr, size_t align) {
    size_t align_mask = align - 1;
    return (addr + align_mask) & ~align_mask;
}

static size_t gb(size_t x) {
    return x * 1024 * 1024 * 1024;
}

static size_t tb(size_t x) {
    return x * 1024 * 1024 * 1024 * 1024;
}

struct BoxMap* boxmap_new(struct BoxMapOptions opts) {
    struct BoxMap* map = calloc(sizeof(struct BoxMap), 1);
    if (!map)
        return NULL;
    map->opts = opts;
    return map;
}

void boxmap_delete(struct BoxMap* map) {
    for (size_t i = 0; i < map->nregions; i++) {
        munmap(map->regions[i].base, map->regions[i].size);
    }

    free(map);
}

uint64_t boxmap_size(struct BoxMap* map) {
    size_t total = 0;
    for (size_t i = 0; i < map->nregions; i++) {
        total += map->regions[i].size - 2 * map->opts.guardsize;
    }
    return total;
}

uint64_t boxmap_active(struct BoxMap* map) {
    size_t total = 0;
    for (size_t i = 0; i < map->nregions; i++) {
        total += map->regions[i].active;
    }
    return total;
}

// Attempt to reserve as much virtual address space as possible, starting with
// 'size'. Returns 0 if it is not able to reserve at least 'threshold'.
static size_t reserve(size_t size, size_t threshold, void** base) {
    void* p;
    do {
        p = mmap(NULL, size, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (p == (void*) -1) {
            size /= 2;
        } else {
            munmap(p, size);
        }
        if (size < threshold)
            return 0;
    } while (p == (void*) -1);
    *base = p;
    return size;
}

static struct buddy* buddynew(void* base, size_t size, size_t align) {
    unsigned char* at = malloc(buddy_sizeof_alignment(size, align));
    if (!at) {
        return NULL;
    }
    return buddy_init_alignment(at, base, size, align);
}

static bool addregion(struct BoxMap* map, void* base, size_t size) {
    if (map->nregions >= ADDR_REGION_MAX) {
        g_err = "boxmap: no available regions";
        return false;
    }

    uintptr_t alignbase = ceilp((uintptr_t) base, map->opts.maxalign);
    size_t alignsize = truncp(alignbase + (size - (alignbase - (uintptr_t) base)), map->opts.maxalign) - alignbase;

    struct buddy* alloc = buddynew((void*) alignbase, alignsize, map->opts.minalign);
    if (!alloc)
        return false;

    void* region = mmap((void*) alignbase, alignsize, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
    if (region != (void*) alignbase) {
        free(alloc);
        g_err = "boxmap: cannot mmap";
        return false;
    }

    // To be safe we will reserve the start and end sandboxes of the region, in
    // case the region is directly adjacent to some sensitive data.
    buddy_reserve_range(alloc, (void*) alignbase, map->opts.guardsize);
    buddy_reserve_range(alloc, (void*) (alignbase + alignsize - map->opts.guardsize), map->opts.guardsize);

    map->regions[map->nregions++] = (struct AddrRegion) {
        .base = (void*) alignbase,
        .size = alignsize,
        .alloc = alloc,
    };

    return true;
}

bool boxmap_reserve(struct BoxMap* map, size_t size) {
    size_t total = size;
    size_t min = size;
    size_t totalgot = 0;

    if (size == 0) {
        total = tb(256);
        size = tb(255);
        min = gb(32);
    }
    size_t i_size = size;

    int i;
    for (i = 0; i < ADDR_REGION_MAX; i++) {
        void* base;
        size_t got = reserve(size, min, &base);
        if (!got)
            break;
        totalgot += got;
        total = total - got;
        size = total;
        if (!addregion(map, base, got))
            return false;
        if (totalgot >= i_size)
            break;
    }
    if (totalgot < i_size) {
        g_err = "boxmap: cannot mmap";
        return false;
    }
    return true;
}

static bool isfull(struct BoxMap* map) {
    for (size_t i = 0; i < map->nregions; i++) {
        if (!buddy_is_full(map->regions[i].alloc))
            return false;
    }
    return true;
}

// This function can only be called if the engine is not full.
static uintptr_t allocslot(struct BoxMap* map, size_t size) {
    for (size_t i = 0; i < map->nregions; i++) {
        if (!buddy_is_full(map->regions[i].alloc)) {
            map->regions[i].active++;
            return (uintptr_t) buddy_malloc(map->regions[i].alloc, size);
        }
    }
    assert(!"unreachable: engine was full");
}

static void deleteslot(struct BoxMap* map, uintptr_t base, size_t size) {
    for (size_t i = 0; i < map->nregions; i++) {
        uintptr_t vabase = (uintptr_t) map->regions[i].base;
        if (base >= vabase && base < vabase + map->regions[i].size) {
            int b = buddy_safe_free(map->regions[i].alloc, (void*) base, size);
            assert(b);
            map->regions[i].active--;
        }
    }
}

uintptr_t boxmap_addspace(struct BoxMap* map, size_t size) {
    if (isfull(map)) {
        g_err = "boxmap: no available slots";
        return 0;
    }

    return allocslot(map, size);
}

void boxmap_rmspace(struct BoxMap* map, uintptr_t space, size_t size) {
    deleteslot(map, space, size);
}