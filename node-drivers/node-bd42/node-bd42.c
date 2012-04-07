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

#define MRBFS_NODE_DRIVER_NAME   "node-bd42"
#define MRB_BD42_MAX_CHANNELS  12

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
	MRBFSFileNode* file_occ[12];
	MRBFSFileNode* file_packetsReceived;
} NodeLocalStorage;

const char* mrbfsNodeOptionGet(MRBFSBusNode* mrbfsNode, const char* nodeOptionKey, const char* defaultValue)
{
	int i;
	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] - [%d] node options, looking for [%s]", mrbfsNode->nodeName, mrbfsNode->nodeOptions, nodeOptionKey);

	for(i=0; i<mrbfsNode->nodeOptions; i++)
	{
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] - node option [%d], comparing key [%s] to [%s]", mrbfsNode->nodeName, mrbfsNode->nodeOptions, nodeOptionKey, mrbfsNode->nodeOptionList[i].key);
		if (0 == strcmp(nodeOptionKey, mrbfsNode->nodeOptionList[i].key))
			return(mrbfsNode->nodeOptionList[i].value);
	}
	return(defaultValue);
}

int mrbfsNodeInit(MRBFSBusNode* mrbfsNode)
{
	NodeLocalStorage* nodeLocalStorage = calloc(1, sizeof(NodeLocalStorage));
	int nodeOccupancyDetectorsConnected;
	int i;

	mrbfsNode->nodeLocalStorage = (void*)nodeLocalStorage;

	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] starting up with driver [%s]", mrbfsNode->nodeName, MRBFS_NODE_DRIVER_NAME);
	
	nodeOccupancyDetectorsConnected = atoi(mrbfsNodeOptionGet(mrbfsNode, "channels_connected", "4"));

	nodeLocalStorage->pktsReceived = 0;
	nodeLocalStorage->file_packetsReceived = (*mrbfsNode->mrbfsFilesystemAddFile)("packetsReceived", FNODE_RW_VALUE_INT, mrbfsNode->path);

	// Initialize the occupancy files
	if (nodeOccupancyDetectorsConnected < 0 || nodeOccupancyDetectorsConnected > MRB_BD42_MAX_CHANNELS)
	{
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Node [%s] - channels_connected must be between 0-%d, not [%d] - defaulting to %d", 
			mrbfsNode->nodeName, MRB_BD42_MAX_CHANNELS, nodeOccupancyDetectorsConnected, MRB_BD42_MAX_CHANNELS);
		nodeOccupancyDetectorsConnected = MRB_BD42_MAX_CHANNELS;
	}

	for(i=0; i<nodeOccupancyDetectorsConnected; i++)
	{
		char occupancyDefaultFilename[32];
		char occupancyKeyname[32];
		const char* occupancyFilename = occupancyDefaultFilename;
		sprintf(occupancyKeyname, "channel_%d_name", i+1);
		sprintf(occupancyDefaultFilename, "channel_%d_occ", i+1);
		occupancyFilename = mrbfsNodeOptionGet(mrbfsNode, occupancyKeyname, occupancyDefaultFilename);
		nodeLocalStorage->file_occ[i] = (*mrbfsNode->mrbfsFilesystemAddFile)(occupancyFilename, FNODE_RO_VALUE_INT, mrbfsNode->path);
	}

	return (0);
}


int mrbfsNodeDestroy(MRBFSBusNode* mrbfsNode)
{
	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] shutting down", mrbfsNode->nodeName);

	if (NULL != mrbfsNode->nodeLocalStorage)
	{
		// FIXME - remove files here
		free(mrbfsNode->nodeLocalStorage);
	}
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

	switch(rxPkt->pkt[MRBUS_PKT_TYPE])
	{
		case 'S':
		{
			unsigned int occupancy = 0;
			int i=0;

			if (rxPkt->pkt[MRBUS_PKT_LEN] = 7)
				occupancy = rxPkt->pkt[MRBUS_PKT_DATA];
			else if (rxPkt->pkt[MRBUS_PKT_LEN] = 8)
				occupancy |= 0xFF00 & ((unsigned int)rxPkt->pkt[MRBUS_PKT_DATA+1])<<8;

			for (i=0; i<MRB_BD42_MAX_CHANNELS; i++)
			{
				if (NULL == nodeLocalStorage->file_occ[i])
					continue;
				nodeLocalStorage->file_occ[i]->value.valueInt = (occupancy & 1<<i)?1:0;
				nodeLocalStorage->file_occ[i]->updateTime = time(NULL);
			}
		}
			break;			
	}

	nodeLocalStorage->file_packetsReceived->updateTime = time(NULL);
	nodeLocalStorage->file_packetsReceived->value.valueInt = ++nodeLocalStorage->pktsReceived;
	pthread_mutex_unlock(&mrbfsNode->nodeLock);
	return(0);
}
