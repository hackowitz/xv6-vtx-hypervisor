#define NPROC        64  // maximum number of processes
#define KSTACKSIZE 4096  // size of per-process kernel stack
#define NCPU          8  // maximum number of CPUs
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes
#define LOGSIZE      (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         (MAXOPBLOCKS*3)  // size of disk block cache
// HW2: FSSIZE is the size of the disk that holds the root file system.
// mkfs builds an image of exactly FSSIZE blocks, and ide.c panics on a
// block number at or past it. The host disk grew from stock xv6's 1000
// blocks to 4000, so that it can also hold xv6kernel and xv6fs.img
// (each 1 MB or less); dot-bochsrc gives Bochs a disk of the same size.
// The #ifndef lets the Makefile build the guest kernel and mkfsguest
// with -DFSSIZE=$(GUESTFSSIZE), for the guest's smaller disk; both must
// agree with the image runxv6 serves.
#ifndef FSSIZE                      // the guest file system is smaller
#define FSSIZE       4000  // size of file system in blocks
#endif

