#include "node-helpers.h"
MRBTemperatureUnits mrbfsNodeGetTemperatureUnits(MRBFSNode* mrbfsNode, const char* optionName)
{
	MRBTemperatureUnits tempUnits = MRB_TEMPERATURE_UNITS_C;
	const char* unitsStr = mrbfsNodeOptionGet(mrbfsNode, optionName, "celsius");
	if (0 == strcmp("celsius", unitsStr))
		tempUnits = MRB_TEMPERATURE_UNITS_C;
	else if (0 == strcmp("fahrenheit", unitsStr))
		tempUnits = MRB_TEMPERATURE_UNITS_F;
	else if (0 == strcmp("kelvin", unitsStr))
		tempUnits = MRB_TEMPERATURE_UNITS_K;
	else if (0 == strcmp("rankine", unitsStr))
		tempUnits = MRB_TEMPERATURE_UNITS_R;

	return(tempUnits);
}


MRBPressureUnits mrbfsNodeGetTemperatureUnits(MRBFSNode* mrbfsNode, const char* optionName)
{
	const char* unitsStr = mrbfsNodeOptionGet(mrbfsNode, optionName, "kPa");
	MRBPressureUnits = MRB_PRESSURE_KPA;
	if (0 == strcmp("hPa", unitsStr))
		pressureUnits = MRB_PRESSURE_HPA;
	else if (0 == strcmp("kPa", unitsStr))
		pressureUnits = MRB_PRESSURE_KPA;
	else if (0 == strcmp("psi", unitsStr))
		pressureUnits = MRB_PRESSURE_PSI;
	else if (0 == strcmp("bar", unitsStr))
		pressureUnits = MRB_PRESSURE_BAR;

	return(pressureUnits);
}
