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
#include "mrbfs-pktqueue.h"

#define MRBFS_NODE_DRIVER_NAME   "node-rts"

int mrbfsNodeDriverVersionCheck(int ifaceVersion)
{
	if (ifaceVersion != MRBFS_NODE_DRIVER_VERSION)
		return(0);
	return(1);
}

// ~83 bytes per packet, and hold 25
#define RX_PKT_BUFFER_SZ  (83 * 25)  

typedef enum
{
	MRB_RTS_UNITS_C = 0,
	MRB_RTS_UNITS_K = 1,
	MRB_RTS_UNITS_F = 2
} MrbRtsTemperatureUnits;

typedef struct
{
	UINT32 pktsReceived;
	UINT32 value;
	MRBFSFileNode* tempSensorFiles[4];
	MrbRtsTemperatureUnits units;
	char* tempSensorValues[4];
	MRBFSFileNode* file_rxCounter;
	MRBFSFileNode* file_rxPackets;
	MRBFSFileNode* file_eepromNodeAddr;
	char rxPacketStr[RX_PKT_BUFFER_SZ];
	UINT8 requestRXFeed;
	MRBusPacketQueue rxq;
} NodeLocalStorage;


int nodeQueueTransmitPacket(MRBFSBusNode* mrbfsNode, MRBusPacket* txPkt)
{
	int success = 0;
	if (NULL == mrbfsNode->mrbfsNodeTxPacket)
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] can't transmit - no mrbfsNodeTxPacket function defined", mrbfsNode->nodeName);
	else
	{
		char txPktBuffer[256];
		int i;

		sprintf(txPktBuffer, ":%02X->%02X %02X", txPkt->pkt[MRBUS_PKT_SRC], txPkt->pkt[MRBUS_PKT_DEST], txPkt->pkt[MRBUS_PKT_TYPE]);
		for (i=MRBUS_PKT_DATA; i<txPkt->pkt[MRBUS_PKT_LEN]; i++)
			sprintf(txPktBuffer + strlen(txPktBuffer), " %02X", txPkt->pkt[i]);
	
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] sending packet [%s]", mrbfsNode->nodeName, txPktBuffer);
		(*mrbfsNode->mrbfsNodeTxPacket)(txPkt);
		return(0);
	}
	return(-1);
}

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
	else
	{
		int i;
		for(i=0; i<4; i++)
		{
			if (mrbfsFileNode == nodeLocalStorage->file_output[i])
			{
				MRBusPacket txPkt;
				memset(&txPkt, 0, sizeof(MRBusPacket));
				txPkt.pkt[MRBUS_PKT_SRC] = 0;  // A source of 0xFF will be replaced by the transmit drivers with the interface addresses
				txPkt.pkt[MRBUS_PKT_DEST] = mrbfsNode->address;
				txPkt.pkt[MRBUS_PKT_LEN] = 8;
				txPkt.pkt[MRBUS_PKT_TYPE] = 'C';
				txPkt.pkt[MRBUS_PKT_DATA] = i;
				txPkt.pkt[MRBUS_PKT_DATA+1] = (atoi(data)>0)?1:0;
				nodeQueueTransmitPacket(mrbfsNode, &txPkt);
				break;
			}
		}
	}	
}

// The mrbfsFileNodeRead function is called for files that identify themselves as "readback", meaning
// that it's more than just a simple variable access to get their value
// It must return the size of things written to the buffer, or 0 otherwise

size_t mrbfsFileNodeRead(MRBFSFileNode* mrbfsFileNode, char *buf, size_t size, off_t offset)
{
	MRBFSBusNode* mrbfsNode = (MRBFSBusNode*)(mrbfsFileNode->nodeLocalStorage);
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)(mrbfsNode->nodeLocalStorage);
	MRBusPacket pkt;
	int timeout = 0;
	int foundResponse = 0;
	char responseBuffer[256] = "";
	size_t len=0;

	if (mrbfsFileNode == nodeLocalStorage->file_eepromNodeAddr)
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
			txPkt->pkt[MRBUS_PKT_LEN] = 7;
			txPkt->pkt[MRBUS_PKT_TYPE] = 'R';
			txPkt->pkt[MRBUS_PKT_DATA] = 0;
			// Spin on requestRXFeed - we need to make sure we're the only one listening
			while(nodeLocalStorage->requestRXFeed);
			nodeLocalStorage->requestRXFeed = 1;
			mrbusPacketQueueInitialize(&nodeLocalStorage->rxq);

			(*mrbfsNode->mrbfsNodeTxPacket)(txPkt);
			free(txPkt);
			for(timeout=0; !foundResponse && timeout<1000; timeout++)
			{
				while(!foundResponse && mrbusPacketQueueDepth(&nodeLocalStorage->rxq))
				{
					memset(&pkt, 0, sizeof(MRBusPacket));
					mrbusPacketQueuePop(&nodeLocalStorage->rxq, &pkt);
					(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Readback [%s] saw pkt [%02X->%02X] ['%c']", mrbfsNode->nodeName, pkt.pkt[MRBUS_PKT_SRC], pkt.pkt[MRBUS_PKT_DEST], pkt.pkt[MRBUS_PKT_TYPE]);

					if (pkt.pkt[MRBUS_PKT_SRC] == mrbfsNode->address && pkt.pkt[MRBUS_PKT_TYPE] == 'r')
						foundResponse = 1;
				}
				usleep(1000);
			}
			nodeLocalStorage->requestRXFeed = 0;
			if(!foundResponse)
			{
				(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_WARNING, "Node [%s], no response to EEPROM read request", mrbfsNode->nodeName);
				size = 0;
				return(size);
			}
			sprintf(responseBuffer, "0x%02X\n", pkt.pkt[MRBUS_PKT_DATA]);
		}
	}
	
	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] responding to readback on [%s] with [%s]", mrbfsNode->nodeName, mrbfsFileNode->fileName, responseBuffer);

	len = strlen(responseBuffer);
	if (offset < len) 
	{
		if (offset + size > len)
			size = len - offset;
		memcpy(buf, responseBuffer + offset, size);
	} else
		size = 0;		

	return(size);
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
	int i;
	int units = MRB_RTS_UNITS_C;
	const char* unitsStr;

	mrbfsNode->nodeLocalStorage = (void*)nodeLocalStorage;

	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] starting up with driver [%s]", mrbfsNode->nodeName, MRBFS_NODE_DRIVER_NAME);

	nodeLocalStorage->pktsReceived = 0;
	nodeLocalStorage->file_rxCounter = (*mrbfsNode->mrbfsFilesystemAddFile)("rxCounter", FNODE_RW_VALUE_INT, mrbfsNode->path);
	nodeLocalStorage->file_rxPackets = (*mrbfsNode->mrbfsFilesystemAddFile)("rxPackets", FNODE_RO_VALUE_STR, mrbfsNode->path);
	nodeLocalStorage->file_eepromNodeAddr = (*mrbfsNode->mrbfsFilesystemAddFile)("eepromNodeAddr", FNODE_RO_VALUE_READBACK, mrbfsNode->path);

	nodeLocalStorage->file_rxPackets->value.valueStr = nodeLocalStorage->rxPacketStr;
	nodeLocalStorage->file_rxCounter->mrbfsFileNodeWrite = &mrbfsFileNodeWrite;
	nodeLocalStorage->file_rxCounter->nodeLocalStorage = (void*)mrbfsNode;

	nodeLocalStorage->file_eepromNodeAddr->mrbfsFileNodeWrite = &mrbfsFileNodeWrite;
	nodeLocalStorage->file_eepromNodeAddr->mrbfsFileNodeRead = &mrbfsFileNodeRead;
	nodeLocalStorage->file_eepromNodeAddr->nodeLocalStorage = (void*)mrbfsNode;

	unitsStr = mrbfsNodeOptionGet(mrbfsNode, "temperature_units", "celsius");
	if (0 == strcmp("celsius", unitsStr))
		nodeLocalStorage->units = MRB_RTS_UNITS_C;
	else if (0 == strcmp("fahrenheit", unitsStr))
		nodeLocalStorage->units = MRB_RTS_UNITS_F;
	else if (0 == strcmp("kelvin", unitsStr))
		nodeLocalStorage->units = MRB_RTS_UNITS_K;
	else
		nodeLocalStorage->units = MRB_RTS_UNITS_C;



	for(i=0; i<4; i++)
	{
		char tempSensorDefaultFilename[32];
		char tempSensorKeyname[32];
		const char* tempSensorFilename = tempSensorDefaultFilename;
		sprintf(tempSensorKeyname, "temp_sensor_%c_name", 'A'+i);
		sprintf(tempSensorDefaultFilename, "temp_sensor_%c", 'A' + i);
		tempSensorFilename = mrbfsNodeOptionGet(mrbfsNode, tempSensorKeyname, tempSensorDefaultFilename);
		nodeLocalStorage->tempSensorFiles[i] = (*mrbfsNode->mrbfsFilesystemAddFile)(tempSensorFilename, FNODE_RO_VALUE_STR, mrbfsNode->path);
		// Allocate string storage for values
		nodeLocalStorage->tempSensorValues[i] = calloc(1, 32);
		memcpy(tempSensorValues[i], "No Data");
		nodeLocalStorage->tempSensorFiles[i]->value.valueStr = nodeLocalStorage->tempSensorValues[i];
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
			int i=0;
			for(i=0; i<4; i++)
			{
				int shorted=0;
				int open=0;
				unsigned char flags = rxPkt->pkt[6];
				unsigned short temperatureK = 0;
				switch(i)
				{
					case 0:
						open = (flags & 0x40)?1:0;
						shorted = (flags & 0x80)?1:0;
						temperatureK = (((unsigned short)rxPkt->pkt[7])<<8) + rxPkt->pkt[8];
						break;
					case 1:
						open = (flags & 0x10)?1:0;
						shorted = (flags & 0x20)?1:0;
						temperatureK = (((unsigned short)rxPkt->pkt[9])<<8) + rxPkt->pkt[10];
						break;
					case 2:
						open = (flags & 0x04)?1:0;
						shorted = (flags & 0x08)?1:0;
						temperatureK = (((unsigned short)rxPkt->pkt[11])<<8) + rxPkt->pkt[12];
						break;
					case 3:
						open = (flags & 0x01)?1:0;
						shorted = (flags & 0x02)?1:0;
						temperatureK = (((unsigned short)rxPkt->pkt[13])<<8) + rxPkt->pkt[14];
						break;
				
				}
			
						
			


				nodeLocalStorage->file_output[i]->updateTime = currentTime;
			}

		}
			break;			
	}


	if (nodeLocalStorage->requestRXFeed)
		mrbusPacketQueuePush(&nodeLocalStorage->rxq, rxPkt, 0);

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
