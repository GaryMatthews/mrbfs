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

// ~83 bytes per packet, and hold 25
#define RX_PKT_BUFFER_SZ  (83 * 25)  

typedef struct
{
	UINT32 pktsReceived;
	UINT32 value;
	MRBFSFileNode* file_occ[12];
	MRBFSFileNode* file_rxCounter;
	MRBFSFileNode* file_rxPackets;
	MRBFSFileNode* file_mcwritey;
	char rxPacketStr[RX_PKT_BUFFER_SZ];
} NodeLocalStorage;

void mrbfsFileNodeWrite(MRBFSFileNode* mrbfsFileNode, const char* data, int dataSz)
{
	MRBFSBusNode* mrbfsNode = (MRBFSBusNode*)(mrbfsFileNode->nodeLocalStorage);
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)(mrbfsNode->nodeLocalStorage);

	// Now, figure out where this thing goes...
	if (mrbfsFileNode == nodeLocalStorage->file_rxCounter)
	{
		if (0 == atoi(data))
		{
			memset(nodeLocalStorage->rxPacketStr, 0, RX_PKT_BUFFER_SZ);
			mrbfsFileNode->value.valueInt = nodeLocalStorage->pktsReceived = 0;
		}
	}
	else if (mrbfsFileNode == nodeLocalStorage->file_mcwritey)
	{
		MRBusPacket* txPkt = NULL;
		// Oh, we're going to write something to the bus, fun!
		if (NULL == mrbfsNode->mrbfsNodeTxPacket)
			(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] can't transmit - no mrbfsNodeTxPacket function defined", mrbfsNode->nodeName);
		else if (NULL == (txPkt = calloc(1, sizeof(MRBusPacket))))
			(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] can't transmit - failed txPkt allocation", mrbfsNode->nodeName);
		else
		{
			(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] sending packet (dest=0x%02X)", mrbfsNode->nodeName, mrbfsNode->address);
			txPkt->bus = mrbfsNode->bus;
			txPkt->pkt[MRBUS_PKT_SRC] = 0;  // A source of 0xFF will be replaced by the transmit drivers with the interface addresses
			txPkt->pkt[MRBUS_PKT_DEST] = mrbfsNode->address;
			txPkt->pkt[MRBUS_PKT_LEN] = 6;
			txPkt->pkt[MRBUS_PKT_TYPE] = 'A';
			(*mrbfsNode->mrbfsNodeTxPacket)(txPkt);
			free(txPkt);
		}
	}


}


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
	nodeLocalStorage->file_rxCounter = (*mrbfsNode->mrbfsFilesystemAddFile)("rxCounter", FNODE_RW_VALUE_INT, mrbfsNode->path);
	nodeLocalStorage->file_rxPackets = (*mrbfsNode->mrbfsFilesystemAddFile)("rxPackets", FNODE_RO_VALUE_STR, mrbfsNode->path);
	nodeLocalStorage->file_mcwritey = (*mrbfsNode->mrbfsFilesystemAddFile)("mcwritey", FNODE_RW_VALUE_INT, mrbfsNode->path);


	nodeLocalStorage->file_rxPackets->value.valueStr = nodeLocalStorage->rxPacketStr;
	nodeLocalStorage->file_rxCounter->mrbfsFileNodeWrite = &mrbfsFileNodeWrite;
	nodeLocalStorage->file_rxCounter->nodeLocalStorage = (void*)mrbfsNode;

	nodeLocalStorage->file_mcwritey->mrbfsFileNodeWrite = &mrbfsFileNodeWrite;
	nodeLocalStorage->file_mcwritey->nodeLocalStorage = (void*)mrbfsNode;


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

int trimNewlines(char* str, int trimval)
{
	int newlines=0;
	while(0 != *str)
	{
		if ('\n' == *str)
			newlines++;
		if (newlines >= trimval)
			*++str = 0;
		else
			++str;
	}
	return(newlines);
}

// This function may be called simultaneously by multiple packet receivers.  Make sure anything affecting
// the node as a whole is interlocked with mutexes.
int mrbfsNodeRxPacket(MRBFSBusNode* mrbfsNode, MRBusPacket* rxPkt)
{
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)mrbfsNode->nodeLocalStorage;
	time_t currentTime = time(NULL);
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
				nodeLocalStorage->file_occ[i]->updateTime = currentTime;
			}
		}
			break;			
	}

	// Store the packet in the receive queue
	{
		char *newStart = nodeLocalStorage->rxPacketStr;
		size_t rxPacketLen = strlen(nodeLocalStorage->rxPacketStr), newLen=rxPacketLen, newRemaining=2000-rxPacketLen;
		char timeString[64];
		char newPacket[100];
		int b;
		size_t timeSize=0;
		struct tm pktTimeTM;

		localtime_r(&currentTime, &pktTimeTM);
		memset(newPacket, 0, sizeof(newPacket));
		strftime(newPacket, sizeof(newPacket), "[%Y%m%d %H%M%S] ", &pktTimeTM);
	
		for(b=0; b<rxPkt->len; b++)
			sprintf(newPacket + 18 + b*3, "%02X ", rxPkt->pkt[b]);
		*(newPacket + 18 + b*3-1) = '\n';
		*(newPacket + 18 + b*3) = 0;
		newLen = 18 + b*3;

		// Trim rear of existing string
		trimNewlines(nodeLocalStorage->rxPacketStr, 24);

		memmove(nodeLocalStorage->rxPacketStr + newLen, nodeLocalStorage->rxPacketStr, strlen(nodeLocalStorage->rxPacketStr));
		memcpy(nodeLocalStorage->rxPacketStr, newPacket, newLen);

		nodeLocalStorage->file_rxPackets->updateTime = currentTime;
	}

	// Update the number of packets received
	nodeLocalStorage->file_rxCounter->updateTime = currentTime;
	nodeLocalStorage->file_rxCounter->value.valueInt = ++nodeLocalStorage->pktsReceived;
	pthread_mutex_unlock(&mrbfsNode->nodeLock);
	return(0);
}
