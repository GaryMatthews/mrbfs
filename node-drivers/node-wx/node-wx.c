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
#include <math.h>
#include "mrbfs-module.h"
#include "mrbfs-pktqueue.h"
#include "node-helpers.h"

#define MRBFS_NODE_DRIVER_NAME   "node-wx"

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

	MRBFSFileNode* file_tempSensor2;
	char* tempSensorValue2;
	MRBFSFileNode* file_relativeHumidity2;
	char* relativeHumidityValue2;

	MRBFSFileNode* file_busVoltage;
	char* busVoltageValue;

	MRBFSFileNode* file_tempSensor3;
	char* tempSensorValue3;
	MRBFSFileNode* file_tempSensor4;
	char* tempSensorValue4;

	MRBPressureUnits pressureUnits;
	MRBPressureUnits pressureUnitsMSL;
	MRBFSFileNode* file_pressureSensor;
	char* pressureSensorValue;
	MRBFSFileNode* file_meanSeaLevelPressure;
	char* meanSeaLevelPressureValue;
	
	MRBFSFileNode* file_pressureSensor2;
	char* pressureSensorValue2;
	MRBFSFileNode* file_meanSeaLevelPressure2;
	char* meanSeaLevelPressureValue2;
	
	
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
	int altitude;
	time_t lastUpdated;
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
	strcpy(nodeLocalStorage->tempSensorValue2, "No Data\n");
	strcpy(nodeLocalStorage->tempSensorValue3, "No Data\n");
	strcpy(nodeLocalStorage->tempSensorValue4, "No Data\n");
	strcpy(nodeLocalStorage->relativeHumidityValue, "No Data\n");
	strcpy(nodeLocalStorage->relativeHumidityValue2, "No Data\n");
	strcpy(nodeLocalStorage->pressureSensorValue, "No Data\n");	
	strcpy(nodeLocalStorage->pressureSensorValue2, "No Data\n");	
	strcpy(nodeLocalStorage->meanSeaLevelPressureValue, "No Data\n");
	strcpy(nodeLocalStorage->meanSeaLevelPressureValue2, "No Data\n");
	strcpy(nodeLocalStorage->busVoltageValue, "No Data\n");
}

int mrbfsNodeTick(MRBFSBusNode* mrbfsNode, time_t currentTime)
{
	NodeLocalStorage* nodeLocalStorage = mrbfsNode->nodeLocalStorage;
	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_ANNOYING, "Node [%s] received tick", mrbfsNode->nodeName);

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
	nodeLocalStorage->pressureUnitsMSL = mrbfsNodeGetPressureUnits(mrbfsNode, "mean_sea_level_pressure_units");

	nodeLocalStorage->altitude = atoi(mrbfsNodeOptionGet(mrbfsNode, "altitude_meters", "-1"));
	

	// Allocate all storage, even if not used
	nodeLocalStorage->tempSensorValue = calloc(1, TEMPERATURE_VALUE_BUFFER_SZ);	
	nodeLocalStorage->tempSensorValue2 = calloc(1, TEMPERATURE_VALUE_BUFFER_SZ);	
	nodeLocalStorage->tempSensorValue3 = calloc(1, TEMPERATURE_VALUE_BUFFER_SZ);	
	nodeLocalStorage->tempSensorValue4 = calloc(1, TEMPERATURE_VALUE_BUFFER_SZ);	
	nodeLocalStorage->relativeHumidityValue = calloc(1, TEMPERATURE_VALUE_BUFFER_SZ);
	nodeLocalStorage->relativeHumidityValue2 = calloc(1, TEMPERATURE_VALUE_BUFFER_SZ);
	nodeLocalStorage->pressureSensorValue = calloc(1, TEMPERATURE_VALUE_BUFFER_SZ);	
	nodeLocalStorage->pressureSensorValue2 = calloc(1, TEMPERATURE_VALUE_BUFFER_SZ);	
	nodeLocalStorage->meanSeaLevelPressureValue = calloc(1, TEMPERATURE_VALUE_BUFFER_SZ);
	nodeLocalStorage->meanSeaLevelPressureValue2 = calloc(1, TEMPERATURE_VALUE_BUFFER_SZ);
	nodeLocalStorage->busVoltageValue = calloc(1, TEMPERATURE_VALUE_BUFFER_SZ);	
	
	nodeLocalStorage->file_tempSensor = (*mrbfsNode->mrbfsFilesystemAddFile)("temperature", FNODE_RO_VALUE_STR, mrbfsNode->path);
	nodeLocalStorage->file_tempSensor->value.valueStr = nodeLocalStorage->tempSensorValue;
	nodeLocalStorage->file_tempSensor2 = (*mrbfsNode->mrbfsFilesystemAddFile)("temperature2", FNODE_RO_VALUE_STR, mrbfsNode->path);
	nodeLocalStorage->file_tempSensor2->value.valueStr = nodeLocalStorage->tempSensorValue2;
	nodeLocalStorage->file_tempSensor3 = (*mrbfsNode->mrbfsFilesystemAddFile)("temperature3", FNODE_RO_VALUE_STR, mrbfsNode->path);
	nodeLocalStorage->file_tempSensor3->value.valueStr = nodeLocalStorage->tempSensorValue3;
	nodeLocalStorage->file_tempSensor4 = (*mrbfsNode->mrbfsFilesystemAddFile)("temperature4", FNODE_RO_VALUE_STR, mrbfsNode->path);
	nodeLocalStorage->file_tempSensor4->value.valueStr = nodeLocalStorage->tempSensorValue4;

	nodeLocalStorage->file_relativeHumidity = (*mrbfsNode->mrbfsFilesystemAddFile)("relative_humidity", FNODE_RO_VALUE_STR, mrbfsNode->path);
	nodeLocalStorage->file_relativeHumidity->value.valueStr = nodeLocalStorage->relativeHumidityValue;
	nodeLocalStorage->file_relativeHumidity2 = (*mrbfsNode->mrbfsFilesystemAddFile)("relative_humidity2", FNODE_RO_VALUE_STR, mrbfsNode->path);
	nodeLocalStorage->file_relativeHumidity2->value.valueStr = nodeLocalStorage->relativeHumidityValue2;

	nodeLocalStorage->file_pressureSensor = (*mrbfsNode->mrbfsFilesystemAddFile)("absolute_pressure", FNODE_RO_VALUE_STR, mrbfsNode->path);
	nodeLocalStorage->file_pressureSensor->value.valueStr = nodeLocalStorage->pressureSensorValue;
	nodeLocalStorage->file_pressureSensor2 = (*mrbfsNode->mrbfsFilesystemAddFile)("absolute_pressure2", FNODE_RO_VALUE_STR, mrbfsNode->path);
	nodeLocalStorage->file_pressureSensor2->value.valueStr = nodeLocalStorage->pressureSensorValue2;
	if (-1 != nodeLocalStorage->altitude)
	{
		nodeLocalStorage->file_meanSeaLevelPressure = (*mrbfsNode->mrbfsFilesystemAddFile)("mean_sea_level_pressure", FNODE_RO_VALUE_STR, mrbfsNode->path);
		nodeLocalStorage->file_meanSeaLevelPressure->value.valueStr = nodeLocalStorage->meanSeaLevelPressureValue;
		nodeLocalStorage->file_meanSeaLevelPressure2 = (*mrbfsNode->mrbfsFilesystemAddFile)("mean_sea_level_pressure2", FNODE_RO_VALUE_STR, mrbfsNode->path);
		nodeLocalStorage->file_meanSeaLevelPressure2->value.valueStr = nodeLocalStorage->meanSeaLevelPressureValue2;
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

// This function may be called simultaneously by multiple packet receivers.  Make sure anything affecting
// the node as a whole is interlocked with mutexes.

void populateTempFile(NodeLocalStorage* nodeLocalStorage, double temperature, time_t currentTime)
{
	char unitsStr[16] = "";
	if (!nodeLocalStorage->suppressUnits)
		sprintf(unitsStr, " %s\n", mrbfsGetTemperatureDisplayUnits(nodeLocalStorage->tempUnits));
	snprintf(nodeLocalStorage->tempSensorValue, TEMPERATURE_VALUE_BUFFER_SZ-1, 
		"%.*f%s", nodeLocalStorage->decimalPositions, temperature, unitsStr);
	nodeLocalStorage->file_tempSensor->updateTime = currentTime;
}

void populateTempFile2(NodeLocalStorage* nodeLocalStorage, double temperature, time_t currentTime)
{
	char unitsStr[16] = "";
	if (!nodeLocalStorage->suppressUnits)
		sprintf(unitsStr, " %s\n", mrbfsGetTemperatureDisplayUnits(nodeLocalStorage->tempUnits));
	snprintf(nodeLocalStorage->tempSensorValue2, TEMPERATURE_VALUE_BUFFER_SZ-1, 
		"%.*f%s", nodeLocalStorage->decimalPositions, temperature, unitsStr);
	nodeLocalStorage->file_tempSensor2->updateTime = currentTime;
}

void populateTempFile3(NodeLocalStorage* nodeLocalStorage, double temperature, time_t currentTime)
{
	char unitsStr[16] = "";
	if (!nodeLocalStorage->suppressUnits)
		sprintf(unitsStr, " %s\n", mrbfsGetTemperatureDisplayUnits(nodeLocalStorage->tempUnits));
	snprintf(nodeLocalStorage->tempSensorValue3, TEMPERATURE_VALUE_BUFFER_SZ-1, 
		"%.*f%s", nodeLocalStorage->decimalPositions, temperature, unitsStr);
	nodeLocalStorage->file_tempSensor3->updateTime = currentTime;
}

void populateTempFile4(NodeLocalStorage* nodeLocalStorage, double temperature, time_t currentTime)
{
	char unitsStr[16] = "";
	if (!nodeLocalStorage->suppressUnits)
		sprintf(unitsStr, " %s\n", mrbfsGetTemperatureDisplayUnits(nodeLocalStorage->tempUnits));
	snprintf(nodeLocalStorage->tempSensorValue4, TEMPERATURE_VALUE_BUFFER_SZ-1, 
		"%.*f%s", nodeLocalStorage->decimalPositions, temperature, unitsStr);
	nodeLocalStorage->file_tempSensor4->updateTime = currentTime;
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

void populateHumidityFile2(NodeLocalStorage* nodeLocalStorage, double humidity, time_t currentTime)
{
	char unitsStr[16] = "";
	if (!nodeLocalStorage->suppressUnits)
		strcpy(unitsStr, " %RH\n");
	snprintf(nodeLocalStorage->relativeHumidityValue2, TEMPERATURE_VALUE_BUFFER_SZ-1, 
		"%.*f%s", nodeLocalStorage->decimalPositions, humidity, unitsStr);
	nodeLocalStorage->file_relativeHumidity2->updateTime = currentTime;
}

// Temperature must be in degrees K
// Pressure is absolute and in hectopascals

void populateMSLPFile(NodeLocalStorage* nodeLocalStorage, double pressure, double temperature, time_t currentTime)
{
	if (-1 != nodeLocalStorage->altitude)
	{
		double mslp = 0.0;
		double altitude = (double)nodeLocalStorage->altitude * 0.0065;
		double altitude_factor = 1.0 - (altitude / (temperature + altitude));
		
		char unitsStr[16] = "";
		//P0 = P * (1 - (0.0065 * h) / (Tk + 0.0065 * h) ) ^ -5.257
		mslp = pressure * 1.0/pow(altitude_factor, 5.257);

		if (!nodeLocalStorage->suppressUnits)
			sprintf(unitsStr, " %s\n", mrbfsGetPressureDisplayUnits(nodeLocalStorage->pressureUnitsMSL));

		snprintf(nodeLocalStorage->meanSeaLevelPressureValue, TEMPERATURE_VALUE_BUFFER_SZ-1, 
			"%.*f%s", nodeLocalStorage->decimalPositions, mrbfsGetPressureFromHPaDouble(mslp, nodeLocalStorage->pressureUnitsMSL), unitsStr);
		nodeLocalStorage->file_meanSeaLevelPressure->updateTime = currentTime;
	}
}

void populateMSLPFile2(NodeLocalStorage* nodeLocalStorage, double pressure, double temperature, time_t currentTime)
{
	if (-1 != nodeLocalStorage->altitude)
	{
		double mslp = 0.0;
		double altitude = (double)nodeLocalStorage->altitude * 0.0065;
		double altitude_factor = 1.0 - (altitude / (temperature + altitude));
		
		char unitsStr[16] = "";
		//P0 = P * (1 - (0.0065 * h) / (Tk + 0.0065 * h) ) ^ -5.257
		mslp = pressure * 1.0/pow(altitude_factor, 5.257);

		if (!nodeLocalStorage->suppressUnits)
			sprintf(unitsStr, " %s\n", mrbfsGetPressureDisplayUnits(nodeLocalStorage->pressureUnitsMSL));

		snprintf(nodeLocalStorage->meanSeaLevelPressureValue2, TEMPERATURE_VALUE_BUFFER_SZ-1, 
			"%.*f%s", nodeLocalStorage->decimalPositions, mrbfsGetPressureFromHPaDouble(mslp, nodeLocalStorage->pressureUnitsMSL), unitsStr);
		nodeLocalStorage->file_meanSeaLevelPressure2->updateTime = currentTime;
	}
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

void populatePressureFile2(NodeLocalStorage* nodeLocalStorage, double pressure, time_t currentTime)
{
	char unitsStr[16] = "";
	if (!nodeLocalStorage->suppressUnits)
		sprintf(unitsStr, " %s\n", mrbfsGetPressureDisplayUnits(nodeLocalStorage->pressureUnits));
	snprintf(nodeLocalStorage->pressureSensorValue2, TEMPERATURE_VALUE_BUFFER_SZ-1, 
		"%.*f%s", nodeLocalStorage->decimalPositions, pressure, unitsStr);
	nodeLocalStorage->file_pressureSensor2->updateTime = currentTime;
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
/*
			FILE *fptr;
			char timeString[64];
			char newPacket[100];
			int b;
			size_t timeSize=0;
			struct tm pktTimeTM;
			fptr = fopen("/home/house/mrbfs.wx.pktlog", "a");

			localtime_r(&currentTime, &pktTimeTM);
			memset(newPacket, 0, sizeof(newPacket));
			strftime(newPacket, sizeof(newPacket), "[%Y%m%d %H%M%S] R ", &pktTimeTM);

			for(b=0; b<rxPkt->len; b++)
				sprintf(newPacket + 20 + b*3, "%02X ", rxPkt->pkt[b]);
			*(newPacket + 20 + b*3-1) = '\n';
			*(newPacket + 20 + b*3) = 0;
			fprintf(fptr, "%s", newPacket);
*/
			switch(rxPkt->pkt[MRBUS_PKT_TYPE+1])
			{
				case 'W':
				{
//					fprintf(fptr, "--> W\n");
					nodeLocalStorage->lastUpdated = currentTime;

					populateTempFile(nodeLocalStorage, mrbfsGetTempFrom16K(&rxPkt->pkt[8], nodeLocalStorage->tempUnits), currentTime);
					populateTempFile2(nodeLocalStorage, mrbfsGetTempFrom16K(&rxPkt->pkt[14], nodeLocalStorage->tempUnits), currentTime);
					populateHumidityFile(nodeLocalStorage, ((double)rxPkt->pkt[10])/2.0, currentTime);
					populateHumidityFile2(nodeLocalStorage, ((double)rxPkt->pkt[16])/2.0, currentTime);

					populateVoltageFile(nodeLocalStorage, ((double)rxPkt->pkt[19])/10.0, currentTime);
					break;
				}
				case 'X':
				{
//					fprintf(fptr, "--> X\n");
					nodeLocalStorage->lastUpdated = currentTime;

					populateTempFile3(nodeLocalStorage, mrbfsGetTempFrom16K(&rxPkt->pkt[8], nodeLocalStorage->tempUnits), currentTime);
					populateTempFile4(nodeLocalStorage, mrbfsGetTempFrom16K(&rxPkt->pkt[12], nodeLocalStorage->tempUnits), currentTime);

					populatePressureFile(nodeLocalStorage, mrbfsGetPressureFromHPa(&rxPkt->pkt[10], nodeLocalStorage->pressureUnits), currentTime);
					populatePressureFile2(nodeLocalStorage, mrbfsGetPressureFromHPa(&rxPkt->pkt[14], nodeLocalStorage->pressureUnits), currentTime);
					if (-1 != nodeLocalStorage->altitude)
					{
						populateMSLPFile(nodeLocalStorage, mrbfsGetPressureFromHPa(&rxPkt->pkt[10], MRB_PRESSURE_HPA), mrbfsGetTempFrom16K(&rxPkt->pkt[8], MRB_TEMPERATURE_UNITS_K), currentTime);
						populateMSLPFile2(nodeLocalStorage, mrbfsGetPressureFromHPa(&rxPkt->pkt[14], MRB_PRESSURE_HPA), mrbfsGetTempFrom16K(&rxPkt->pkt[12], MRB_TEMPERATURE_UNITS_K), currentTime);
					}
					break;
				}
			}

//		fclose(fptr);

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
