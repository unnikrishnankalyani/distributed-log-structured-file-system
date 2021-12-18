/* This is the client library source code for libmfs.so. The client library allows a user to interact with the filesystem implemented in server.c. The client supports the following operations:
 * MFS_Init: Client takes in the hostname and port number for the hosted filesystem server and initializes it for all further calls.
 * MFS_Write: Write a data block to the filesystem
 * MFS_Read: Read a data block/ Directory entry from the filesystem. Directory Entry struct is defined in mfs.h
 * MFS_Stat: Get File Info (struct defined in mfs.h
 * MFS_Lookup: Get Inode number given filename and parent inode number
 * MFS_Creat: Create a new file / directory
 * MFS_Shutdown: Shuts down the server. Mostly used for testing purposes.
 *
 * The code logic is simple. A global message struct (defined in lfs.h) is used for the request to and the reply from the server. When any of the above operations is called, the request struct is updated with the arguments and sent to the server. The reply struct is then received back with the query results. */


#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "udp.h"
#include "mfs.h"
#include "lfs.h"
#include <sys/select.h>

char* server_host; //Hostname of the server
int server_port;   //Server Port
int sd; 	   //socket handle

MFS_Msg_t request; //Message sent to server
MFS_Msg_t reply;   //Message received from server

//Uses the library defined in udp.h to send and receive UDP messages. The select interface is used to retry calls when a response isn't received
int send_request(){
	fd_set rfds;
	struct timeval tv;
	int retval;
	struct sockaddr_in addrSnd, addrRcv;
   	UDP_FillSockAddr(&addrSnd, server_host, server_port);
	//Repeat UDP write until a response is received
	do{
		FD_ZERO(&rfds);
        	FD_SET(sd, &rfds);
		UDP_Write(sd, &addrSnd, (char*)&request, sizeof(MFS_Msg_t));
		//No response will be sent for a shutdown operation.
		if(request.operation == SHUTDOWN){
                	return 0;
        	}
		tv.tv_sec = 5; //timeout period
		tv.tv_usec = 0;
		retval = select(8,&rfds,NULL,NULL,&tv); //block on select until ready to read. After 5 seconds, try sending again
       		if(retval){	
			UDP_Read(sd, &addrRcv, (char*)&reply, sizeof(MFS_Msg_t));
			break;
		}
	} while(retval < 1);	
	return 0;
}

//Opens a port for sending and receiving messages. Sets global values of server host and port.
int MFS_Init(char *hostname, int port){
	server_host = malloc(100);
	strcpy(server_host, hostname);
	server_port = port;
	int client_port = 3000;
	do{
		sd = UDP_Open(client_port);
		client_port++;
	} while(sd <= -1);
	return 0;
}

//Update parent inode, filename and operation in request and fetch inode number returned in reply
int MFS_Lookup(int pinum, char *name){
	request.pinum = pinum;
	memcpy(request.name, name, sizeof(request.name));
	request.operation = LOOKUP;
	
	send_request();
	if(reply.status == SUCCESS){
		return reply.inum;
	}
	else
		return -1;
}


int MFS_Stat(int inum, MFS_Stat_t *m){
	request.inum = inum;
 	request.operation = STAT;
	send_request();
	if(reply.status == SUCCESS){
		*m = reply.info;
	}
	else
		return	-1;
	
	return 0;
}

int MFS_Write(int inum, char *buffer, int block){
	buffer = (void *) buffer;
	request.inum = inum;
	memcpy(request.buffer, buffer, MFS_BLOCK_SIZE);
	request.block = block;
	request.operation = WRITE;
	send_request();
	if(reply.status == SUCCESS){
		return 0;
	}
	return -1;
}

int MFS_Read(int inum, char *buffer, int block){
	request.inum = inum;

	memcpy(request.buffer, buffer, MFS_BLOCK_SIZE);
	request.block = block;
	request.operation = READ;
	send_request();
	if(reply.status == SUCCESS){
		if (reply.info.type == MFS_DIRECTORY){
			memcpy(buffer, &reply.dir_info, MFS_BLOCK_SIZE);
		}
		else{
			memcpy(buffer, &reply.buffer, MFS_BLOCK_SIZE);
		}
		return 0;
	}
	else return -1;
}

int MFS_Creat(int pinum, int type, char *name){
	//handle case when name is too long
	if(strlen(name) > 28){
		return -1;
	}
	request.pinum = pinum;
	request.type = type;
	memcpy(request.name, name, sizeof(request.name));
	request.operation = CREATE;
	send_request();
	if(reply.status == SUCCESS){
		return 0;
	}
	else return -1;
}

int MFS_Unlink(int pinum, char *name){
	request.pinum = pinum;
	memcpy(request.name, name, sizeof(request.name));
	request.operation = UNLINK;
	send_request();
	if(reply.status == SUCCESS){
		return 0;
	}
	else return -1;

}

int MFS_Shutdown(){
	request.operation = SHUTDOWN;
	send_request();
	return 0;
}


