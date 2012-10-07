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

MRBTemperatureUnits mrbfsNodeGetTemperatureUnits(MRBFSBusNode* mrbfsNode, const char* optionName)
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


MRBPressureUnits mrbfsNodeGetPressureUnits(MRBFSBusNode* mrbfsNode, const char* optionName)
{
	const char* unitsStr = mrbfsNodeOptionGet(mrbfsNode, optionName, "kPa");
	MRBPressureUnits pressureUnits = MRB_PRESSURE_KPA;
	if (0 == strcmp("hPa", unitsStr))
		pressureUnits = MRB_PRESSURE_HPA;
	else if (0 == strcmp("kPa", unitsStr))
		pressureUnits = MRB_PRESSURE_KPA;
	else if (0 == strcmp("psi", unitsStr))
		pressureUnits = MRB_PRESSURE_PSI;
	else if (0 == strcmp("bar", unitsStr))
		pressureUnits = MRB_PRESSURE_BAR;
	else if (0 == strcmp("Pa", unitsStr))
		pressureUnits = MRB_PRESSURE_PA;
	else if (0 == strcmp("Torr", unitsStr))
		pressureUnits = MRB_PRESSURE_TORR;
	else if (0 == strcmp("inH2O", unitsStr))
		pressureUnits = MRB_PRESSURE_IN_H2O;
	else if (0 == strcmp("inHg", unitsStr))
		pressureUnits = MRB_PRESSURE_IN_HG;
	else if (0 == strcmp("atm", unitsStr))
		pressureUnits = MRB_PRESSURE_STD_ATM;

	return(pressureUnits);
}

const char* mrbfsGetTemperatureDisplayUnits(MRBTemperatureUnits units)
{
	switch(units)
	{
		case MRB_TEMPERATURE_UNITS_C:
			return("C");
		case MRB_TEMPERATURE_UNITS_F:
			return("F");
		case MRB_TEMPERATURE_UNITS_K:
			return("K");
		case MRB_TEMPERATURE_UNITS_R:
			return("R");
	}

	return("Unk");
}

const char* mrbfsGetPressureDisplayUnits(MRBPressureUnits units)
{
	switch(units)
	{
		case MRB_PRESSURE_PA:
			return("Pa");
		case MRB_PRESSURE_HPA:
			return("hPa");
		case MRB_PRESSURE_KPA:
			return("kPa");
		case MRB_PRESSURE_PSI:
			return("psi");
		case MRB_PRESSURE_BAR:
			return("bar");
		case MRB_PRESSURE_TORR:
			return("Torr");
		case MRB_PRESSURE_STD_ATM:
			return("atm");
		case MRB_PRESSURE_IN_H2O:
			return("inH2O");
		case MRB_PRESSURE_IN_HG:
			return("inHg");
	}
	return("Unk");
}

double mrbfsGetPressureFromHPaDouble(double pressure, MRBPressureUnits units)
{
	switch(units)
	{
		case MRB_PRESSURE_PA:
			return(pressure * 100.0);		
		case MRB_PRESSURE_HPA:
			return(pressure);
		case MRB_PRESSURE_KPA:
			return(pressure / 10.0);		
		case MRB_PRESSURE_PSI:
			return(pressure * 0.0145038);
		case MRB_PRESSURE_BAR:
			return(pressure * 1000.0);
		case MRB_PRESSURE_TORR:
			return(pressure * 100.0 / 133.3224);
		case MRB_PRESSURE_STD_ATM:
			return(pressure * 100.0 / 101325.0);
		case MRB_PRESSURE_IN_H2O:
			return(pressure * 0.4015);
		case MRB_PRESSURE_IN_HG:
			return(pressure * 100.0 / 3386.389);
	}
			
	return(pressure);
}

double mrbfsGetPressureFromHPa(const UINT8* pktByte, MRBPressureUnits units)
{
	double pressure = (double)((((unsigned short)pktByte[0])<<8) + (unsigned short)pktByte[1]);
	return(mrbfsGetPressureFromHPaDouble(pressure, units));
}

double mrbfsGetTempFrom16K(const UINT8* pktByte, MRBTemperatureUnits units)
{
	int temperature16K = (((unsigned short)pktByte[0])<<8) + (unsigned short)pktByte[1];
	double temperatureK = (double)temperature16K / 16.0;

	switch(units)
	{
		case MRB_TEMPERATURE_UNITS_K:
			// Do nothing
			break;
		case MRB_TEMPERATURE_UNITS_F:
			temperatureK -= 273.15; // Convert to C, then to F
			temperatureK = (temperatureK * 9.0) / 5.0;
			temperatureK += 32.0;
			break;

		case MRB_TEMPERATURE_UNITS_C:
			temperatureK -= 273.15;
			break;

		case MRB_TEMPERATURE_UNITS_R:
			temperatureK = (temperatureK * 9.0) / 5.0;
			break;

		default:
			temperatureK = 0.0;
			break;
	}
	
	return(temperatureK);
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

int mrbfsNodeQueueTransmitPacket(MRBFSBusNode* mrbfsNode, MRBusPacket* txPkt)
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



