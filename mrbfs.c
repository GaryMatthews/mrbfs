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
//	mrbfsFilesystemDestroy();
//	mrbfsInterfacesDestroy();
 //  cfg_free(gMrbfsConfig->cfgParms);	
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
   struct fuse *fuse;
   struct fuse_chan *ch;
   char *mountpoint;
   int multithreaded;
   int foreground;
   int res;
   struct stat st;   

	
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

	res = fuse_parse_cmdline(&args, &mountpoint, &multithreaded, &foreground);
	if (res == -1)
		exit(1);   

	res = stat(mountpoint, &st);
	if (res == -1) 
	{
		perror(mountpoint);
		exit(1);
	}
	ch = fuse_mount(mountpoint, &args);
	if (!ch)
		exit(1);

	res = fcntl(fuse_chan_fd(ch), F_SETFD, FD_CLOEXEC);
	if (res == -1)
		perror("WARNING: failed to set FD_CLOEXEC on fuse device");

	fuse = fuse_new(ch, &args, &mrbfsOperations, sizeof(struct fuse_operations), NULL);
	if (fuse == NULL) 
	{
		fuse_unmount(mountpoint, ch);
		exit(1);
	}
	mrbfsLogMessage(MRBFS_LOG_INFO, "Daemonizing");
	res = fuse_daemonize(foreground);
	if (res != -1)
		res = fuse_set_signal_handlers(fuse_get_session(fuse));

	if (res == -1) 
	{
		fuse_unmount(mountpoint, ch);
		fuse_destroy(fuse);
		exit(1);
	}
	
	mrbfsLogMessage(MRBFS_LOG_INFO, "Starting MRBFS filesystem");
	// Setup the initial filesystem
	mrbfsFilesystemInitialize();

	mrbfsLogMessage(MRBFS_LOG_INFO, "Starting MRBFS interfaces");
	// Setup the interfaces
	mrbfsOpenInterfaces();

	mrbfsLogMessage(MRBFS_LOG_INFO, "Starting MRBFS known nodes");
	// Setup nodes we know about
	mrbfsLoadNodes();	
	
	mrbfsLogMessage(MRBFS_LOG_INFO, "Starting MRBFS fuse main loop");
	if (multithreaded)
		res = fuse_loop_mt(fuse);
	else
		res = fuse_loop(fuse);

	if (res == -1)
		res = 1;   
	else
		res = 0;

   fuse_remove_signal_handlers(fuse_get_session(fuse));
   fuse_unmount(mountpoint, ch);
   fuse_destroy(fuse);
   free(mountpoint);  

	return(res);
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

void mrbusPacketQueueInitialize(MRBusPacketQueue* q)
{
	pthread_mutexattr_t lockAttr;

	// Initialize the queue lock
	pthread_mutexattr_init(&lockAttr);
	pthread_mutexattr_settype(&lockAttr, PTHREAD_MUTEX_ADAPTIVE_NP);
	pthread_mutex_init(&q->queueLock, &lockAttr);
	pthread_mutexattr_destroy(&lockAttr);		

	q->headIdx = q->tailIdx = 0;
}


int mrbusPacketQueueDepth(MRBusPacketQueue* q)
{
	int depth = 0;
	
	pthread_mutex_lock(&q->queueLock);
	depth = (q->headIdx - q->tailIdx) % MRBUS_PACKET_QUEUE_SIZE; 
	pthread_mutex_unlock(&q->queueLock);

	return(depth);
}

void mrbusPacketQueuePush(MRBusPacketQueue* q, UINT8 bus, UINT8 len, UINT8 srcInterface, const UINT8* data)
{
	pthread_mutex_lock(&q->queueLock);
	if (len > MRBFS_MAX_PACKET_LEN)
		len = MRBFS_MAX_PACKET_LEN;

	q->pkts[q->headIdx].bus = bus;
	q->pkts[q->headIdx].len = len;
	q->pkts[q->headIdx].srcInterface = srcInterface;
	memset(q->pkts[q->headIdx].pkt, 0, MRBFS_MAX_PACKET_LEN);
	memcpy(q->pkts[q->headIdx].pkt, data, len);
	
	if( ++q->headIdx >= MRBUS_PACKET_QUEUE_SIZE )
		q->headIdx = 0;

	pthread_mutex_unlock(&q->queueLock);
}

MRBusPacket mrbusPacketQueuePop(MRBusPacketQueue* q)
{
	MRBusPacket pkt;
	memcpy(&pkt, &q->pkts[q->tailIdx], sizeof(MRBusPacket));

	if( ++q->tailIdx >= MRBUS_PACKET_QUEUE_SIZE )
		q->tailIdx = 0;

	return(pkt);
}

// mrbfsPacketReceive is spun off by the interfaces as they receive data
// thus is can be running multiple times in parallel

void mrbfsPacketReceive(MRBusPacket* rxPkt)
{
	UINT8 srcAddr = rxPkt->pkt[MRBUS_PKT_SRC];
// I don't think we need mutexing here, since this will run in the interface process space
	if (NULL == gMrbfsConfig->bus[rxPkt->bus])
	{
		mrbfsLogMessage(MRBFS_LOG_INFO, "Received packet for bus[%d], which isn't set up", rxPkt->bus);
		return;
	}

	if (NULL == gMrbfsConfig->bus[rxPkt->bus]->node[srcAddr])
	{
		mrbfsLogMessage(MRBFS_LOG_INFO, "Received packet for [%d/0x%02X], which isn't set up", rxPkt->bus, srcAddr);
		// FIXME - load generic node driver every time we see a packet going somewhere we don't recognize
		return;
	}
	
	if (NULL != gMrbfsConfig->bus[rxPkt->bus]->node[srcAddr]->mrbfsNodeRxPacket)
	{
		int ret = (*gMrbfsConfig->bus[rxPkt->bus]->node[srcAddr]->mrbfsNodeRxPacket)(gMrbfsConfig->bus[rxPkt->bus]->node[srcAddr], rxPkt);
		mrbfsLogMessage(MRBFS_LOG_DEBUG, "Received packet for [%d/%02X] and processed, ret=%d", rxPkt->bus, srcAddr, ret);
	}
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
		MRBFSInterfaceDriverVersionCheck = dlsym(interfaceDriverHandle, "mrbfsInterfaceDriverVersionCheck");
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
		mrbfsInterfaceDriver->addr = strtol(cfg_getstr(cfgInterface, "interface-address"), NULL, 16);
		mrbfsInterfaceDriver->mrbfsInterfacePacketTransmit = dlsym(interfaceDriverHandle, "mrbfsInterfacePacketTransmit");
		mrbfsInterfaceDriver->mrbfsInterfaceDriverInit = dlsym(interfaceDriverHandle, "mrbfsInterfaceDriverInit");
	
		mrbfsInterfaceDriver->interfaceOptions = cfg_size(cfgInterface, "option");
		if (mrbfsInterfaceDriver->interfaceOptions > 0)
		{
			int interfaceOption=0;
			mrbfsInterfaceDriver->interfaceOptionList = calloc(mrbfsInterfaceDriver->interfaceOptions, sizeof(MRBFSModuleOption));
			for(interfaceOption=0; interfaceOption < mrbfsInterfaceDriver->interfaceOptions; interfaceOption++)
			{
				cfg_t *cfgInterfaceOption = cfg_getnsec(cfgInterface, "option", interfaceOption);
				mrbfsInterfaceDriver->interfaceOptionList[i].key = strdup(cfg_title(cfgInterfaceOption));
				mrbfsInterfaceDriver->interfaceOptionList[i].value = strdup(cfg_getstr(cfgInterfaceOption, "value"));
			}	
		}
		else
		{
			mrbfsInterfaceDriver->interfaceOptions = 0;
		}
		
		
		if (NULL != mrbfsInterfaceDriver->mrbfsInterfaceDriverInit)
			(*mrbfsInterfaceDriver->mrbfsInterfaceDriverInit)(mrbfsInterfaceDriver);
	
		mrbfsInterfaceDriver->mrbfsInterfaceDriverRun = dlsym(interfaceDriverHandle, "mrbfsInterfaceDriverRun");
		if(NULL == mrbfsInterfaceDriver->mrbfsInterfaceDriverRun)
		{
			mrbfsLogMessage(MRBFS_LOG_ERROR, "Interface [%s] - module doesn't have a runnable function", interfaceName);
			continue;
		}	
	
		mrbfsAddBus(mrbfsInterfaceDriver->bus);

		// Hook up the callbacks for the driver to talk to the main thread
		mrbfsInterfaceDriver->mrbfsLogMessage = &mrbfsLogMessage;
		mrbfsInterfaceDriver->mrbfsPacketReceive = &mrbfsPacketReceive;
		gMrbfsConfig->mrbfsInterfaceDrivers[gMrbfsConfig->mrbfsUsedInterfaces++] = mrbfsInterfaceDriver;

		mrbfsLogMessage(MRBFS_LOG_INFO, "Interface [%s] successfully set up in slot %d", mrbfsInterfaceDriver->interfaceName, gMrbfsConfig->mrbfsUsedInterfaces-1);

		{
			err = pthread_create(&mrbfsInterfaceDriver->interfaceThread, NULL, (void*)mrbfsInterfaceDriver->mrbfsInterfaceDriverRun, mrbfsInterfaceDriver);
			pthread_detach(mrbfsInterfaceDriver->interfaceThread);
		}
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


void mrbfsPacketTransmit(MRBusPacket* txPkt)
{
	int i;
	for(i=0; i<gMrbfsConfig->mrbfsUsedInterfaces; i++)
	{
		if (txPkt->bus == gMrbfsConfig->mrbfsInterfaceDrivers[i]->bus)
			return;  // FIXME - this actually should be transmit...
	
	}
}

int mrbfsLoadNodes()
{
	int nodes = cfg_size(gMrbfsConfig->cfgParms, "node");
	int i=0, err=0, nodeOption=0;
	
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
		UINT8 address = strtol(cfg_getstr(cfgNode, "address"), NULL, 16);		
		
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
		node->mrbfsNodeRxPacket = dlsym(nodeDriverHandle, "mrbfsNodeRxPacket");
		node->baseFileNode = mrbfsFilesystemAddFile(modulePath, FNODE_DIR_NODE, fsPath);

		node->nodeOptions = cfg_size(cfgNode, "option");
		node->nodeOptionList = calloc(node->nodeOptions, sizeof(MRBFSModuleOption));
		for(nodeOption=0; nodeOption < node->nodeOptions; nodeOption++)
		{
			cfg_t *cfgNodeOption = cfg_getnsec(cfgNode, "option", nodeOption);
			node->nodeOptionList[i].key = strdup(cfg_title(cfgNodeOption));
			node->nodeOptionList[i].value = strdup(cfg_getstr(cfgNodeOption, "value"));
		}

		(*node->mrbfsNodeInit)(node);

		pthread_mutex_lock(&gMrbfsConfig->bus[bus]->busLock);
		gMrbfsConfig->bus[bus]->node[address] = node;
		pthread_mutex_unlock(&gMrbfsConfig->bus[bus]->busLock);


		
		free(modulePath);
		free(fsPath);

	}

	mrbfsLogMessage(MRBFS_LOG_INFO, "Completed configuration of nodes");
	return(0);	
}
