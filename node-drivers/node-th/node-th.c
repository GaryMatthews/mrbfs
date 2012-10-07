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
#include <time.h>
#include "mrbfs-module.h"
#include "mrbfs-pktqueue.h"
#include "node-helpers.h"

#define MRBFS_NODE_DRIVER_NAME   "node-th"

typedef enum
{
	SENSOR_UNKNOWN = 0,
	SENSOR_DHT11,
	SENSOR_DHT22,
	SENSOR_HYT221,
	SENSOR_TMP275,
	SENSOR_CPS150
} THSensorPackage;


int mrbfsNodeDriverVersionCheck(int ifaceVersion)
{
	if (ifaceVersion != MRBFS_NODE_DRIVER_VERSION)
		return(0);
	return(1);
}

// ~83 bytes per packet, and hold 25
#define RX_PKT_BUFFER_SZ  (83 * 25)  
#define TEMPERATURE_VALUE_BUFFER_SZ 33




typedef struct
{
	UINT32 pktsReceived;
	UINT32 value;
	MRBFSFileNode* file_tempSensor;
	MRBTemperatureUnits tempUnits;
	char* tempSensorValue;
	MRBFSFileNode* file_relativeHumidity;
	char* relativeHumidityValue;
	MRBFSFileNode* file_busVoltage;
	char* busVoltageValue;

	MRBPressureUnits pressureUnits;
	MRBFSFileNode* file_pressureSensor;
	char* pressureSensorValue;
	
	
	MRBFSFileNode* file_rxCounter;
	MRBFSFileNode* file_rxPackets;
	MRBFSFileNode* file_eepromNodeAddr;
	UINT8 suppressUnits;
	UINT8 decimalPositions;
	UINT8 isWireless;
	char rxPacketStr[RX_PKT_BUFFER_SZ];
	UINT8 requestRXFeed;
	MRBusPacketQueue rxq;
	int timeout;
	time_t lastUpdated;
	THSensorPackage sensorPackage;
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


void nodeResetFilesNoData(MRBFSBusNode* mrbfsNode)
{
	NodeLocalStorage* nodeLocalStorage = mrbfsNode->nodeLocalStorage;
	strcpy(nodeLocalStorage->tempSensorValue, "No Data\n");
	strcpy(nodeLocalStorage->relativeHumidityValue, "No Data\n");
	strcpy(nodeLocalStorage->busVoltageValue, "No Data\n");
}

int mrbfsNodeTick(MRBFSBusNode* mrbfsNode, time_t currentTime)
{
	NodeLocalStorage* nodeLocalStorage = mrbfsNode->nodeLocalStorage;
	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] received tick", mrbfsNode->nodeName);

	if (0 == nodeLocalStorage->timeout)
		return(0);
		
	if (currentTime > (nodeLocalStorage->lastUpdated + nodeLocalStorage->timeout))
	{
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] has timed out on receive, resetting files", mrbfsNode->nodeName);	
		pthread_mutex_lock(&mrbfsNode->nodeLock);
		nodeResetFilesNoData(mrbfsNode);
		pthread_mutex_unlock(&mrbfsNode->nodeLock);
	}
	return(0);
}

int mrbfsNodeInit(MRBFSBusNode* mrbfsNode)
{
	NodeLocalStorage* nodeLocalStorage = calloc(1, sizeof(NodeLocalStorage));
	const char* sensorPkgStr;
	int i;
	const char* unitsStr;
	
	mrbfsNode->nodeLocalStorage = (void*)nodeLocalStorage;

	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] starting up with driver [%s]", mrbfsNode->nodeName, MRBFS_NODE_DRIVER_NAME);

	nodeLocalStorage->pktsReceived = 0;
	nodeLocalStorage->lastUpdated = 0;
	
	nodeLocalStorage->file_rxCounter = (*mrbfsNode->mrbfsFilesystemAddFile)("rxCounter", FNODE_RW_VALUE_INT, mrbfsNode->path);
	nodeLocalStorage->file_rxPackets = (*mrbfsNode->mrbfsFilesystemAddFile)("rxPackets", FNODE_RO_VALUE_STR, mrbfsNode->path);
	nodeLocalStorage->file_eepromNodeAddr = (*mrbfsNode->mrbfsFilesystemAddFile)("eepromNodeAddr", FNODE_RO_VALUE_READBACK, mrbfsNode->path);

	nodeLocalStorage->file_rxPackets->value.valueStr = nodeLocalStorage->rxPacketStr;
	nodeLocalStorage->file_rxCounter->mrbfsFileNodeWrite = &mrbfsFileNodeWrite;
	nodeLocalStorage->file_rxCounter->nodeLocalStorage = (void*)mrbfsNode;

	nodeLocalStorage->file_eepromNodeAddr->mrbfsFileNodeWrite = &mrbfsFileNodeWrite;
	nodeLocalStorage->file_eepromNodeAddr->mrbfsFileNodeRead = &mrbfsFileNodeRead;
	nodeLocalStorage->file_eepromNodeAddr->nodeLocalStorage = (void*)mrbfsNode;
	
	nodeLocalStorage->suppressUnits = 0;
	if (0 == strcmp(mrbfsNodeOptionGet(mrbfsNode, "suppress_units", "no"), "yes"))
		nodeLocalStorage->suppressUnits = 1;

	nodeLocalStorage->timeout = atoi(mrbfsNodeOptionGet(mrbfsNode, "timeout", "none"));

	nodeLocalStorage->decimalPositions = atoi(mrbfsNodeOptionGet(mrbfsNode, "decimal_positions", "2"));

	nodeLocalStorage->isWireless = 0;
	if (0 == strcmp(mrbfsNodeOptionGet(mrbfsNode, "connection", "wired"), "wireless"))
		nodeLocalStorage->isWireless = 1;

	nodeLocalStorage->tempUnits = mrbfsNodeGetTemperatureUnits(mrbfsNode, "temperature_units");
	nodeLocalStorage->pressureUnits = mrbfsNodeGetPressureUnits(mrbfsNode, "pressure_units");

	sensorPkgStr = mrbfsNodeOptionGet(mrbfsNode, "sensor_package", "DHT22");
	if (0 == strcmp(sensorPkgStr, "DHT11"))
		nodeLocalStorage->sensorPackage = SENSOR_DHT11;
	else if (0 == strcmp(sensorPkgStr, "DHT22"))
		nodeLocalStorage->sensorPackage = SENSOR_DHT22;
	else if (0 == strcmp(sensorPkgStr, "TMP275"))
		nodeLocalStorage->sensorPackage = SENSOR_TMP275;		
	else if (0 == strcmp(sensorPkgStr, "HYT221"))
		nodeLocalStorage->sensorPackage = SENSOR_HYT221;
	else if (0 == strcmp(sensorPkgStr, "CPS150"))
		nodeLocalStorage->sensorPackage = SENSOR_CPS150;
	else
		nodeLocalStorage->sensorPackage = SENSOR_UNKNOWN;

	// Allocate all storage, even if not used
	nodeLocalStorage->pressureSensorValue = calloc(1, TEMPERATURE_VALUE_BUFFER_SZ);	
	nodeLocalStorage->tempSensorValue = calloc(1, TEMPERATURE_VALUE_BUFFER_SZ);	
	nodeLocalStorage->relativeHumidityValue = calloc(1, TEMPERATURE_VALUE_BUFFER_SZ);
	nodeLocalStorage->busVoltageValue = calloc(1, TEMPERATURE_VALUE_BUFFER_SZ);	
	
	switch(nodeLocalStorage->sensorPackage)
	{
		case SENSOR_DHT11:
		case SENSOR_DHT22:
		case SENSOR_HYT221:
			nodeLocalStorage->file_relativeHumidity = (*mrbfsNode->mrbfsFilesystemAddFile)("relative_humidity", FNODE_RO_VALUE_STR, mrbfsNode->path);
			nodeLocalStorage->file_relativeHumidity->value.valueStr = nodeLocalStorage->relativeHumidityValue;

			nodeLocalStorage->file_tempSensor = (*mrbfsNode->mrbfsFilesystemAddFile)("temperature", FNODE_RO_VALUE_STR, mrbfsNode->path);
			nodeLocalStorage->file_tempSensor->value.valueStr = nodeLocalStorage->tempSensorValue;
			break;


		case SENSOR_TMP275:
			nodeLocalStorage->file_tempSensor = (*mrbfsNode->mrbfsFilesystemAddFile)("temperature", FNODE_RO_VALUE_STR, mrbfsNode->path);
			nodeLocalStorage->file_tempSensor->value.valueStr = nodeLocalStorage->tempSensorValue;
			break;
			
		case SENSOR_CPS150:
			nodeLocalStorage->file_tempSensor = (*mrbfsNode->mrbfsFilesystemAddFile)("temperature", FNODE_RO_VALUE_STR, mrbfsNode->path);
			nodeLocalStorage->file_tempSensor->value.valueStr = nodeLocalStorage->tempSensorValue;

			nodeLocalStorage->file_pressureSensor = (*mrbfsNode->mrbfsFilesystemAddFile)("absolute_pressure", FNODE_RO_VALUE_STR, mrbfsNode->path);
			nodeLocalStorage->file_pressureSensor->value.valueStr = nodeLocalStorage->pressureSensorValue;
			break;
	}

	nodeLocalStorage->file_busVoltage = (*mrbfsNode->mrbfsFilesystemAddFile)(nodeLocalStorage->isWireless?"battery_voltage":"mrbus_voltage", FNODE_RO_VALUE_STR, mrbfsNode->path);
	nodeLocalStorage->file_busVoltage->value.valueStr = nodeLocalStorage->busVoltageValue;

	nodeResetFilesNoData(mrbfsNode);
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

void populateTempFile(NodeLocalStorage* nodeLocalStorage, double temperature, time_t currentTime)
{
	char unitsStr[16] = "";
	if (!nodeLocalStorage->suppressUnits)
		sprintf(unitsStr, "%s\n", mrbfsGetTemperatureDisplayUnits(nodeLocalStorage->tempUnits));
	snprintf(nodeLocalStorage->tempSensorValue, TEMPERATURE_VALUE_BUFFER_SZ-1, 
		"%.*f%s", nodeLocalStorage->decimalPositions, temperature, unitsStr);
	nodeLocalStorage->file_tempSensor->updateTime = currentTime;
}

void populateHumidityFile(NodeLocalStorage* nodeLocalStorage, double humidity, time_t currentTime)
{
	char unitsStr[16] = "";
	if (!nodeLocalStorage->suppressUnits)
		strcpy(unitsStr, " %RH\n");
	snprintf(nodeLocalStorage->relativeHumidityValue, TEMPERATURE_VALUE_BUFFER_SZ-1, 
		"%.*f%s", nodeLocalStorage->decimalPositions, humidity, unitsStr);
	nodeLocalStorage->file_relativeHumidity->updateTime = currentTime;
}

void populatePressureFile(NodeLocalStorage* nodeLocalStorage, double pressure, time_t currentTime)
{
	char unitsStr[16] = "";
	if (!nodeLocalStorage->suppressUnits)
		sprintf(unitsStr, " %s\n", mrbfsGetPressureDisplayUnits(nodeLocalStorage->pressureUnits));
	snprintf(nodeLocalStorage->pressureSensorValue, TEMPERATURE_VALUE_BUFFER_SZ-1, 
		"%.*f%s", nodeLocalStorage->decimalPositions, pressure, unitsStr);
	nodeLocalStorage->file_pressureSensor->updateTime = currentTime;
}

void populateVoltageFile(NodeLocalStorage* nodeLocalStorage, double busVoltage, time_t currentTime)
{
	snprintf(nodeLocalStorage->busVoltageValue, TEMPERATURE_VALUE_BUFFER_SZ-1, "%.*f%s", nodeLocalStorage->decimalPositions, busVoltage, nodeLocalStorage->suppressUnits?"":" V\n" );
	nodeLocalStorage->file_busVoltage->updateTime = currentTime;
}

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
			double busVoltage = 0;

			nodeLocalStorage->lastUpdated = currentTime;

			switch(nodeLocalStorage->sensorPackage)
			{
				case SENSOR_DHT11:
				case SENSOR_DHT22:
				case SENSOR_HYT221:				
					populateTempFile(nodeLocalStorage, mrbfsGetTempFrom16K(&rxPkt->pkt[7], nodeLocalStorage->tempUnits), currentTime);
					populateHumidityFile(nodeLocalStorage, ((double)rxPkt->pkt[9])/2.0, currentTime);
					populateVoltageFile(nodeLocalStorage, ((double)rxPkt->pkt[10])/10.0, currentTime);
					break;

				case SENSOR_TMP275:
					populateTempFile(nodeLocalStorage, mrbfsGetTempFrom16K(&rxPkt->pkt[7], nodeLocalStorage->tempUnits), currentTime);
					populateVoltageFile(nodeLocalStorage, ((double)rxPkt->pkt[10])/10.0, currentTime);
					break;
			
				case SENSOR_CPS150:
					populateTempFile(nodeLocalStorage, mrbfsGetTempFrom16K(&rxPkt->pkt[7], nodeLocalStorage->tempUnits), currentTime);
					populatePressureFile(nodeLocalStorage, mrbfsGetPressureFromHPa(&rxPkt->pkt[9], nodeLocalStorage->tempUnits), currentTime);
					populateVoltageFile(nodeLocalStorage, ((double)rxPkt->pkt[10])/10.0, currentTime);
					break;
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
