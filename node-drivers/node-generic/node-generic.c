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
#include "mrbfs-module.h"


int mrbfsNodeDriverVersionCheck(int ifaceVersion)
{
	if (ifaceVersion != MRBFS_NODE_DRIVER_VERSION)
		return(0);
	return(1);
}

typedef struct
{
	UINT32 pktsReceived;
	UINT32 value;
	MRBFSFileNode* file_packetsReceived;
	MRBFSFileNode* file_mrzero;

} NodeLocalStorage;

int mrbfsNodeInit(MRBFSBusNode* mrbfsNode)
{
	NodeLocalStorage* nodeLocalStorage = calloc(1, sizeof(NodeLocalStorage));
	mrbfsNode->nodeLocalStorage = (void*)nodeLocalStorage;

	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] starting up", mrbfsNode->nodeName);
	
	nodeLocalStorage->pktsReceived = 0;
	nodeLocalStorage->file_packetsReceived = (*mrbfsNode->mrbfsFilesystemAddFile)("packetsReceived", FNODE_RW_VALUE_INT, mrbfsNode->path);
	nodeLocalStorage->file_mrzero = (*mrbfsNode->mrbfsFilesystemAddFile)("mrzero", FNODE_RO_VALUE_INT, mrbfsNode->path);
	return (0);
}


int mrbfsNodeDestroy(MRBFSBusNode* mrbfsNode)
{
	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] shutting down", mrbfsNode->nodeName);

	if (NULL != mrbfsNode->nodeLocalStorage)
		free(mrbfsNode->nodeLocalStorage);
	mrbfsNode->nodeLocalStorage = NULL;


	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] shutdown complete", mrbfsNode->nodeName);
	return (0);
}


// This function may be called simultaneously by multiple packet receivers.  Make sure anything affecting
// the node as a whole is interlocked with mutexes.
int mrbfsNodeRxPacket(MRBFSBusNode* mrbfsNode, MRBusPacket* rxPkt)
{
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)mrbfsNode->nodeLocalStorage;
	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] received packet", mrbfsNode->nodeName);
	pthread_mutex_lock(&mrbfsNode->nodeLock);
	nodeLocalStorage->file_packetsReceived->updateTime = time(NULL);
	nodeLocalStorage->file_packetsReceived->value.valueInt = ++nodeLocalStorage->pktsReceived;
	pthread_mutex_unlock(&mrbfsNode->nodeLock);
	return(0);
}
