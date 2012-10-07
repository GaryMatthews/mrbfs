#ifndef NODE_HELPERS_H
#define NODE_HELPERS_H

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

const char* mrbfsNodeOptionGet(MRBFSBusNode* mrbfsNode, const char* nodeOptionKey, const char* defaultValue);
int mrbfsNodeQueueTransmitPacket(MRBFSBusNode* mrbfsNode, MRBusPacket* txPkt);
MRBTemperatureUnits mrbfsNodeGetTemperatureUnits(MRBFSBusNode* mrbfsNode, const char* optionName);
MRBPressureUnits mrbfsNodeGetPressureUnits(MRBFSBusNode* mrbfsNode, const char* optionName);
double mrbfsGetTempFrom16K(const UINT8* pktByte, MRBTemperatureUnits units);
const char* mrbfsGetTemperatureDisplayUnits(MRBTemperatureUnits units);
double mrbfsGetPressureFromHPa(const UINT8* pktByte, MRBPressureUnits units);
const char* mrbfsGetPressureDisplayUnits(MRBPressureUnits units);
#endif
