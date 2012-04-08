#ifndef _MRBFS_TYPES_H
#define _MRBFS_TYPES_H

#define MRBFS_MAX_PACKET_LEN 0x14
#define MRBFS_MAX_NODES 256

// Packet component defines
#define MRBUS_PKT_DEST  0
#define MRBUS_PKT_SRC   1
#define MRBUS_PKT_LEN   2
#define MRBUS_PKT_CRC_L 3
#define MRBUS_PKT_CRC_H 4
#define MRBUS_PKT_TYPE  5
#define MRBUS_PKT_DATA  6

#define MRBFS_VERSION "0.0.1"

#define MRBFS_INTERFACE_DRIVER_VERSION   0x01000001
#define MRBFS_NODE_DRIVER_VERSION        0x02000001

typedef unsigned long UINT32 ;
typedef unsigned char UINT8 ;

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
	char* key;
	char* value;
} MRBFSModuleOption;

typedef struct
{
	UINT8 bus;
	UINT8 len;
	UINT8 srcInterface;
	UINT8 pkt[MRBFS_MAX_PACKET_LEN];
} MRBusPacket;

#define MRBUS_PACKET_QUEUE_SIZE  32

typedef struct 
{
	pthread_mutex_t queueLock;
	int headIdx;
	int tailIdx;
	MRBusPacket pkts[MRBUS_PACKET_QUEUE_SIZE];
} MRBusPacketQueue;


// Note: Must be a power of 2
#define MRBFS_PACKET_LIST_SIZE 64

typedef struct
{
	time_t pktTime[MRBFS_PACKET_LIST_SIZE];
	MRBusPacket pkt[MRBFS_PACKET_LIST_SIZE];
	int headPtr;
} MRBFSFilePacketList;

typedef union
{
	char* valueStr;
	int valueInt;
	void* dirPtr;
} MRBFSFileNodeValue;

typedef enum
{
	FNODE_DIR             = 1,
	FNODE_DIR_NODE        = 2,	
	FNODE_RO_VALUE_STR    = 3,
	FNODE_RO_VALUE_INT    = 4,
	FNODE_RW_VALUE_STR    = 5,
	FNODE_RW_VALUE_INT    = 6,
	FNODE_RO_VALUE_PACKET_LIST = 7,
	FNODE_RW_VALUE_PACKET_LIST = 8,
	FNODE_END_OF_LIST
} MRBFSFileNodeType;

typedef struct MRBFSFileNode
{
	char* fileName;
	MRBFSFileNodeValue value;
	MRBFSFileNodeType fileType;
	time_t updateTime;
	time_t accessTime;
	void (*mrbfsFileNodeWrite)(struct MRBFSFileNode*, const char* data, int dataSz);
	void* nodeLocalStorage;
	struct MRBFSFileNode* childPtr;
	struct MRBFSFileNode* siblingPtr;
} MRBFSFileNode;


typedef struct MRBFSBusNode
{
	void* nodeDriverHandle;
	char* nodeName;
  	pthread_mutex_t nodeLock;
	UINT8 bus;
	UINT8 address;
	void* nodeLocalStorage;
	char* path;
	MRBFSFileNode* baseFileNode;
	int nodeOptions;
	MRBFSModuleOption* nodeOptionList;
	

	// Function pointers from main to the node module
	int (*mrbfsLogMessage)(mrbfsLogLevel, const char*, ...);
	MRBFSNode* (*mrbfsGetNode)(UINT8);
	MRBFSFileNode* (*mrbfsFilesystemAddFile)(const char* fileName, MRBFSFileNodeType fileType, const char* insertionPath);
	int (*mrbfsNodeTxPacket)(MRBusPacket* txPkt);

	// Function pointers from the node to main
	int (*mrbfsNodeInit)(struct MRBFSBusNode*);
	int (*mrbfsNodeRxPacket)(struct MRBFSBusNode* mrbfsNode, MRBusPacket* rxPkt);
	int (*mrbfsNodeDestroy)(struct MRBFSBusNode*);
	
} MRBFSBusNode;

typedef struct
{
	UINT8 bus;
	MRBFSBusNode* node[256];
  	pthread_mutex_t busLock;
} MRBFSBus;

#define MRBFS_MAX_INTERFACES   16

typedef struct MRBFSInterfaceDriver
{
	void* interfaceDriverHandle;
	pthread_t interfaceThread;
	char* interfaceName;
	char* port;
	UINT8 bus;
	UINT8 addr;
	UINT8 terminate;

	int interfaceOptions;
	MRBFSModuleOption* interfaceOptionList;

	char* path;
	MRBFSFileNode* baseFileNode;

	void* nodeLocalStorage;

	// Function pointers from main to the module
	int (*mrbfsLogMessage)(mrbfsLogLevel, const char*, ...);
	MRBFSFileNode* (*mrbfsFilesystemAddFile)(const char* fileName, MRBFSFileNodeType fileType, const char* insertionPath);
	void (*mrbfsPacketReceive)(MRBusPacket* rxPkt);
	
	// Function pointers from the module to main
	void (*mrbfsInterfaceDriverInit)(struct MRBFSInterfaceDriver* mrbfsInterfaceDriver);
	void (*mrbfsInterfaceDriverRun)(struct MRBFSInterfaceDriver* mrbfsInterfaceDriver);
	void (*mrbfsInterfacePacketTransmit)(struct MRBFSInterfaceDriver* mrbfsInterfaceDriver, MRBusPacket* txPkt);
	void* moduleLocalStorage;
	
} MRBFSInterfaceDriver;


typedef struct 
{
   mrbfsLogLevel logLevel;
	const char *configFileStr;
   cfg_t* cfgParms;
   FILE* logFile;
  	pthread_mutex_t logLock;
	MRBFSInterfaceDriver* mrbfsInterfaceDrivers[MRBFS_MAX_INTERFACES];
	UINT8 mrbfsUsedInterfaces;
	MRBFSBus* bus[256];
  	pthread_mutex_t masterLock;
	MRBFSFileNode* rootNode;
	pthread_mutex_t fsLock;
	
	MRBusPacketQueue rx;  // RX is the incoming queue, meaning from the interfaces to the filesystem
	MRBusPacketQueue tx;  // TX is the outgoing queue, meaning from the filesystem to the interfaces
	
} MRBFSConfig;


#endif

