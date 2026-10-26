// On-disk file system format.
// Both the kernel and user programs use this header file.


#define ROOTINO 1  // root i-number
#define BSIZE 512  // block size

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
  uint nlog;         // Number of log blocks
  uint logstart;     // Block number of first log block
  uint inodestart;   // Block number of first inode block
  uint bmapstart;    // Block number of first free map block
};

// Block addresses in an inode (HW2): NDIRECT direct blocks, then one
// doubly-indirect block. That block lists NINDIRECT indirect blocks,
// each of which lists NINDIRECT data blocks, so a file can hold a whole
// guest kernel or disk image (up to about 8 MB). Stock xv6 had a
// singly-indirect block in the same slot, which limited a file to
// 12 + 128 blocks (70 KB). See figures/fs-layout.svg.
//
// NDIRECT stays 12, and addrs[NDIRECT] is still the only pointer to
// more blocks, so struct dinode keeps its 13 addresses and its 64
// bytes: 8 inodes still fit exactly in a 512-byte block (IPB below,
// and mkfs asserts that BSIZE is a multiple of the inode size), and the
// in-memory struct inode (file.h) is unchanged. The cost is that a file
// just past 12 blocks needs two pointer blocks (the doubly-indirect one
// and its first indirect one) instead of one.
#define NDIRECT 12
// Block addresses per indirect block: 512 / 4 = 128.
#define NINDIRECT (BSIZE / sizeof(uint))
// Data blocks reached through the doubly-indirect block: 128 * 128.
#define NDINDIRECT (NINDIRECT * NINDIRECT)
// Largest file in blocks: 12 + 16384 = 16396 blocks, 8,394,752 bytes.
#define MAXFILE (NDIRECT + NDINDIRECT)

// On-disk inode structure
struct dinode {
  short type;           // File type
  short major;          // Major device number (T_DEV only)
  short minor;          // Minor device number (T_DEV only)
  short nlink;          // Number of links to inode in file system
  uint size;            // Size of file (bytes)
  uint addrs[NDIRECT+1];   // Data block addresses
};

// Inodes per block.
#define IPB           (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB           (BSIZE*8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) (b/BPB + sb.bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};

