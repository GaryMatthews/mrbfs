#ifndef _MRBFS_TYPES_H
#define _MRBFS_TYPES_H

#define MRBFS_MAX_PACKET_LEN 14
#define MRBFS_MAX_NODES 256

#define MRBFS_VERSION "0.0.1"

typedef enum
{
	MRBFS_LOG_ERROR   = 0,
	MRBFS_LOG_WARNING = 1,
	MRBFS_LOG_INFO    = 2,
	MRBFS_LOG_DEBUG   = 7
} mrbfsLogLevel;

typedef struct
{
	UINT8 mrbusBusNum;
	UINT8 mrbusAddress;
	int (*mrbfsNodePacket)(UINT8* mrbusPacket, UINT8 mrbusPacketLen);
} MRBFSNode;

typedef struct 
{
	int (*mrbfsLogMessage)(mrbfsLogLevel logLevel, const char* format, ...);
	UINT8 mrbusBusNum;
	UINT8 mrbusAddress;
	
	MRBFSNode* (*mrbfsGetNode)(UINT8 mrbusBusNum, 
	
	// Thing to get node to enqueue messages
	
	
} MRBFSInterfaceModule;

#endif

