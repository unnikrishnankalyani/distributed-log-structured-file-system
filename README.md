Distributed FileSystem
	- Adil Ahmed, Kalyani Unnikrishnan

This project implements a distributed filesystem that can be used by multiple clients. The implementation is split into two parts:
	1. A shared library (libmfs.so) that an application can use to interface with the server
	2. The filesystem implementation on the server

The client library allows a user to interact with the filesystem implemented in server.c. The client supports the following operations:
- `int MFS_Init(char *hostname, int port)`: Client takes in the hostname and port number for the hosted filesystem server and initializes it for all further calls.
- `int MFS_Write(int inum, char *buffer, int block)`: Write a data block to the filesystem
- `int MFS_Read(int inum, char *buffer, int block)`: Read a data block/ Directory entry from the filesystem. Directory Entry struct is defined in mfs.h
- `int MFS_Stat(int inum, MFS_Stat_t *m)`: Get File Info (struct defined in mfs.h)
- `int MFS_Lookup(int pinum, char *name)`: Get Inode number given filename and parent inode number
- `int MFS_Creat(int pinum, int type, char *name)`: Create a new file / directory
- `int MFS_Shutdown()`: Shuts down the server. Mostly used for testing purposes.

The code logic is simple. A global message struct (defined in lfs.h) is used for the request to and the reply from the server. When any of the above operations is called, the request struct is updated with the arguments and sent to the server. The reply struct is then received back with the query results.

The server takes a port number and filesystem image path as arguments. If the path doesn't exist, the server creates a new filesystem image. 
We then listen on the provided port number for incoming UDP messages which are of the form MFS_Msg_t. The incoming struct contains the operation to be performed (write, unlink, read, etc.), based on which the appropriate utility is called. The input and output are taken from or written to the global request and reply structs.

Compilation: A Makefile is provided in the directory. Run make to compile the shared library and server executable.

Server Usage:
prompt> server [portnum] [file-system-image]
