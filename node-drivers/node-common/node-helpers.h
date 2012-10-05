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
	MRB_PRESSURE_BAR = 3
} MRBPressureUnits;


MRBTemperatureUnits mrbfsNodeGetTemperatureUnits(MRBFSNode* mrbfsNode, const char* optionName);
MRBPressureUnits mrbfsNodeGetTemperatureUnits(MRBFSNode* mrbfsNode, const char* optionName);

#endif
