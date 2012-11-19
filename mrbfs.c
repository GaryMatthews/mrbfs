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
#include <signal.h>
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
	gMrbfsConfig->terminate = 1;
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
	.write	= mrbfsWrite,
	.truncate = mrbfsTruncate,
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


void mrbfsTicker()
{
	UINT32 i=0, busNumber=0, nodeNumber=0;
	time_t currentTime=0;
	char buffer[256];
	struct tm timeLocal;
	
	while(!gMrbfsConfig->terminate)
	{
		usleep(10000);
		i++;
		if (i<100)
			continue;

		i=0;
		
		// Fire off tick to everybody once a second
		// Traverse all valid busses and nodes and issue a tick command if any of them have a tick handler registered
		currentTime = time(NULL);
		localtime_r(&currentTime, &timeLocal);
		
		strftime(buffer, sizeof(buffer), "%Y-%b-%d %H:%M:%S", &timeLocal);
		
		mrbfsLogMessage(MRBFS_LOG_DEBUG, "Ticking at %s [%d]", buffer, currentTime);
		
		for(busNumber=0; busNumber<MRBFS_MAX_INTERFACES; busNumber++)
		{
			MRBFSBus* bus = gMrbfsConfig->bus[busNumber];
			if (NULL == bus)
				continue;

			for(nodeNumber=0; nodeNumber<MRBFS_MAX_BUS_NODES; nodeNumber++)
			{
				MRBFSBusNode* node = bus->node[nodeNumber];
				if (NULL == node || NULL == node->mrbfsNodeTick)
					continue;
				mrbfsLogMessage(MRBFS_LOG_DEBUG, "Trying to call tick function, bus=[%d], node=[%02X]", busNumber, nodeNumber);
				(*node->mrbfsNodeTick)((MRBFSBusNode*)node, currentTime);
			}
		}
		mrbfsLogMessage(MRBFS_LOG_DEBUG, "Finished tick at %s [%d]", buffer, currentTime);
		
	}
	
	mrbfsLogMessage(MRBFS_LOG_INFO, "Ticker terminating");	
}

void mrbfsStartTicker()
{
	mrbfsLogMessage(MRBFS_LOG_INFO, "Acquiring lock to start ticker");
	pthread_mutex_lock(&gMrbfsConfig->masterLock);
	mrbfsLogMessage(MRBFS_LOG_INFO, "Lock acquired");	
	// The ticker is a thread with a 1 second clock that calls all nodes with a tick() function once a second
	pthread_create(&gMrbfsConfig->tickerThread, NULL, (void*)&mrbfsTicker, NULL);
	mrbfsLogMessage(MRBFS_LOG_INFO, "Ticker created");	
	pthread_detach(gMrbfsConfig->tickerThread);
	mrbfsLogMessage(MRBFS_LOG_INFO, "Ticker detached");	
	pthread_mutex_unlock(&gMrbfsConfig->masterLock);
	mrbfsLogMessage(MRBFS_LOG_INFO, "Released master lock - ticker running");	
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

void mrbfsSighup(int sig)
{
	char buffer[256];
	sprintf(buffer, "Got signal %d, dying...\n", sig);
	perror(buffer);
	exit(1);

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
	
	mrbfsLogMessage(MRBFS_LOG_INFO, "Starting MRBFS 1 second ticker");	
	mrbfsStartTicker();

	signal(SIGHUP, mrbfsSighup);
	
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
	for(node=0; node<MRBFS_MAX_BUS_NODES; node++)
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
	
static int mrbfsIsValidPacketString(const char* pktStr, MRBusPacket* txPkt)
{
	UINT8 isValid = 0;
	UINT8 pktLen = 6; // Base len - S+D+CRCL+CRCH+LEN+TYPE
	char hexPair[3];
	char* endPtr;
	const char* pktPtr;
	//           0123456789
	// Format is SS->DD D0 D1 D2 D3 ...
	// This is total crap parsing code that should be replaced with regexs by a competent 
	// programmer.  However, today I just want this to work, so...
	
	// Too short or no src/dest sep
	if (strlen(pktStr) < 9 || '-' != pktStr[2] || '>' != pktStr[3])
	{
		mrbfsLogMessage(MRBFS_LOG_ERROR, "Packet [%s] doesn't pass length or src/dest separator tests", pktStr);
		return(0);
	}

	memset(hexPair, 0, sizeof(hexPair));
	strncpy(hexPair, pktStr + 0, 2);
	txPkt->pkt[MRBUS_PKT_SRC] = strtol(hexPair, &endPtr, 16);
	if (endPtr != hexPair + 2)
	{
		mrbfsLogMessage(MRBFS_LOG_ERROR, "Packet [%s] doesn't pass src addr test", pktStr);
		return(0);
	}

	
	strncpy(hexPair, pktStr + 4, 2);
	txPkt->pkt[MRBUS_PKT_DEST] = strtol(hexPair, &endPtr, 16);
	if (endPtr != hexPair + 2)
	{
		mrbfsLogMessage(MRBFS_LOG_ERROR, "Packet [%s] doesn't pass dest addr test", pktStr);
		return(0);
	}

	strncpy(hexPair, pktStr + 7, 2);
	txPkt->pkt[MRBUS_PKT_TYPE] = strtol(hexPair, &endPtr, 16);
	if (endPtr != hexPair + 2)
	{
		mrbfsLogMessage(MRBFS_LOG_ERROR, "Packet [%s] doesn't pass type byte test", pktStr);
		return(0);
	}

	pktPtr = pktStr + 9;
	// Now, there may or may not be more data bytes
	while(strlen(pktPtr) >= 3 && (pktLen < 20))
	{
		// Gotta be hex digits
		if(!(' ' == pktPtr[0] && isxdigit(pktPtr[1]) && isxdigit(pktPtr[2])))
		{
			mrbfsLogMessage(MRBFS_LOG_ERROR, "Packet [%s] doesn't pass format test - %d", pktStr, pktLen);
			return(0);
		}
	
		strncpy(hexPair, pktPtr+1, 2);
		txPkt->pkt[pktLen++] = strtol(hexPair, NULL, 16);
		pktPtr += 3;
	}

	txPkt->pkt[MRBUS_PKT_LEN] = pktLen;
	return(1);

}
		
void mrbfsBusTxWrite(MRBFSFileNode* mrbfsFileNode, const char* data, int dataSz)
{
	uint32_t i=0, bus=0x100;
	MRBusFilePktTxLocalStorage* nodeLocalStorage = NULL;
	MRBusPacket txPkt;
	char pktBuffer[256];
	char* ptr, *pkt, *nextPkt;

	for(i=0; i<MRBFS_MAX_BUS_NODES; i++)
	{
		if (mrbfsFileNode == gMrbfsConfig->bus_filePktTransmit[i])
		{
			// If we match this bus's transmit file, throw the packet down here
			bus = i;
			break;
		}
	}

	// Not matched, bomb out
	if (0x100 == bus)
		return;

	mrbfsLogMessage(MRBFS_LOG_INFO, "Bus %d pkt write - data=[%s], dataSz=%d", bus, data, dataSz);	

	nodeLocalStorage = (MRBusFilePktTxLocalStorage*)(gMrbfsConfig->bus_filePktTransmit[bus]->nodeLocalStorage);
	
	ptr = nodeLocalStorage->inputBuffer + strlen(nodeLocalStorage->inputBuffer);
	
	// Parse and collect user input, cleansing to remove any unpalatables
	for(i=0; i<dataSz && ptr < nodeLocalStorage->inputBuffer + BUS_TX_INPUT_BUFFER_SZ - 2; i++)
	{
		char c = toupper(data[i]);
		if (isalnum(c))
			*ptr++ = c;
		else
		{
			switch(c)
			{
				case '\t':
					*ptr++ = ' ';
					break;
				case '>':
				case '-':
				case ' ':
				case '\n':
					*ptr++ = c;
					break;
			}
		}		
	}

	mrbfsLogMessage(MRBFS_LOG_INFO, "Bus %d pkt write - cleansed - ipb = [%s]", bus,  nodeLocalStorage->inputBuffer);	
	
	// Rip through the input buffer and see if we have a complete packet ready to go
	ptr = pkt = nodeLocalStorage->inputBuffer;
	while('\n' != *pkt && 0 != *pkt)
		pkt++;

	mrbfsLogMessage(MRBFS_LOG_INFO, "Bus %d pkt write - pkt offset at %d", bus, pkt - ptr);	

	while(0 != *pkt)
	{
		// Hey look, we might have a packet
		char* pktStr = (char*)alloca(pkt-ptr + 2);

		mrbfsLogMessage(MRBFS_LOG_INFO, "Bus %d pkt write - parser found packet", bus);

		memset(pktStr, 0, pkt-ptr+2);
		strncpy(pktStr, ptr, pkt-ptr); // Kills the trailing new line or null

		memset(&txPkt, 0, sizeof(MRBusPacket));	
		txPkt.bus = bus;
		
		// Validate that it makes sense and transmit it
		if(mrbfsIsValidPacketString(pktStr, &txPkt))
		{
			mrbfsLogMessage(MRBFS_LOG_INFO, "Bus %d pkt write - transmitting packet", bus);	
		
			if (mrbfsPacketTransmit(&txPkt) < 0)
				mrbfsLogMessage(MRBFS_LOG_ERROR, "Bus %d failed to send packet", bus);
		}
		
/*
		01234567890123456789
      ASDFn000000000000000
      ^   ^
      ipb pkt
  
		mov 0, 5, 20-5
		clr 20-5,     
      
*/

	
		// Cleanse current packet off the input buffer
		memmove(nodeLocalStorage->inputBuffer, pkt+1, BUS_TX_INPUT_BUFFER_SZ - (1 + (pkt - nodeLocalStorage->inputBuffer)));
		memset(nodeLocalStorage->inputBuffer + BUS_TX_INPUT_BUFFER_SZ - (1 + pkt - ptr), 0, (1 + pkt - ptr));
		
		ptr = pkt = nodeLocalStorage->inputBuffer;
		while('\n' != *pkt && 0 != *pkt)
			pkt++;
	}
	
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
		
		// Add transmit file
		sprintf(buffer, "/bus%d", busNumber);
		gMrbfsConfig->bus_filePktTransmit[busNumber] = mrbfsFilesystemAddFile("txPacket", FNODE_RW_VALUE_STR, buffer);
		gMrbfsConfig->bus_filePktTransmit[busNumber]->mrbfsFileNodeWrite = &mrbfsBusTxWrite;
		gMrbfsConfig->bus_filePktTransmit[busNumber]->nodeLocalStorage = (void*)calloc(1, sizeof(MRBusFilePktTxLocalStorage));
		((MRBusFilePktTxLocalStorage*)(gMrbfsConfig->bus_filePktTransmit[busNumber]->nodeLocalStorage))->bus = busNumber;
		gMrbfsConfig->bus_filePktTransmit[busNumber]->value.valueStr = ((MRBusFilePktTxLocalStorage*)(gMrbfsConfig->bus_filePktTransmit[busNumber]->nodeLocalStorage))->inputBuffer;
	}
	else
	{
		mrbfsLogMessage(MRBFS_LOG_INFO, "Bus [%d] already exists, skipping add", busNumber);
	}


	
	pthread_mutex_unlock(&gMrbfsConfig->masterLock);
	mrbfsLogMessage(MRBFS_LOG_DEBUG, "Master mutex lock released");	
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
		mrbfsLogMessage(MRBFS_LOG_DEBUG, "Received packet for [%d/0x%02X] and processed, ret=%d", rxPkt->bus, srcAddr, ret);
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

		mrbfsLogMessage(MRBFS_LOG_DEBUG, "Interface [%s] - setting up filesystem directory", interfaceName);
		ret = asprintf(&mrbfsInterfaceDriver->path, "/interfaces/%s", mrbfsInterfaceDriver->interfaceName);
		mrbfsInterfaceDriver->baseFileNode = mrbfsFilesystemAddFile(mrbfsInterfaceDriver->interfaceName, FNODE_DIR_NODE, "/interfaces");

		// Hook up the callbacks for the driver to talk to the main thread
		mrbfsInterfaceDriver->mrbfsLogMessage = &mrbfsLogMessage;
		mrbfsInterfaceDriver->mrbfsPacketReceive = &mrbfsPacketReceive;
		mrbfsInterfaceDriver->mrbfsFilesystemAddFile = &mrbfsFilesystemAddFile;		
		
		mrbfsInterfaceDriver->mrbfsInterfaceDriverRun = dlsym(interfaceDriverHandle, "mrbfsInterfaceDriverRun");
		if(NULL == mrbfsInterfaceDriver->mrbfsInterfaceDriverRun)
		{
			mrbfsLogMessage(MRBFS_LOG_ERROR, "Interface [%s] - module doesn't have a runnable function", interfaceName);
			continue;
		}	
	
		mrbfsAddBus(mrbfsInterfaceDriver->bus);

		gMrbfsConfig->mrbfsInterfaceDrivers[gMrbfsConfig->mrbfsUsedInterfaces++] = mrbfsInterfaceDriver;

		mrbfsLogMessage(MRBFS_LOG_INFO, "Interface [%s] successfully set up in slot %d", mrbfsInterfaceDriver->interfaceName, gMrbfsConfig->mrbfsUsedInterfaces-1);

		if (NULL != mrbfsInterfaceDriver->mrbfsInterfaceDriverInit)
		{
			mrbfsLogMessage(MRBFS_LOG_DEBUG, "Interface [%s] - running mrbfsInterfaceDriverInit function", interfaceName);
			(*mrbfsInterfaceDriver->mrbfsInterfaceDriverInit)(mrbfsInterfaceDriver);
		}


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


int mrbfsPacketTransmit(MRBusPacket* txPkt)
{
	int i;
	char buffer[64];
	memset(buffer, 0, sizeof(buffer));
	mrbfsLogMessage(MRBFS_LOG_DEBUG, "Starting master transmit routine");


	for(i=0; i<txPkt->pkt[MRBUS_PKT_LEN]; i++)
		sprintf(buffer+3*i, "%02X ", txPkt->pkt[i]);
	*(buffer+3*i-1) = ']';
	mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsPacketTransmit starting - [%s", buffer);

	for(i=0; i<gMrbfsConfig->mrbfsUsedInterfaces; i++)
	{
		if (NULL == gMrbfsConfig->mrbfsInterfaceDrivers[i]->mrbfsInterfacePacketTransmit)
		{
			mrbfsLogMessage(MRBFS_LOG_DEBUG, "Non-transmitting interface [%s], skipping", gMrbfsConfig->mrbfsInterfaceDrivers[i]->interfaceName);
			continue;
		}
		else if (txPkt->bus == gMrbfsConfig->mrbfsInterfaceDrivers[i]->bus)
		{
			mrbfsLogMessage(MRBFS_LOG_DEBUG, "Transmit queueing pkt on Interface [%s], bus[%d]", gMrbfsConfig->mrbfsInterfaceDrivers[i]->interfaceName, txPkt->bus);
			(*gMrbfsConfig->mrbfsInterfaceDrivers[i]->mrbfsInterfacePacketTransmit)(gMrbfsConfig->mrbfsInterfaceDrivers[i], txPkt);
		}
		else
			mrbfsLogMessage(MRBFS_LOG_DEBUG, "Transmit skipping interface [%s], bus[%d]", gMrbfsConfig->mrbfsInterfaceDrivers[i]->interfaceName, txPkt->bus);
	}
	return(0);
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

		ret = asprintf(&modulePath, "0x%02X-%s", address, nodeName);
		ret = asprintf(&fsPath, "/bus%d", bus);
		
		node->nodeName = strdup(nodeName);
		node->bus = bus;
		node->address = address;
		node->nodeDriverHandle = nodeDriverHandle;
		node->nodeLocalStorage = NULL;
		ret = asprintf(&node->path, "%s/%s", fsPath, modulePath);
		
		node->mrbfsLogMessage = &mrbfsLogMessage;
		node->mrbfsFilesystemAddFile = &mrbfsFilesystemAddFile;
		node->mrbfsNodeTxPacket = &mrbfsPacketTransmit;

		node->mrbfsNodeInit = dlsym(nodeDriverHandle, "mrbfsNodeInit");
		node->mrbfsNodeDestroy = dlsym(nodeDriverHandle, "mrbfsNodeDestroy");
		node->mrbfsNodeRxPacket = dlsym(nodeDriverHandle, "mrbfsNodeRxPacket");
		node->mrbfsNodeTick = dlsym(nodeDriverHandle, "mrbfsNodeTick");		
		node->baseFileNode = mrbfsFilesystemAddFile(modulePath, FNODE_DIR_NODE, fsPath);

		node->nodeOptions = cfg_size(cfgNode, "option");
		node->nodeOptionList = calloc(node->nodeOptions, sizeof(MRBFSModuleOption));
		for(nodeOption=0; nodeOption < node->nodeOptions; nodeOption++)
		{
			cfg_t *cfgNodeOption = cfg_getnsec(cfgNode, "option", nodeOption);
			const char* ptr = cfg_title(cfgNodeOption);
			node->nodeOptionList[nodeOption].key = strdup(ptr?:"");
			ptr = cfg_getstr(cfgNodeOption, "value");
			node->nodeOptionList[nodeOption].value = strdup(ptr?:"");
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
