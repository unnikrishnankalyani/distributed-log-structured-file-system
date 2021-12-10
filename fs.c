#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

/* Inode should contain:
    - a size field (the number of the last byte in the file)
    - a type field (regular or directory)
    - 14 direct pointers; thus, the maximum file size is 14 times the 4KB block size, or 56 KB.
*/
typedef struct inode{
  int size;
  char *type;
  int *direct_pointers[14];
} inode;

    
/* The inode map is just an array, indexed by inode number. 
Each entry is a simple 4-byte integer, which is just the disk address of the location of the inode in question.
*/
typedef struct inode_map{
  struct *inode[16];
} inode_map;


/* The checkpoint region should contain:
 -  a disk pointer to the current end of the log; 
 -  pointers to pieces of the inode map 

Assume there are a maximum of 4096 inodes; assume each piece of the inode map has 16 entries
=> 256 pieces of inode map.
*/

typedef struct checkpoint{
  int *disk_pointer;
  struct *inode_map[256]; //
} checkpoint;


int main(int argc, char *argv[]){
    return 0;
}
