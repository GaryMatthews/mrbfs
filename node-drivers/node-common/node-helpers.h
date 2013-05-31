#ifndef NODE_HELPERS_H
#define NODE_HELPERS_H

#include "mrbfs-types.h"

typedef enum
{
	MRB_TEMPERATURE_UNITS_C = 0,
	MRB_TEMPERATURE_UNITS_K = 1,
	MRB_TEMPERATURE_UNITS_F = 2,
	MRB_TEMPERATURE_UNITS_R = 3
} MRBTemperatureUnits;

typedef enum
{
	MRB_PRESSURE_HPA = 0,
	MRB_PRESSURE_KPA = 1,
	MRB_PRESSURE_PSI = 2,
	MRB_PRESSURE_BAR = 3,
	MRB_PRESSURE_PA  = 4,
	MRB_PRESSURE_TORR = 5,
	MRB_PRESSURE_IN_H2O = 6,
	MRB_PRESSURE_IN_HG = 7,
	MRB_PRESSURE_STD_ATM = 8
	
} MRBPressureUnits;

typedef int (*mrbfsRxPktFilterCallback)(MRBusPacket* rxPkt, uint8_t srcAddress, void* otherFilterData);

const char* mrbfsNodeOptionGet(MRBFSBusNode* mrbfsNode, const char* nodeOptionKey, const char* defaultValue);
int mrbfsNodeQueueTransmitPacket(MRBFSBusNode* mrbfsNode, MRBusPacket* txPkt);
MRBTemperatureUnits mrbfsNodeGetTemperatureUnits(MRBFSBusNode* mrbfsNode, const char* optionName);
MRBPressureUnits mrbfsNodeGetPressureUnits(MRBFSBusNode* mrbfsNode, const char* optionName);
double mrbfsGetTempFrom16K(const UINT8* pktByte, MRBTemperatureUnits units);
const char* mrbfsGetTemperatureDisplayUnits(MRBTemperatureUnits units);
double mrbfsGetPressureFromHPa(const UINT8* pktByte, MRBPressureUnits units);
double mrbfsGetPressureFromHPaDouble(double pressure, MRBPressureUnits units);
const char* mrbfsGetPressureDisplayUnits(MRBPressureUnits units);

MRBFSFileNode* mrbfsNodeCreateFile_RO_STR(MRBFSBusNode* mrbfsNode, const char* fileNameStr, char** fileValueStr, uint32_t fileValueStrSz);
MRBFSFileNode* mrbfsNodeCreateFile_RW_STR(MRBFSBusNode* mrbfsNode, const char* fileNameStr, char** fileValueStr, uint32_t fileValueStrSz, mrbfsFileNodeWriteCallback mrbfsFileNodeWrite);
MRBFSFileNode* mrbfsNodeCreateFile_RO_INT(MRBFSBusNode* mrbfsNode, const char* fileNameStr);
MRBFSFileNode* mrbfsNodeCreateFile_RW_INT(MRBFSBusNode* mrbfsNode, const char* fileNameStr, mrbfsFileNodeWriteCallback mrbfsFileNodeWrite);
MRBFSFileNode* mrbfsNodeCreateFile_RW_READBACK(MRBFSBusNode* mrbfsNode, const char* fileNameStr, mrbfsFileNodeReadCallback mrbfsFileNodeRead, mrbfsFileNodeWriteCallback mrbfsFileNodeWrite);

int mrbfsNodeTxAndGetResponse(MRBFSBusNode* mrbfsNode, MRBusPacketQueue* rxq, pthread_mutex_t* rxFeedLock, volatile uint8_t* requestRxFeed, MRBusPacket* txPkt, MRBusPacket* rxPkt, uint32_t timeoutMilliseconds, uint8_t retries, mrbfsRxPktFilterCallback mrbfsRxPktFilter, void* otherFilterData);
void mrbfsNodeMutexInit(pthread_mutex_t* mutex);
int trimNewlines(char* str, int trimval);

#endif
