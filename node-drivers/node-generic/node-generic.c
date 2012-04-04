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

int mrbfsNodeInit(MRBFSBusNode* mrbfsNode)
{
	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] starting up", mrbfsNode->nodeName);
	// FIXME - create some sort of files here


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

