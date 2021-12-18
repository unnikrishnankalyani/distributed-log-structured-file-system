/*This is the server side implementation of a distributed filesystem. The server takes a port number and filesystem image path as arguments. If the path doesn't exist, the server creates a new filesystem image. 
 * We then listen on the provided port number for incoming UDP messages which are of the form MFS_Msg_t. The incoming struct contains the operation to be performed (write, unlink, read, etc.), based on which the appropriate utility is called. The input and output are taken from or written to the global request and reply structs */


#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "udp.h"
#include "mfs.h"
#include "lfs.h"

#define MAX_INODES (4096)

MFS_Msg_t request;	//Global request message (incoming from client)
MFS_Msg_t reply;	//Global reply message (outgoing from server)

int sd; 	//socket handler
int debug = 0;	

MFS_Checkpoint_t *cr; 		//checkpoint region
int imap_mem[MAX_INODES]; 	//in memory imap - max 4096 inodes
int fd; 			//descriptor for file system image file
char *file_image_path;		
int last_inum = -1;

//This function writes the updated checkpoint region from memory to disk. 
//fsync needs to be called from within the calling function to persist the changes.
void write_checkpoint(){
  lseek(fd, 0, SEEK_SET);
  write(fd, (void *)cr, sizeof(*cr));  // sizeof(*cr) = 1028
  if (debug) printf("writing cr\n");
}


//After an inode is updated, the inode number is passed to the function to write the updated imap piece to the end of the log
void write_update_imap_piece(int inum){
  //calculate relevant imap piece
  int imap_start = (int) inum/16;
  
  //write inode map piece of root directory
  if (debug) printf("writing imap piece corresponding to inum %d to %d\n", inum, cr->disk_pointer);
  lseek(fd, cr->disk_pointer, SEEK_SET);
  write(fd, (void *)(imap_mem + imap_start*16), 16*sizeof(int)); 

  cr->inode_map_pieces[imap_start] = cr->disk_pointer; //address of inode map
  cr->disk_pointer += 16*sizeof(int);
}

//Initialize directory entries
void fill_data_block_with_invalid_dir_entries(int start_of_block){
   while (cr->disk_pointer < start_of_block + MFS_BLOCK_SIZE){
    MFS_DirEnt_t *invalid = (MFS_DirEnt_t *) malloc(sizeof(MFS_DirEnt_t));
    invalid->inum = -1;
    write(fd, (void *)invalid, sizeof(*invalid)); 
    cr->disk_pointer += sizeof(*invalid);
  }
  
}

//Create a file given a parent directory inode number and a new inode number
void create_file(int parent_inode_num, int cur_inode_num){

  lseek(fd, cr->disk_pointer, SEEK_SET);
  
  //create inode of file
  MFS_Inode_t *file_inode = (MFS_Inode_t *) malloc(sizeof(MFS_Inode_t));
  file_inode->stat.type = MFS_REGULAR_FILE;
  file_inode->stat.size = 0;//4095 
  //Initialize all pointers to -1
  for(int i=0;i < 14 ;i++){
  	file_inode->direct_pointers[i] = -1; //do I need to set this?
  }
  //write inode of directory
  write(fd, (void *)file_inode, sizeof(*file_inode)); 

  //update cr disk pointer
  int file_inode_address = cr->disk_pointer;
  cr->disk_pointer += sizeof(*file_inode);

  //update dir inode address to imap_mem
  imap_mem[cur_inode_num] = file_inode_address; //inum root = 0

  //write out imap piece of root dir and update cr
  write_update_imap_piece(cur_inode_num);

  //overwrite cr
  write_checkpoint();
}


//Create a new directory given the parent directory inode number and new inode number
void create_dir(int parent_inode_num, int cur_inode_num){

  MFS_DirEnt_t *cur = (MFS_DirEnt_t *) malloc(sizeof(MFS_DirEnt_t));
  MFS_DirEnt_t *par = (MFS_DirEnt_t *) malloc(sizeof(MFS_DirEnt_t));

  strncpy(cur->name, ".", 28);
  cur->inum = cur_inode_num;
  strncpy(par->name, "..", 28);
  par->inum = parent_inode_num;

  //start_of_block = cr->disk_pointer
  int start_of_block = lseek(fd, cr->disk_pointer, SEEK_SET);

  write(fd, (void *)cur, sizeof(*cur));  // sizeof(*cur) = 32
  cr->disk_pointer +=sizeof(*cur);

  write(fd, (void *)par, sizeof(*par)); // sizeof(*par) = 32
  cr->disk_pointer += sizeof(*par);

  /*
    For directory entries that are not yet in use (in an allocated 4-KB directory block),
    the inode number should be set to -1. 
    This way, utilities can scan through the entries to check if they are valid.
    => 128 - 2(".","..") = 126 dir entries initialized to -1
  */
  fill_data_block_with_invalid_dir_entries(start_of_block);
  
  //create inode of directory
  MFS_Inode_t *dir_inode = (MFS_Inode_t *) malloc(sizeof(MFS_Inode_t));
  dir_inode->stat.type = MFS_DIRECTORY;
  dir_inode->stat.size = MFS_BLOCK_SIZE - 1;//4095 
  dir_inode->direct_pointers[0] = start_of_block;

  //write inode of directory
  write(fd, (void *)dir_inode, sizeof(*dir_inode)); 

  //update cr disk pointer
  int dir_inode_address = cr->disk_pointer;
  cr->disk_pointer += sizeof(*dir_inode);

  //update dir inode address to imap_mem
  imap_mem[cur_inode_num] = dir_inode_address; //inum root = 0

  //write out imap piece of root dir and update cr
  write_update_imap_piece(cur_inode_num);

  //overwrite cr
  write_checkpoint();
}

/* Write:
 *  writes a block of size 4096 bytes at the block offset specified by block. 
 *  Returns 0 on success, -1 on failure. 
 *  Failure modes: invalid inum, invalid block, not a regular file (because you can't write to directories).
*/

int fswrite(int inum, int block_num){
	char* buffer = malloc(MFS_BLOCK_SIZE);
	
	fd = open(file_image_path,O_RDWR);
	
	//copy buffer from message to pointer
	memcpy(buffer, &request.buffer, MFS_BLOCK_SIZE);
	//Check if inum and block_num are valid
	if(inum < 0 || inum > MAX_INODES - 1 || block_num < 0 || block_num > 13)
		return -1;
	//Get current inode address
	int inode_address = imap_mem[inum];
	//Get inode
	MFS_Inode_t *inode = (MFS_Inode_t *) malloc(sizeof(MFS_Inode_t));
	lseek(fd, inode_address, SEEK_SET);
	read(fd, inode, sizeof(*inode));
	if (inode->stat.type != MFS_REGULAR_FILE) {
    		printf("ERROR: HAS TO BE A REGULAR FILE, NOT A DIRECTORY \n");
    		close(fd);
    		return -1;
  	}
	//seek to end of log
        int start_of_block = lseek(fd, cr->disk_pointer, SEEK_SET);
	//Check if block is new or overwriting existing block and update inode size accordingly
	
	//Update inode
	inode->direct_pointers[block_num] = start_of_block;
	
	//Update inode stat size. (Last written block number) * block size. 
	//We assume that files with holes (only 1st and 12th block fileld, for example) include the gaps, for the purpose of computing size.
	inode->stat.size = 0;
	for(int i=13;i>=0 ;i--){
		if(inode->direct_pointers[i] != -1){
			inode->stat.size = (i+1) *  MFS_BLOCK_SIZE;
			break;
		}
	}
		
	//Write data
	write(fd, (void *) buffer, MFS_BLOCK_SIZE);
	//Update pointer
	cr->disk_pointer += MFS_BLOCK_SIZE;
	//Write inode
	inode_address = lseek(fd, cr->disk_pointer, SEEK_SET);
	write(fd, inode, sizeof(*inode));
	//Update inode address in memory
	imap_mem[inum] = inode_address;
	//Update cr pointer again
	cr->disk_pointer += sizeof(*inode);

	//Write the updated imap piece to disk
	write_update_imap_piece(inum);

  	//overwrite cr
  	write_checkpoint();

	fsync(fd);
	close(fd);
	free(inode);
	free(buffer);
	return 0;
}

/*Read:
 * reads a block specified by block into the buffer from file specified by inum. 
 * The routine should work for either a file or directory; 
 * directories should return data in the format specified by MFS_DirEnt_t. 
 * Success: 0, failure: -1. Failure modes: invalid inum, invalid block.*/

int fsread(int inum, int block_num){
	void* buffer = malloc(MFS_BLOCK_SIZE);
	//MFS_DirEnt_t dirent[128];
  fd = open(file_image_path,O_RDWR);

  //Check if inum and block_num are valid
  if(inum < 0 || inum > MAX_INODES - 1 || block_num < 0 || block_num > 13)
          return -1;
  //Get inode address
  int inode_address = imap_mem[inum];
  //Get inode
  MFS_Inode_t *inode = (MFS_Inode_t *) malloc(sizeof(MFS_Inode_t));
  lseek(fd, inode_address, SEEK_SET);
  read(fd, inode, sizeof(*inode));
       	
	//Update global stat
	reply.info = inode->stat;
  //Get address of data block
	int data_address = inode->direct_pointers[block_num];	
	
	//seek to start of data
	lseek(fd, data_address, SEEK_SET);
	//read data
	if(inode->stat.type == MFS_DIRECTORY){
		read(fd, reply.dir_info, MFS_BLOCK_SIZE);
		//reply.dir_info = dirent;
	}
	else{

		read(fd, buffer, MFS_BLOCK_SIZE);

		//Update global reply variable
		memcpy(reply.buffer, buffer, MFS_BLOCK_SIZE);
	}
	//free(dirent);
	free(buffer);
	free(inode);
	close(fd);
	return 0;

}


/* Stat:
 * Returns some information about the file specified by inum. 
 * Upon success, return 0, otherwise -1. 
 * The exact info returned is defined by MFS_Stat_t. 
 * Failure modes: inum does not exist.*/

int fsstat(int inum){

	fd = open(file_image_path,O_RDWR);
	//Check if inum and block_num are valid
  if(inum < 0 || inum > MAX_INODES - 1)
          return -1;
        
	//Get inode address
  int inode_address = imap_mem[inum];
        
	//Get inode
  MFS_Inode_t *inode = (MFS_Inode_t *) malloc(sizeof(MFS_Inode_t));
  lseek(fd, inode_address, SEEK_SET);
  read(fd, inode, sizeof(*inode));
	
	//Set global stat variable to that of inode
	reply.info = inode->stat;
	free(inode);
	close(fd);
	return 0;
}

/* Lookup:
  -takes the parent inode number (which should be the inode number of a directory)
  -looks up the entry name in it
  
  Success: return inode number of name; failure: return -1. 
  Failure modes: invalid pinum, name does not exist in pinum.
*/

int fslookup(int pinum, char name[]){
  //check if pinum is valid
  if(pinum < 0 || pinum>MAX_INODES-1) return -1;

  int inode_address = imap_mem[pinum];
  if(inode_address==-1) return -1; //pinum does not exist

  fd = open(file_image_path,O_RDWR);
  if (debug) printf("\nparent inode_address: %d\n", inode_address);
  lseek(fd, inode_address, SEEK_SET);

  MFS_Inode_t *inode = (MFS_Inode_t *) malloc(sizeof(MFS_Inode_t));

  read(fd,inode,sizeof(*inode));

  if (inode->stat.type != MFS_DIRECTORY) {
    printf("ERROR: HAS TO BE A DIRECTORY \n");
    close(fd);
    return -1;
  }
  if (debug) printf("calc blocks %d\n",inode->stat.size);
  for (int i=0; i<(inode->stat.size+1)/MFS_BLOCK_SIZE; i++){
    //go to dir entry i
    lseek(fd, inode->direct_pointers[i], SEEK_SET);

    MFS_DirEnt_t *buf = (MFS_DirEnt_t*) malloc(sizeof(MFS_DirEnt_t));
    for (int j=0;j<128;j++){
      //read contents into buf
      read(fd,buf,32);
      if (strcmp(buf->name, name)==0) {
        close(fd);
	//Update global reply variable
	reply.inum = buf->inum;
        return buf->inum; //found inum
      }
    }
  }
  close(fd);
  return -1; //name does not exist in pinum
}

/*
Create:
  - type == MFS_REGULAR_FILE or MFS_DIRECTORY
  - pinum: parent directory 
  - name: name of DIR/FILE
Returns 0 on success, -1 on failure. 
Failure modes: pinum does not exist, or name is too long. 
If name already exists, return success 
*/

int fscreat(int pinum, int type, char name[]){

  //check if pinum is valid
  if(pinum < 0 || pinum>MAX_INODES-1) return -1;

  if(sizeof(*name)>28) return -1; //name is too long

  //get the inode of the parent directory
  int inode_address = imap_mem[pinum];
  if(inode_address==-1) return -1; //pinum does not exist

  fd = open(file_image_path,O_RDWR);
  lseek(fd, inode_address, SEEK_SET);

  MFS_Inode_t *pinode = (MFS_Inode_t *) malloc(sizeof(MFS_Inode_t));

  read(fd,pinode,sizeof(*pinode));

  if (pinode->stat.type != MFS_DIRECTORY) {
    printf("ERROR: HAS TO BE A DIRECTORY \n");
    close(fd);
    return -1;
  }

//Iterate through the directory entries of the parent inode to create an entry for the file/dir
  for (int i=0; i<(pinode->stat.size+1)/MFS_BLOCK_SIZE; i++){
    //go to dir entry i
    int cur_address = lseek(fd, pinode->direct_pointers[i], SEEK_SET);
    MFS_DirEnt_t *buf = (MFS_DirEnt_t*) malloc(sizeof(MFS_DirEnt_t));

    /*
    Iterate through directory entries to see if there is an invalid entry or if the filename/dirname already exists
    In case of invalid entry, (inum = -1), use this space to enter the new details
    If the name already exists, return success 
    */
    for (int j=0;j<128;j++){
      //read contents into buf
      read(fd,buf,32);
      
      if (buf->inum==-1) {
        //found an invalid entry, use this space for the new file/dir
        buf->inum = ++last_inum;
        //if inum is more than MAX_INODES, iterate through the imap to find an inum which points to an invalid address (previously unlinked) to be used for the new file/dir
        if(buf->inum >= MAX_INODES){
          for (int i=0; i<MAX_INODES; i++){
            if(imap_mem[i]==-1){
              if (debug) printf("inode set to %d\n ", i );
              buf->inum = i;
              break;
            }
          }
        }

        //write (new file/dir name, inum) to directory entry
        strcpy(buf->name,name);
        lseek(fd, cur_address, SEEK_SET);
        write(fd, (void *)buf, sizeof(*buf)); 
        
        //Create file/dir as per type
        if(type == MFS_DIRECTORY) create_dir(pinum, buf->inum);
        else create_file(pinum, buf->inum);
        fsync(fd);
        close(fd);
        return 0; 
      }
      if (strcmp(buf->name, name)==0) {
        //file/dir already exists, return success
        if(debug) printf("file/dir already exists: %s\n", buf->name);
        close(fd);
        return 0; 
      }
      cur_address+=32;
    }
  }
  
  /*
  Did not find an invalid entry and filename/dirname does not exist already.
  All directory entries are full, create new data block to make a new entry 
  */

  //check if max directory size is reached, return failure if so
  if(pinode->stat.size == MFS_BLOCK_SIZE*14 - 1){
    if (debug) printf("max directory size reached");
    return -1;
  }
  
  //create a new directory entry
  int start_of_block = lseek(fd, cr->disk_pointer, SEEK_SET);
  MFS_DirEnt_t *cur = (MFS_DirEnt_t *) malloc(sizeof(MFS_DirEnt_t));
  strncpy(cur->name, name, 28);
  cur->inum = ++last_inum;
  //if inum is more than MAX_INODES, iterate through the imap to find an inum which points to an invalid address (previously unlinked) to be used for the new file/dir
  if(cur->inum >= MAX_INODES){
    for (int i=0; i<MAX_INODES; i++){
      if(imap_mem[i]==-1){
        cur->inum = i;
        break;
      }
    }
  }

  //write this directory entry and fill the remaining datablocks with invalid entries
  write(fd, (void *)cur, sizeof(*cur));  
  cr->disk_pointer +=sizeof(*cur);
  fill_data_block_with_invalid_dir_entries(start_of_block);

  //update the parent node stats since a new data block is created now
  pinode->stat.size += MFS_BLOCK_SIZE;
  pinode->direct_pointers[pinode->stat.size/MFS_BLOCK_SIZE] = start_of_block;
  
  //write out pinode since it's changed
  write(fd, (void *)pinode, sizeof(*pinode)); 

  //update dir inode address to imap_mem
  imap_mem[pinum] = cr->disk_pointer; //inum root = 0
  cr->disk_pointer +=sizeof(*pinode);

  //write out imap piece of root dir and update cr
  write_update_imap_piece(pinum);

  //Not writing cr here because it will be written after creating dir/file
  
  if(type == MFS_DIRECTORY) create_dir(pinum, cur->inum);
  else create_file(pinum, cur->inum);
   
  fsync(fd);
  close(fd);
  return 0;
}

/*
Unlink:
 - removes the file or directory name from the directory specified by pinum. 
 0 on success, -1 on failure. 
 Failure modes: pinum does not exist, directory is NOT empty. 
 Note that the name not existing is NOT a failure by our definition (think about why this might be).
*/

int fsunlink(int pinum, char name[]){
  //check if pinum is valid
  if(pinum < 0 || pinum>MAX_INODES-1) return -1;

  int pinode_address = imap_mem[pinum];
  if(pinode_address==-1) return -1; //pinum does not exist
  
  fd = open(file_image_path,O_RDWR);
  lseek(fd, pinode_address, SEEK_SET);
  
  //get the inode of the parent directory
  MFS_Inode_t *pinode = (MFS_Inode_t *) malloc(sizeof(MFS_Inode_t));
  read(fd,pinode,sizeof(*pinode));

  if (pinode->stat.type != MFS_DIRECTORY) {
    printf("ERROR: HAS TO BE A DIRECTORY \n");
    close(fd);
    return -1;
  }

  /*
  Iterate through directory entries to search for name to be unlinked 
  If the name does not exist, return success (nothing to unlink)
  */
  for (int i=0; i<(pinode->stat.size+1)/MFS_BLOCK_SIZE; i++){
    //go to dir entry i
    int cur_address = lseek(fd, pinode->direct_pointers[i], SEEK_SET);
    MFS_DirEnt_t *buf = (MFS_DirEnt_t*) malloc(sizeof(MFS_DirEnt_t));

    for (int j=0;j<128;j++){
      //read contents into buf
      read(fd,buf,32);
      if (buf->inum==-1) {
        //name does not exist 
        close(fd);
        return 0; 
      }
      if (strcmp(buf->name, name)==0) {
        /*
        found the file/dir
        If it is a directory, need to check that it is empty before deleting
        (check for Failure mode: directory is NOT empty.)
        Read in the inode to check for type == MFS_DIRECTORY
        */
        int inode_address = imap_mem[buf->inum];
        lseek(fd, inode_address, SEEK_SET);
        MFS_Inode_t *inode = (MFS_Inode_t *) malloc(sizeof(MFS_Inode_t));
        read(fd,inode,sizeof(*inode));
        
        //If it is a directory, verify that it is empty
        if(inode->stat.type == MFS_DIRECTORY){
          //need to check every directory entry because entries could have been deleted from between
          for (int i=0; i<(inode->stat.size+1)/MFS_BLOCK_SIZE; i++){
            //go to dir entry i
            lseek(fd, inode->direct_pointers[i], SEEK_SET);
            MFS_DirEnt_t *buf_d = (MFS_DirEnt_t*) malloc(sizeof(MFS_DirEnt_t));
            for (int j=0;j<128;j++){
              //read contents into buf
              read(fd,buf_d,32);
              if (buf_d->inum!=-1 && strcmp(buf_d->name,".")!=0 && strcmp(buf_d->name,"..")!=0) {
                //Directory is not empty. Cannot unlink, return Failure
                if (debug) printf("Directory you are trying to delete is not empty, it contains file/dir %d : %s\n", buf_d->inum, buf_d->name);
                return -1; 
              }
            }
          }
        }

        //Now we know that it is safe to unlink this name
        //Update inode address in memory
        int inum_to_del = buf->inum;
        imap_mem[inum_to_del] = -1;
        
        //Update the directory entry
        buf->inum = -1;
        strcpy(buf->name,""); //doesn't matter as we always check buf inum == -1 first. Setting just to be safe.
        lseek(fd, cur_address, SEEK_SET);
        write(fd, (void *)buf, sizeof(*buf)); 

        lseek(fd, 0, SEEK_END);
        //Write the updated imap piece to disk
        write_update_imap_piece(inum_to_del);

        //overwrite cr
        write_checkpoint();
        
        fsync(fd);
        close(fd);
        return 0; //successfully removed
      }
    cur_address+=32;
    }
  }

  close(fd);
  return 0; //name not existing is NOT a failure
}

int init(){

  //init fd
  fd = open(file_image_path,O_RDWR);

  //initialize cr 
  cr = (MFS_Checkpoint_t *) malloc(sizeof(MFS_Checkpoint_t));
  cr->disk_pointer = (int) sizeof(*cr);
  
  //initialize imap_mem
  for(int i=0;i<MAX_INODES;i++){
      imap_mem[i]=-1;
  }

  if(fd < 0){

    //create file with permissions for all users to read and write to file
    fd = creat(file_image_path,O_RDWR | S_IROTH | S_IRGRP | S_IRUSR | S_IWGRP | S_IWOTH | S_IWUSR);

    //init cr
    for(int i=0;i<256;i++){
      cr->inode_map_pieces[i]=-1;
    }

    //create root dir and write checkpoint
    create_dir(0,0);
    last_inum = 0;

    //initialize inode map
  } else {
    //load from image
    read(fd,cr,sizeof(*cr));
    
    for(int i=0; i<256; i++){
      int imap = cr->inode_map_pieces[i];
      if (imap!=-1){
        lseek(fd, imap, SEEK_SET);
        int inode_map[16];
        read(fd,inode_map,16*sizeof(int));
        for(int j=0; j<16;j++){
          int inode_address = inode_map[j];
          if (inode_address!=-1){
            imap_mem[i*16+j]  = inode_address;
            last_inum = i*16+j;
          }
        }
      }
    }
  }
  fsync(fd);
  close(fd);
  return 0;
}

int send_reply(struct sockaddr_in addr){
        int rc = UDP_Write(sd, &addr, (char*)&reply, sizeof(MFS_Msg_t));
        if (rc < 0) {
                if (debug) printf("server:: failed to send\n");
                exit(1);
        }
        return 0;
}

int main(int argc, char *argv[]){
	if(argc < 2){
            printf("Usage: server [portnum] [file-system-image]\n");
            exit(1);
    }
    int server_port = atoi(argv[1]);
    do{
        sd = UDP_Open(server_port);
        server_port++;
    } while (sd <= -1);
    if (debug) printf("Server launched on port %d\n",server_port-1);

    file_image_path = strdup(argv[2]);
    init(file_image_path);
 while (1) {
        struct sockaddr_in addr;
        int rc = UDP_Read(sd, &addr,(char*)&request, sizeof(MFS_Msg_t));
	reply.status = SUCCESS; //set default reply status as successful
        if (rc > 0) {
            if(request.operation == LOOKUP){
                    if (debug) printf("Operation: LOOKUP. Parent Inode Number: %d, filename = %s\n",request.pinum, request.name);
                    if(fslookup(request.pinum,request.name) == -1){
			    reply.status = FAILURE;
		    }
            }
            if(request.operation == STAT){
                    if (debug) printf("Operation: STAT. Inode Number: %d\n",request.inum);
                    if(fsstat(request.inum) == -1){
			    reply.status = FAILURE;
		    }
            }
            if(request.operation == WRITE){
                    if (debug) printf("Operation: WRITE. Inode Number: %d, Block Number = %d\n",request.inum, request.block);
                    if(fswrite(request.inum, request.block) == -1){
			    reply.status = FAILURE;
		    }
            }
            if(request.operation == READ){
                    if (debug) printf("Operation: READ. Inode Number: %d, Block Number = %d\n",request.inum, request.block);
                    if(fsread(request.inum, request.block) == -1){
			    reply.status = FAILURE;
		    }
            }
            if(request.operation == CREATE){
                    if (debug) printf("Operation: CREATE. Parent Inode Number: %d, Type: %d, filename = %s\n",request.pinum, request.type, request.name);
                    if(fscreat(request.pinum, request.type, request.name) == -1){
			    reply.status = FAILURE;
		    }
            }
            if(request.operation == UNLINK){
                    if (debug) printf("Operation: UNLINK. Parent Inode Number: %d, filename = %s\n",request.pinum, request.name);
                    if(fsunlink(request.pinum, request.name) == -1){
			    reply.status = FAILURE;
		    }
            }
            if(request.operation == SHUTDOWN){
                    if (debug) printf("Operation: SHUTDOWN\n");
		    exit(0);
            }
            send_reply(addr);
      }
    }
    return 0;
}


