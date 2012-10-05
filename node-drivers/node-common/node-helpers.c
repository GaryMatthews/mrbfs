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

double mrbfsGetTempFrom16K(const UINT8* pktByte)
{
	int temperatureK = (((unsigned short)rxPkt->pkt[0])<<8) + rxPkt->pkt[1];
	return((double)temperatureK / 16.0);
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



