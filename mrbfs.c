#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <confuse.h>
#include "mrbfs.h"
#include "mrbfs-log.h"
#include "mrbfs-filesys.h"
#include "mrbfs-cfg.h"


// Globals
MRBFSConfig* gMrbfsConfig = NULL;

static void* mrbfsInit(struct fuse_conn_info *conn)
{
	int err;

}

void mrbfsInterfacesDestroy()
{
	int i;
	// Give the interfaces the chance to terminate gracefully
	for(i = 0; i < gMrbfsConfig->mrbfsUsedInterfaces; i++)
	{
		gMrbfsConfig->mrbfsInterfaceDrivers[i]->terminate = 1;
	}
	
	sleep(1);
	
	// Now just kill whatever's left
	for(i = 0; i < gMrbfsConfig->mrbfsUsedInterfaces; i++)
	{
		pthread_cancel(gMrbfsConfig->mrbfsInterfaceDrivers[i]->interfaceThread);
		// Deallocate its storate
		free(gMrbfsConfig->mrbfsInterfaceDrivers[i]->interfaceName);
		free(gMrbfsConfig->mrbfsInterfaceDrivers[i]->port);
		free(gMrbfsConfig->mrbfsInterfaceDrivers[i]);
	}
	
}

static void mrbfsDestroy(void* v)
{
	mrbfsFilesystemDestroy();
	mrbfsInterfacesDestroy();
   cfg_free(gMrbfsConfig->cfgParms);	
}


static struct fuse_operations mrbfsOperations = 
{
	.getattr	= mrbfsGetattr,
	.readdir	= mrbfsReaddir,
	.open		= mrbfsOpen,
	.read		= mrbfsRead,
	.init    = mrbfsInit,
	.destroy = mrbfsDestroy,
};

static int mrbfs_opt_proc(void *data, const char *arg, int key, struct fuse_args *outargs)
{
     switch (key) 
     {
		  case KEY_HELP:
		          fprintf(stderr,
		                  "usage: %s mountpoint [options]\n"
		                  "\n"
		                  "general options:\n"
		                  "    -o opt,[opt...]  mount options\n"
		                  "    -h   --help      print help\n"
		                  "    -V   --version   print version\n"
		                  "\n"
		                  "MRBFS options:\n"
		                  "    -d <DEBUG LEVEL 0-2, 0=errors only, 1=warnings, 2=debug\n"
		                  "    -c <CONFIG FILE LOCATION>mybool=BOOL    same as 'mybool' or 'nomybool'\n"
		                  , outargs->argv[0]);
		          fuse_opt_add_arg(outargs, "-hocd");
		          fuse_main(outargs->argc, outargs->argv, &mrbfsOperations, NULL);
		          exit(1);

		  case KEY_VERSION:
		          fprintf(stderr, "MRBFS version %s\n", MRBFS_VERSION);
		          fuse_opt_add_arg(outargs, "--version");
		          fuse_main(outargs->argc, outargs->argv, &mrbfsOperations, NULL);
		          exit(0);
     }
     return 1;
}

void mrbfsSingleInitConfig()
{
	const char* configFileStr = "mrbfs.conf";
	int ret=0;
		
	if (NULL != gMrbfsConfig->configFileStr && 0 != strlen(gMrbfsConfig->configFileStr))
		configFileStr = gMrbfsConfig->configFileStr;

	gMrbfsConfig->cfgParms = cfg_init(opts, CFGF_NOCASE);
	ret = cfg_parse(gMrbfsConfig->cfgParms, configFileStr);

	if (ret == CFG_FILE_ERROR)
	{
		char* errorStr = "Config file not found";
		ret = asprintf(&errorStr, "Config file [%s] not found, exiting...", configFileStr);
		perror(errorStr);
		free(errorStr);
		exit(1);
	} else if (ret == CFG_PARSE_ERROR) {
		char* errorStr = "Config file error";
		ret = asprintf(&errorStr, "Config file [%s] failed parsing, exiting...", configFileStr);
		perror(errorStr);
		free(errorStr);
		exit(1);
	}
}

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	
	if (NULL == (gMrbfsConfig = calloc(1, sizeof(MRBFSConfig))))
	{
		perror("Failed allocation of global configuration structure, exiting...\n");
		exit(1);
	}
	else
	{
		pthread_mutexattr_t lockAttr;
		// Initialize the master lock
		pthread_mutexattr_init(&lockAttr);
		pthread_mutexattr_settype(&lockAttr, PTHREAD_MUTEX_ADAPTIVE_NP);
		pthread_mutex_init(&gMrbfsConfig->masterLock, &lockAttr);
		pthread_mutexattr_destroy(&lockAttr);		
	}
	
	fuse_opt_parse(&args, &gMrbfsConfig, mrbfs_opts, NULL);

	gMrbfsConfig->logLevel=9;

	// Okay, we've theoretically parsed any configuration options from the command line.
	// Go try to load our configuration file
	mrbfsSingleInitConfig();

	// Okay, configuration file is loaded, start logging
	mrbfsSingleInitLogging();
	
	// At this point, we've got our configuration and logging is active
	// Log a startup message and get on with starting the filesystem
	mrbfsLogMessage(MRBFS_LOG_ERROR, "MRBFS Startup");

	// Setup the initial filesystem
	mrbfsFilesystemInitialize();

	// Setup the interfaces
	mrbfsOpenInterfaces();

	// Setup nodes we know about
	mrbfsLoadNodes();

	return fuse_main(args.argc, args.argv, &mrbfsOperations, mrbfs_opt_proc);
}

int fileExists(const char* filename)
{
	struct stat info;
	if (0 == stat(filename, &info))
		return(1);
	return(0);
}


int mrbfsRemoveNode(MRBFSBus* bus, UINT8 nodeNumber)
{
	MRBFSBusNode* node = bus->node[nodeNumber];
	// If node is null, this thing isn't active
	if (NULL == bus->node[nodeNumber])
		return(0);

	pthread_mutex_lock(&node->nodeLock);
	
	if (NULL != node->nodeName)
		free(node->nodeName);
	
	// FIXME: Need to free the actual node module, but for now it'll just leak

	pthread_mutex_unlock(&node->nodeLock);
	free(node);
	bus->node[nodeNumber] = NULL;
	return(0);
}

int mrbfsRemoveBus(UINT8 busNumber)
{
	int node=0;
	MRBFSBus* bus = gMrbfsConfig->bus[busNumber];
	mrbfsLogMessage(MRBFS_LOG_INFO, "Removing Bus [%d]", busNumber);
	if (NULL == bus)
	{
		mrbfsLogMessage(MRBFS_LOG_INFO, "Nothing to be done, bus [%d] not allocated", busNumber);	
		return(0);
	}
	
	pthread_mutex_lock(&bus->busLock);
	for(node=0; node<256; node++)
	{
		mrbfsRemoveNode(bus, node);
	}
	pthread_mutex_unlock(&bus->busLock);
	
	pthread_mutex_lock(&gMrbfsConfig->masterLock);
	free(bus);
	gMrbfsConfig->bus[busNumber] = NULL;
	pthread_mutex_unlock(&gMrbfsConfig->masterLock);	
	mrbfsLogMessage(MRBFS_LOG_INFO, "Bus [%d] successfully removed", busNumber);
	return(0);
}



int mrbfsAddBus(UINT8 busNumber)
{
	mrbfsLogMessage(MRBFS_LOG_INFO, "Adding Bus [%d]", busNumber);
	pthread_mutex_lock(&gMrbfsConfig->masterLock);
	mrbfsLogMessage(MRBFS_LOG_DEBUG, "Master mutex lock acquired");
	if (gMrbfsConfig->bus[busNumber] == NULL)
	{
		int ret;
		char buffer[256];
		pthread_mutexattr_t lockAttr;
		gMrbfsConfig->bus[busNumber] = calloc(1, sizeof(MRBFSBus));
		if (NULL == gMrbfsConfig->bus[busNumber])
		{
			mrbfsLogMessage(MRBFS_LOG_ERROR, "Calloc() failed on bus [%d] add, exiting", busNumber);
			exit(1);
		}
		gMrbfsConfig->bus[busNumber]->bus = busNumber;

		// Initialize the bus lock
		pthread_mutexattr_init(&lockAttr);
		pthread_mutexattr_settype(&lockAttr, PTHREAD_MUTEX_ADAPTIVE_NP);
		pthread_mutex_init(&gMrbfsConfig->bus[busNumber]->busLock, &lockAttr);
		pthread_mutexattr_destroy(&lockAttr);
		
		// Add directory entries
		sprintf(buffer, "bus%d", busNumber);
		if (NULL == mrbfsFilesystemAddFile(buffer, FNODE_DIR, "/"))
		{
			mrbfsLogMessage(MRBFS_LOG_ERROR, "Failed to add bus [%d] directory, exiting", busNumber);
			exit(1);
		}		
	}
	else
	{
		mrbfsLogMessage(MRBFS_LOG_INFO, "Bus [%d] already exists, skipping add", busNumber);
	}


	
	pthread_mutex_unlock(&gMrbfsConfig->logLock);
	mrbfsLogMessage(MRBFS_LOG_DEBUG, "Master mutex lock released");	
}

int mrbfsOpenInterfaces()
{
	int interfaces = cfg_size(gMrbfsConfig->cfgParms, "interface");
	int i=0, err=0;

	gMrbfsConfig->mrbfsUsedInterfaces = 0;
	
	if (0 == interfaces)
		mrbfsLogMessage(MRBFS_LOG_WARNING, "No interfaces configured - proceeding, but this is slightly nuts");


	for(i=0; i<interfaces; i++)
	{
		char* modulePath = NULL;
		void* interfaceDriverHandle = NULL;
		MRBFSInterfaceDriver* mrbfsInterfaceDriver = NULL;
		int (*MRBFSInterfaceDriverVersionCheck)(int);
		int ret;
		cfg_t *cfgInterface = cfg_getnsec(gMrbfsConfig->cfgParms, "interface", i);
		const char* interfaceName = cfg_title(cfgInterface);
		
		mrbfsLogMessage(MRBFS_LOG_INFO, "Setting up interface [%s]", cfg_title(cfgInterface));
		ret = asprintf(&modulePath, "%s/%s", cfg_getstr(gMrbfsConfig->cfgParms, "module-directory"), cfg_getstr(cfgInterface, "driver"));
				
		// First, test if the driver module exists
		if (!fileExists(modulePath))
		{
			mrbfsLogMessage(MRBFS_LOG_ERROR, "Interface [%s] - driver module not found at [%s]", interfaceName, modulePath);
			free(modulePath);
			continue;
		}

		// Test to make sure the dynamic linker can open it
		if (NULL == (interfaceDriverHandle= (void*)dlopen(modulePath, RTLD_LAZY))) 
		{
			mrbfsLogMessage(MRBFS_LOG_ERROR, "Interface [%s] - driver module failed dlopen [%s]", interfaceName, NULL!=dlerror()?dlerror():"");
			free(modulePath);
			continue;
		}

		mrbfsLogMessage(MRBFS_LOG_DEBUG, "Interface [%s] - sanity checks pass", interfaceName);

		free(modulePath);

		// Now do some cursory version checks
		MRBFSInterfaceDriverVersionCheck = dlsym(interfaceDriverHandle, "MRBFSInterfaceDriverVersionCheck");
		if(NULL == MRBFSInterfaceDriverVersionCheck || !(*MRBFSInterfaceDriverVersionCheck)(MRBFS_INTERFACE_DRIVER_VERSION))
		{
			mrbfsLogMessage(MRBFS_LOG_ERROR, "Interface [%s] - module version check failed", interfaceName);
			continue;
		}
		
		// Okay, looks good, add it to the interface list and run the init function

		mrbfsLogMessage(MRBFS_LOG_DEBUG, "Interface [%s] - actually adding interface", interfaceName);

		mrbfsInterfaceDriver = calloc(1, sizeof(MRBFSInterfaceDriver));
		mrbfsInterfaceDriver->interfaceDriverHandle = interfaceDriverHandle;
		mrbfsInterfaceDriver->interfaceName = strdup(interfaceName);
		mrbfsInterfaceDriver->bus = cfg_getint(cfgInterface, "bus");
		mrbfsInterfaceDriver->port = strdup(cfg_getstr(cfgInterface, "port"));
		mrbfsInterfaceDriver->addr = strtol(cfg_getstr(cfgInterface, "interface-address"), NULL, 36);
		
		mrbfsInterfaceDriver->mrbfsInterfaceDriverRun = dlsym(interfaceDriverHandle, "MRBFSInterfaceDriverRun");
		if(NULL == mrbfsInterfaceDriver->mrbfsInterfaceDriverRun)
		{
			mrbfsLogMessage(MRBFS_LOG_ERROR, "Interface [%s] - module doesn't have a runnable function", interfaceName);
			continue;
		}	
	
	
		mrbfsAddBus(mrbfsInterfaceDriver->bus);

		// Hook up the callbacks for the driver to talk to the main thread
		//MRBFSInterfaceDriver->mrbfsGetNode = &mrbfsGetNode;
		mrbfsInterfaceDriver->mrbfsLogMessage = &mrbfsLogMessage;
		
		gMrbfsConfig->mrbfsInterfaceDrivers[gMrbfsConfig->mrbfsUsedInterfaces++] = mrbfsInterfaceDriver;

		mrbfsLogMessage(MRBFS_LOG_INFO, "Interface [%s] successfully set up in slot %d", mrbfsInterfaceDriver->interfaceName, gMrbfsConfig->mrbfsUsedInterfaces-1);

		err = pthread_create(&mrbfsInterfaceDriver->interfaceThread, NULL, (void*)mrbfsInterfaceDriver->mrbfsInterfaceDriverRun, mrbfsInterfaceDriver);
		if (err)
		{
			mrbfsLogMessage(MRBFS_LOG_INFO, "Interface [%s] failed to start", mrbfsInterfaceDriver->interfaceName);
			// FIXME - destroy interface
			
		}
		else
			mrbfsLogMessage(MRBFS_LOG_INFO, "Interface [%s] started successfully", mrbfsInterfaceDriver->interfaceName);

		
	}

	return(0);
}


int mrbfsLoadNodes()
{
	int nodes = cfg_size(gMrbfsConfig->cfgParms, "node");
	int i=0, err=0;
	
	mrbfsLogMessage(MRBFS_LOG_INFO, "Starting configuration of nodes (%d)", nodes);

	for(i=0; i<nodes; i++)
	{
		char* modulePath = NULL;
		MRBFSBusNode* node = NULL;
		char* fsPath = NULL;
		void* nodeDriverHandle = NULL;
		int (*mrbfsNodeDriverVersionCheck)(int);
		int ret;

		cfg_t *cfgNode = cfg_getnsec(gMrbfsConfig->cfgParms, "node", i);
		const char* nodeName = cfg_title(cfgNode);
		UINT8 bus = cfg_getint(cfgNode, "bus");
		UINT8 address = strtol(cfg_getstr(cfgNode, "address"), NULL, 36);		
		
		mrbfsLogMessage(MRBFS_LOG_INFO, "Node [%s] - Starting setup at bus %d, address 0x%02X", nodeName, bus, address);

		if (NULL != gMrbfsConfig->bus[bus] && NULL != gMrbfsConfig->bus[bus]->node[address])
		{
			mrbfsLogMessage(MRBFS_LOG_ERROR, "Node [%s] - conflicts with node [%s] already at bus %d, address 0x%02X", nodeName, gMrbfsConfig->bus[bus]->node[address]->nodeName, bus, address);		
			continue;
		}

		if (NULL == gMrbfsConfig->bus[bus])
			mrbfsAddBus(bus);

		ret = asprintf(&modulePath, "%s/%s", cfg_getstr(gMrbfsConfig->cfgParms, "module-directory"), cfg_getstr(cfgNode, "driver"));
				
		// First, test if the driver module exists
		if (!fileExists(modulePath))
		{
			mrbfsLogMessage(MRBFS_LOG_ERROR, "Node [%s] - driver module [%s] not found at [%s]", nodeName, cfg_getstr(cfgNode, "driver"), modulePath);
			free(modulePath);
			continue;
		}

		// Test to make sure the dynamic linker can open it
		if (NULL == (nodeDriverHandle= (void*)dlopen(modulePath, RTLD_LAZY))) 
		{
			mrbfsLogMessage(MRBFS_LOG_ERROR, "Node [%s] - driver module [%s] failed dlopen [%s]", nodeName, cfg_getstr(cfgNode, "driver"), NULL!=dlerror()?dlerror():"");
			free(modulePath);
			continue;
		}

		// Now do some cursory version checks
		mrbfsNodeDriverVersionCheck = dlsym(nodeDriverHandle, "mrbfsNodeDriverVersionCheck");
		if(NULL == mrbfsNodeDriverVersionCheck || !(*mrbfsNodeDriverVersionCheck)(MRBFS_NODE_DRIVER_VERSION))
		{
			mrbfsLogMessage(MRBFS_LOG_ERROR, "Node [%s] - module version check failed", nodeName);
			continue;
		}

		mrbfsLogMessage(MRBFS_LOG_DEBUG, "Node [%s] - dynamic library sanity checks pass", nodeName);
		free(modulePath);


		node = (MRBFSBusNode*)calloc(1, sizeof(MRBFSBusNode));
		if (NULL == node)
		{
			mrbfsLogMessage(MRBFS_LOG_ERROR, "Calloc() failed on allocating node [%s] at bus %d at address 0x%02X", nodeName, bus, address);
			exit(1);
		}

		{
			// Node lock initialization
			pthread_mutexattr_t lockAttr;
			// Initialize the master lock
			pthread_mutexattr_init(&lockAttr);
			pthread_mutexattr_settype(&lockAttr, PTHREAD_MUTEX_ADAPTIVE_NP);
			pthread_mutex_init(&node->nodeLock, &lockAttr);
			pthread_mutexattr_destroy(&lockAttr);		
		}
		

/* 
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

		
	// Function pointers from main to the node module
	int (*mrbfsLogMessage)(mrbfsLogLevel, const char*, ...);
	MRBFSNode* (*mrbfsGetNode)(UINT8);
	MRBFSFileNode* (*mrbfsFilesystemAddFile)(const char* fileName, MRBFSFileNodeType fileType, const char* insertionPath);

	// Function pointers from the node to main
	int (*mrbfsNodeInit)(struct MRBFSBusNode*);
	
	int (*mrbfsNodeDestroy)(struct MRBFSBusNode*);
	
} MRBFSBusNode;
*/

		ret = asprintf(&modulePath, "0x%02X - %s", address, nodeName);
		ret = asprintf(&fsPath, "/bus%d", bus);
		
		node->nodeName = strdup(nodeName);
		node->bus = bus;
		node->address = address;
		node->nodeDriverHandle = nodeDriverHandle;
		node->nodeLocalStorage = NULL;
		ret = asprintf(&node->path, "%s/%s", fsPath, modulePath);
		
		node->mrbfsLogMessage = &mrbfsLogMessage;
		node->mrbfsFilesystemAddFile = &mrbfsFilesystemAddFile;
		node->mrbfsNodeInit = dlsym(nodeDriverHandle, "mrbfsNodeInit");
		node->mrbfsNodeDestroy = dlsym(nodeDriverHandle, "mrbfsNodeDestroy");
		node->baseFileNode = mrbfsFilesystemAddFile(modulePath, FNODE_DIR_NODE, fsPath);

		pthread_mutex_lock(&gMrbfsConfig->bus[bus]->busLock);
		gMrbfsConfig->bus[bus]->node[address] = node;
		pthread_mutex_unlock(&gMrbfsConfig->bus[bus]->busLock);


		
		free(modulePath);
		free(fsPath);

	}

	mrbfsLogMessage(MRBFS_LOG_INFO, "Completed configuration of nodes");
	return(0);	
}
