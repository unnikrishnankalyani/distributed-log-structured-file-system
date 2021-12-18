#ifndef __LFS_h__
#define __LFS_h__
#include "mfs.h"

enum fsoperation { INIT, LOOKUP, STAT, WRITE, READ, CREATE, UNLINK, SHUTDOWN }; //define what operation server should perform

enum fsstatus { SUCCESS, FAILURE };

typedef struct __MFS_Msg_t{
        int inum;        //inode number
        char buffer[4096];       //data location
        MFS_DirEnt_t dir_info[128]; //directory entry info
	int block;       //block number
        int type;        //Directory (0) or Regular File (1)
        int pinum;       //Parent inode number
        char name[28];   //file or directory name
        enum fsoperation operation;
        enum fsstatus status;
        MFS_Stat_t info;
} MFS_Msg_t;

/* 
Inode should contain:
    - a size field (the number of the last byte in the file)
    - a type field (regular or directory)
    - 14 direct pointers; thus, the maximum file size is 14 times the 4KB block size, or 56 KB.
*/
typedef struct __MFS_Inode_t{
  MFS_Stat_t stat;
  int direct_pointers[14];
} MFS_Inode_t;

/*
The checkpoint region should contain:
 -  a disk pointer to the current end of the log;
 -  pointers to pieces of the inode map

Assume there are a maximum of 4096 inodes; assume each piece of the inode map has 16 entries
=> 256 pieces of inode map.
*/

typedef struct __MFS_Checkpoint_t{
  int disk_pointer;
  int inode_map_pieces[256];
} MFS_Checkpoint_t;




#endif // __LFS_h__
