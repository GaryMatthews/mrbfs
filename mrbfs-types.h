#ifndef _MRBFS_TYPES_H
#define _MRBFS_TYPES_H

#define MRBFS_MAX_PACKET_LEN 14
#define MRBFS_MAX_NODES 256

#define MRBFS_VERSION "0.0.1"

#define MRBFS_INTERFACE_MODULE_VERSION   0x01000001
#define MRBFS_NODE_MODULE_VERSION        0x02000001

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
	UINT8 stuff; // This is a placeholder
	
} MRBFSNodeModule;


typedef union
{
	char* valueStr;
	int valueInt;
	void* dirPtr;

} MRBFSFileNodeValue;

typedef enum
{
	FNODE_DIR,
	FNODE_DIR_NODE,	
	FNODE_RO_VALUE_STR,
	FNODE_RO_VALUE_INT,
	FNODE_RW_VALUE_STR,
	FNODE_RW_VALUE_INT,	
	FNODE_END_OF_LIST
} MRBFSFileNodeType;

typedef struct MRBFSFileNode
{
	char* fileName;
	MRBFSFileNodeValue value;
	MRBFSFileNodeType fileType;
	time_t updateTime;
	time_t accessTime;
	struct MRBFSFileNode* childPtr;
	struct MRBFSFileNode* siblingPtr;
} MRBFSFileNode;


typedef struct
{
	time_t updateTime;
	UINT8 address;
	char* name;
	MRBFSNodeModule nodeModule;
	void* moduleLocalStorage;	
  	pthread_mutex_t nodeLock;	
} MRBFSBusNode;

typedef struct
{
	UINT8 bus;
	MRBFSBusNode* node[256];
  	pthread_mutex_t busLock;
} MRBFSBus;

typedef struct 
{
	int (*mrbfsLogMessage)(mrbfsLogLevel logLevel, const char* format, ...);
	UINT8 bus;
	UINT8 address;
	
	MRBFSNode** (*mrbfsGetAffectedNodeList)(UINT8 bus, UINT8 srcAddress);
	
	// Thing to get node to enqueue messages
	
	
} MRBFSInterfaceContext;

#define MRBFS_MAX_INTERFACES   16

typedef struct MRBFSInterfaceModule
{
	void* interfaceDriverHandle;
	pthread_t interfaceThread;
	const char* interfaceName;
	const char* port;
	UINT8 bus;
	UINT8 addr;
	UINT8 terminate;
	
	// Function pointers from main to the module
	int (*mrbfsLogMessage)(mrbfsLogLevel, const char*, ...);
	MRBFSNode* (*mrbfsGetNode)(UINT8);
	
	// Function pointers from the module to main
	void (*mrbfsInterfaceModuleRun)(struct MRBFSInterfaceModule* mrbfsInterfaceModule);
	
	void* moduleLocalStorage;
	
} MRBFSInterfaceModule;


typedef struct 
{
   mrbfsLogLevel logLevel;
	const char *configFileStr;
   cfg_t* cfgParms;
   FILE* logFile;
  	pthread_mutex_t logLock;
	MRBFSInterfaceModule* mrbfsInterfaceModules[MRBFS_MAX_INTERFACES];
	UINT8 mrbfsUsedInterfaces;
	MRBFSBus* bus[256];
  	pthread_mutex_t masterLock;
	
	MRBFSFileNode* rootNode;
	pthread_mutex_t fsLock;
	
} MRBFSConfig;


#endif

