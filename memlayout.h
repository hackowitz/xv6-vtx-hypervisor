// Memory layout

#define EXTMEM  0x100000            // Start of extended memory
// HW2: xv6 assumes it has physical memory up to PHYSTOP (224 MB here)
// and hands all of it to kalloc(). A guest has only what runxv6 gives
// it (GUESTMEM, 32 MB), and the EPT maps nothing above that, so the
// guest kernel is built with -DPHYSTOP=0x2000000 (Makefile GUESTDEFS);
// otherwise its kinit would touch unmapped guest-physical memory and
// cause an EPT violation. The #ifndef lets the -D value win.
#ifndef PHYSTOP                     // the guest kernel is built smaller
#define PHYSTOP 0xE000000           // Top physical memory
#endif
#define DEVSPACE 0xFE000000         // Other devices are at high addresses

// Key addresses for address space layout (see kmap in vm.c for layout)
#define KERNBASE 0x80000000000      // First kernel virtual address
#define DEVBASE  0xFFFFFFFFFE000000 // First device virtual address
#define KERNLINK (KERNBASE+EXTMEM)  // Address where kernel is linked

#define V2P(a) (((uint64) (a)) - KERNBASE)
#define P2V(a) ((void *)(((char *) (a)) + KERNBASE))
#define DEV_P2V(a) ((void *)(((char *) (a)) + DEVBASE - DEVSPACE))
#define DEV_V2P(a) (((uint64) (a)) - DEVBASE + DEVSPACE)

#define V2P_WO(x) ((x) - KERNBASE)    // same as V2P, but without casts
#define P2V_WO(x) ((x) + KERNBASE)    // same as P2V, but without casts
