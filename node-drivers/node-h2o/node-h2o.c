/*************************************************************************
Title:    MRBus Filesystem Node Driver for MRB-H2O
Authors:  Nathan Holmes <maverick@drgw.net>
File:     node-h2o.c
License:  GNU General Public License v3

LICENSE:
    Copyright (C) 2013 Nathan Holmes

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License along 
    with this program. If not, see http://www.gnu.org/licenses/
    
*************************************************************************/

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <malloc.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>

#include "slre.h"
#include "mrbfs-module.h"
#include "mrbfs-pktqueue.h"
#include "node-helpers.h"

#define MRBFS_NODE_DRIVER_NAME   "node-h2o"
/*******************************************************
 Internal Helper Function Headers - may or may not be helpful to your module
*******************************************************/

void nodeResetFilesNoData(MRBFSBusNode* mrbfsNode);

// ~83 bytes per packet, and hold 25
#define RX_PKT_BUFFER_SZ  (83 * 25)  
#define OUTPUT_VALUE_BUFFER_SZ 33


/*******************************************************
Structure:  NodeLocalStorage

Purpose:  Holds the current state of this particular node,
 allowing the same module to be used for multiple instances.

 Put anything in here that you need to persist over the life
 of the filesystem.  Your init() function should create this
 thing, as shared libraries can't have global objects (and
 you wouldn't want it global anyway, as there will be multiple
 instances of this thing running).

 Example things you'll want to put in here:
  * pointers to any file nodes you create
  * persistant storage to be updated from incoming packets

*******************************************************/

#define MRB_H2O_MAX_PROGRAMS 64
#define MRB_H2O_MAX_ZONES 16

#define MRB_H2O_PROGRAM_LIST_SZ ((3*MRB_H2O_MAX_PROGRAMS)+1)
#define MRB_H2O_ZONE_LIST_SZ ((3*MRB_H2O_MAX_ZONES)+1)


typedef struct
{
	MRBFSFileNode* file_rxCounter;
	MRBFSFileNode* file_rxPackets;

	MRBFSFileNode* file_programs[MRB_H2O_MAX_PROGRAMS];
	char* programValueStr[MRB_H2O_MAX_PROGRAMS];
	time_t programCacheTimers[MRB_H2O_MAX_PROGRAMS];
	uint32_t cacheSeconds;

	MRBFSFileNode* file_zones[MRB_H2O_MAX_ZONES];
	char* zoneValueStr[MRB_H2O_MAX_ZONES];	

	MRBFSFileNode* file_zoneTimes[MRB_H2O_MAX_ZONES];

	MRBFSFileNode* file_activeZoneList;
	char* activeZoneListValueStr;	

	MRBFSFileNode* file_enabledProgramList;
	char* enabledProgramListValueStr;	
	time_t enabledProgramCacheTimer;

	MRBFSFileNode* file_activeZoneBitmask;
	
	MRBFSFileNode* file_activeProgramList;
	char* activeProgramListValueStr;	

	MRBFSFileNode* file_activeProgramBitmask;	
	char* activeProgramBitmaskValueStr;	

	uint64_t activeProgramBitmask;
		
	MRBFSFileNode* file_busVoltage;
	char* busVoltageValue;

	uint8_t zonesUsed;
	uint8_t programsUsed;
	uint8_t decimalPositions;
	uint8_t suppressUnits;
	uint32_t zoneState;
	
	char rxPacketStr[RX_PKT_BUFFER_SZ];
	uint8_t requestRXFeed;
	MRBusPacketQueue rxq;
	int timeout;
	time_t lastUpdated;	

} NodeLocalStorage;



/*******************************************************
Public Function:  mrbfsNodeDriverVersionCheck()

Called from: Main filesystem process (single threaded mode)

Passed in values:
 ifaceVersion - an integer corresponding to the value of
  MRBFS_NODE_DRIVER_VERSION that the main mrbfs program
  was compiled with

Returned value:
 0 = interface check failed
 1 = interface check passed

Purpose:  Takes in the interface version from the controller
 program and checks it against what this module was compiled
 with.  If it fails, the module returns 0, indicating that
 the interface isn't compatible.  This function shouldn't 
 change the state of this driver in any way.

You theoretically shouldn't need to adjust this for any
reason.

*******************************************************/

int mrbfsNodeDriverVersionCheck(int ifaceVersion)
{
	if (ifaceVersion != MRBFS_NODE_DRIVER_VERSION)
		return(0);
	return(1);
}


/*******************************************************
Pubic Function:  mrbfsFileNodeWrite()

Called from: Main filesystem processes (multithreaded)

Passed in values:
 mrbfsFileNode - a pointer to the file node structure
  for the file being written
 data - string of bytes being written
 dataSz - size of data in bytes

Returned value:
 (none)

Purpose:  This function should be called by any file
 marked "writable" in its filetype.  If set up right,
 the nodeLocalStorage pointer in the MRBFSFileNode
 struct will point at the MRBFSBusNode structure, and
 from there you can get ahold of your node's local storage.

 You'll need to set it as the callback in the fileNode
 structure, and associate the MRBFSBusNode pointer for
 this bus node with the MRBFSFileNode's local storage.

 If your node doesn't implement any writable file nodes,
 then you can omit this function.

*******************************************************/

static void cleanCommandStr(const char* inStr, size_t inStrSz, char* outStr, size_t outStrSz)
{
	uint32_t i, j;

	// Cleanse the incoming command string - remove spaces, tabs, line endings, and other crap
	memset(outStr, 0, outStrSz);
	
	for(i=0, j=0; i<outStrSz-1; i++)
	{
		if (j >= inStrSz)
			break;

		if (inStr[j] == 0x0A || inStr[j] == 0x0C || inStr[j] == 0)
			break;

		if (isprint(inStr[j]))
			outStr[i] = toupper(inStr[j]);

		j++;
	}
}

void mrbfsFileNodeWrite(MRBFSFileNode* mrbfsFileNode, const char* data, int dataSz)
{
	MRBFSBusNode* mrbfsNode = (MRBFSBusNode*)(mrbfsFileNode->nodeLocalStorage);
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)(mrbfsNode->nodeLocalStorage);
	char commandStr[17];
	MRBusPacket txPkt;
	// Set up the packet - initialize and fill in a few key values
	memset(&txPkt, 0, sizeof(MRBusPacket));
	txPkt.bus = mrbfsNode->bus;
	txPkt.pkt[MRBUS_PKT_SRC] = 0;  // A source of 0 will be replaced by the transmit drivers with the interface addresses
	txPkt.pkt[MRBUS_PKT_DEST] = mrbfsNode->address;

	cleanCommandStr(data, dataSz, commandStr, sizeof(commandStr));

	// The fastest way to see what file is getting written is to compare the FileNode pointers
	// to files that we know about.
	if (mrbfsFileNode == nodeLocalStorage->file_rxCounter)
	{
		// Example of a simple file write that resets the packet counter and packet log
		if (0 == atoi(data))
		{
			memset(nodeLocalStorage->rxPacketStr, 0, RX_PKT_BUFFER_SZ);
			mrbfsFileNode->value.valueInt = 0;
		}
	}
	else
	{

	}
	
}




void mrbfsFileZoneWrite(MRBFSFileNode* mrbfsFileNode, const char* data, int dataSz)
{
	MRBFSBusNode* mrbfsNode = (MRBFSBusNode*)(mrbfsFileNode->nodeLocalStorage);
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)(mrbfsNode->nodeLocalStorage);
	int i;
	char commandStr[17];
	MRBusPacket txPkt;
	uint8_t zone = 0xFF;
	uint16_t newRunTime = 0;
	
	cleanCommandStr(data, dataSz, commandStr, sizeof(commandStr));

	for (i=0; i<nodeLocalStorage->zonesUsed; i++)
	{
		if (mrbfsFileNode == nodeLocalStorage->file_zones[i])
		{
			zone = i;
			break;
		}		
	}

	// If we didn't find a file pointer match in the zones, bail
	if (0xFF == zone || zone > 15)
		return;

	newRunTime = atoi(commandStr);

	newRunTime = MIN(newRunTime, 1091);

	// Set up the packet - initialize and fill in a few key values
	memset(&txPkt, 0, sizeof(MRBusPacket));
	txPkt.bus = mrbfsNode->bus;
	txPkt.pkt[MRBUS_PKT_SRC] = 0;  // A source of 0 will be replaced by the transmit drivers with the interface addresses
	txPkt.pkt[MRBUS_PKT_DEST] = mrbfsNode->address;
	txPkt.pkt[MRBUS_PKT_LEN] = 10;
	txPkt.pkt[MRBUS_PKT_TYPE] = 'C';
	txPkt.pkt[MRBUS_PKT_DATA] = 'M';
	txPkt.pkt[MRBUS_PKT_DATA+1] = zone;
	txPkt.pkt[MRBUS_PKT_DATA+2] = 0xFF & (newRunTime / 256);
	txPkt.pkt[MRBUS_PKT_DATA+3] = 0xFF & newRunTime;
	
	if (mrbfsNodeQueueTransmitPacket(mrbfsNode, &txPkt) < 0)
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Node [%s] failed to send packet", mrbfsNode->nodeName);
	
}

void mrbfsFileZoneTimeWrite(MRBFSFileNode* mrbfsFileNode, const char* data, int dataSz)
{
	MRBFSBusNode* mrbfsNode = (MRBFSBusNode*)(mrbfsFileNode->nodeLocalStorage);
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)(mrbfsNode->nodeLocalStorage);
	int i;
	char commandStr[17];
	MRBusPacket txPkt;
	uint8_t zone = 0xFF;
	
	cleanCommandStr(data, dataSz, commandStr, sizeof(commandStr));

	for (i=0; i<nodeLocalStorage->zonesUsed; i++)
	{
		if (mrbfsFileNode == nodeLocalStorage->file_zoneTimes[i])
		{
			zone = i;
			break;
		}		
	}

	// If we didn't find a file pointer match in the zones, bail
	if (0xFF == zone || zone > 15)
		return;


	// Only do a reset if we get a non-numeric or a zero
	if (0 != atoi(commandStr))
		return;

	// Set up the packet - initialize and fill in a few key values
	memset(&txPkt, 0, sizeof(MRBusPacket));
	txPkt.bus = mrbfsNode->bus;
	txPkt.pkt[MRBUS_PKT_SRC] = 0;  // A source of 0 will be replaced by the transmit drivers with the interface addresses
	txPkt.pkt[MRBUS_PKT_DEST] = mrbfsNode->address;
	txPkt.pkt[MRBUS_PKT_LEN] = 9;
	txPkt.pkt[MRBUS_PKT_TYPE] = 'C';
	txPkt.pkt[MRBUS_PKT_DATA] = 'X';
	txPkt.pkt[MRBUS_PKT_DATA+1] = 'Z';
	txPkt.pkt[MRBUS_PKT_DATA+2] = zone;
	
	if (mrbfsNodeQueueTransmitPacket(mrbfsNode, &txPkt) < 0)
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Node [%s] failed to send packet", mrbfsNode->nodeName);
}


void numberListToMask(MRBFSBusNode* mrbfsNode, uint64_t* mask, const char* remainingString)
{
	char oldRemainingString[256];
	char newRemainingString[256];
	char thisNumberStr[16];
	const char* error;
	int thisNumber=0;

	memset(oldRemainingString, 0, sizeof(oldRemainingString));

	strncpy(oldRemainingString, remainingString, sizeof(oldRemainingString)-1);

	while (0 != strlen(oldRemainingString))
	{
		memset(thisNumberStr, 0, sizeof(thisNumberStr));
		memset(newRemainingString, 0, sizeof(newRemainingString));

		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] numberListToMask, input=[%s]", mrbfsNode->nodeName, remainingString);

		error = slre_match(0, "^(\\d+)[ ,]*(.*)",
					oldRemainingString, strlen(oldRemainingString),
					SLRE_STRING,  sizeof(thisNumberStr), thisNumberStr,
					SLRE_STRING,  sizeof(newRemainingString), newRemainingString);

	//	if (NULL != error)
	//		printf("Error non-null, [%s]\n\n", error);

		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] numberListToMask, thisNum=[%s], remaining=[%s]", mrbfsNode->nodeName, thisNumberStr, newRemainingString);

		thisNumber = atoi(thisNumberStr);
		if (thisNumber < 65 && thisNumber > 0)
			*mask |= ((uint64_t)1)<<(thisNumber-1);

		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] numberListToMask, after atoi=[%d]", mrbfsNode->nodeName, thisNumber);

		strncpy(oldRemainingString, newRemainingString, sizeof(oldRemainingString)-1);
	}

	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] numberListToMask returning", mrbfsNode->nodeName);
	return;
}


void mrbfsFileProgramWrite(MRBFSFileNode* mrbfsFileNode, const char* data, int dataSz)
{
	MRBFSBusNode* mrbfsNode = (MRBFSBusNode*)(mrbfsFileNode->nodeLocalStorage);
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)(mrbfsNode->nodeLocalStorage);
	MRBusPacket txPkt;
	int i,j;
	uint8_t program = 0xFF;
	char commandStr[65];
	int startHour=0, startMinute=0, endHour=0, endMinute=0;
	char days[12];
	char zoneStr[256];
	uint64_t zoneMask=0;
	const char *error;

	cleanCommandStr(data, dataSz, commandStr, sizeof(commandStr));

	// Set up the packet - initialize and fill in a few key values
	memset(&txPkt, 0, sizeof(MRBusPacket));
	txPkt.bus = mrbfsNode->bus;
	txPkt.pkt[MRBUS_PKT_SRC] = 0;  // A source of 0 will be replaced by the transmit drivers with the interface addresses
	txPkt.pkt[MRBUS_PKT_DEST] = mrbfsNode->address;
	txPkt.pkt[MRBUS_PKT_LEN] = 16;     // Length of 16
	txPkt.pkt[MRBUS_PKT_TYPE] = 'C';
	txPkt.pkt[MRBUS_PKT_DATA] = 'W';
	
	
	for (i=0; i<nodeLocalStorage->programsUsed; i++)
	{
		if (mrbfsFileNode == nodeLocalStorage->file_programs[i])
		{
			program = i;
			break;
		}		
	}

	// If we didn't find a file pointer match in the programs, bail, don't know where this came from
	if (0xFF == program)
	{
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Node [%s] can't figure out which program I am...", mrbfsNode->nodeName);
		return;
	}
	
	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] right before the fun regex", mrbfsNode->nodeName);
	
	// Program input in the form of HHMM-HHMM DD ZZ,ZZ,ZZ
	memset(days, 0, sizeof(days));
	memset(zoneStr, 0, sizeof(zoneStr));
	
	error = slre_match(0, "^\\s*([012]\\d)([012345]\\d)-([012]\\d)([012345]\\d)\\s+([2UMTWRFS]+)\\s*(.*)",
				commandStr, strlen(commandStr),
				SLRE_INT, sizeof(startHour), &startHour,
				SLRE_INT, sizeof(startMinute), &startMinute,
				SLRE_INT, sizeof(endHour), &endHour,
				SLRE_INT, sizeof(endMinute), &endMinute,
				SLRE_STRING,  sizeof(days), days,
				SLRE_STRING, sizeof(zoneStr), zoneStr);

	if (startHour > 23 || endHour > 23) 
	{
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Node [%s] failed to parse program string [%s] - hours out of range", mrbfsNode->nodeName, commandStr);	
		return;
	}

	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] right after the fun regex - zoneStr=[%s]", mrbfsNode->nodeName, zoneStr);

	zoneMask = 0;
	numberListToMask(mrbfsNode, &zoneMask, zoneStr);

	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] after the number list to mask function", mrbfsNode->nodeName);

	txPkt.pkt[MRBUS_PKT_DATA+1] = (uint8_t)program;
	txPkt.pkt[MRBUS_PKT_DATA+2] = 0; // Config byte, nothing defined here
	txPkt.pkt[MRBUS_PKT_DATA+3] = startHour;
	txPkt.pkt[MRBUS_PKT_DATA+4] = startMinute;	
	txPkt.pkt[MRBUS_PKT_DATA+5] = endHour;
	txPkt.pkt[MRBUS_PKT_DATA+6] = endMinute;	
	txPkt.pkt[MRBUS_PKT_DATA+7] = (uint8_t)(zoneMask>>8);
	txPkt.pkt[MRBUS_PKT_DATA+8] = (uint8_t)(zoneMask);

	for(i=0; i<strlen(days); i++)
	{
		switch(days[i])
		{
			case 'S':
				txPkt.pkt[MRBUS_PKT_DATA+9] |= 0x01;
				break;
			case 'F':
				txPkt.pkt[MRBUS_PKT_DATA+9] |= 0x02;
				break;
			case 'R':
				txPkt.pkt[MRBUS_PKT_DATA+9] |= 0x04;
				break;
			case 'W':
				txPkt.pkt[MRBUS_PKT_DATA+9] |= 0x08;
				break;
			case 'T':
				txPkt.pkt[MRBUS_PKT_DATA+9] |= 0x10;
				break;
			case 'M':
				txPkt.pkt[MRBUS_PKT_DATA+9] |= 0x20;
				break;
			case 'U':
				txPkt.pkt[MRBUS_PKT_DATA+9] |= 0x40;
				break;
			case '2':
				txPkt.pkt[MRBUS_PKT_DATA+9] |= 0x80;
				break;
			default:
				break;
		}
	}

	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] sending program [%d] programming packet", mrbfsNode->nodeName, program);

	if (mrbfsNodeQueueTransmitPacket(mrbfsNode, &txPkt) < 0)
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Node [%s] failed to send packet", mrbfsNode->nodeName);
	
	nodeLocalStorage->programCacheTimers[program] = 0;
	
}

void mrbfsFileEnabledProgramWrite(MRBFSFileNode* mrbfsFileNode, const char* data, int dataSz)
{
	MRBFSBusNode* mrbfsNode = (MRBFSBusNode*)(mrbfsFileNode->nodeLocalStorage);
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)(mrbfsNode->nodeLocalStorage);
	int i;
	char commandStr[256];
	char programs[256];
	char enableCmd[32];
	MRBusPacket txPkt;
	int found = -1;
	uint64_t programMask=0;

	// Set up the packet - initialize and fill in a few key values
	memset(&txPkt, 0, sizeof(MRBusPacket));
	txPkt.bus = mrbfsNode->bus;
	txPkt.pkt[MRBUS_PKT_SRC] = 0;  // A source of 0 will be replaced by the transmit drivers with the interface addresses
	txPkt.pkt[MRBUS_PKT_DEST] = mrbfsNode->address;
	txPkt.pkt[MRBUS_PKT_LEN] = 15;
	txPkt.pkt[MRBUS_PKT_TYPE] = 'C';

	cleanCommandStr(data, dataSz, commandStr, sizeof(commandStr));

	memset(enableCmd, 0, sizeof(enableCmd));
	memset(programs, 0, sizeof(programs));

	// Program input in the form of HHMM-HHMM Zzz....
	slre_match(0, "^\\s*(ENABLE|EN|DISABLE|DIS|SET)\\s*(.*)",
				commandStr, strlen(commandStr),
				SLRE_STRING,  sizeof(enableCmd), enableCmd,
				SLRE_STRING,  sizeof(programs), programs);

	numberListToMask(mrbfsNode, &programMask, programs);

	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] enabledProgramList mask=[%016lX]", mrbfsNode->nodeName, programMask);
	
	for(i=8; i>0; i--)
	{
		txPkt.pkt[MRBUS_PKT_DATA+i] = (uint8_t)programMask;
		programMask >>=8;
	}


	if (0 == strcmp(enableCmd, "ENABLE") || 0 == strcmp(enableCmd, "EN"))
	{
		txPkt.pkt[MRBUS_PKT_DATA] = 'E';
	}
	else if (0 == strcmp(enableCmd, "DISABLE") || 0 == strcmp(enableCmd, "DIS")) 
	{
		txPkt.pkt[MRBUS_PKT_DATA] = 'D';
	}
	else if (0 == strcmp(enableCmd, "SET"))
	{
		txPkt.pkt[MRBUS_PKT_DATA] = 'P';
	}
	else
	{
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Node [%s] did not understand command [%s]", mrbfsNode->nodeName, commandStr);
		return;
	}


	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] sending new H2O enable", mrbfsNode->nodeName);

	if (mrbfsNodeQueueTransmitPacket(mrbfsNode, &txPkt) < 0)
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Node [%s] failed to send packet", mrbfsNode->nodeName);
	
	nodeLocalStorage->enabledProgramCacheTimer = 0;
}



/*******************************************************
Public Function:  mrbfsFileNodeRead()

Called from: Main filesystem processes (multithreaded)

Passed in values:
 mrbfsFileNode - a pointer to the file node structure
  for the file being read
 buf - output buffer for data being sent to program
  reading from this file
 size - size of buffer than can be filled with data
 offset - the offset into the file to read from

Returned value:
 The size (in bytes) of data actually placed into buf

Purpose:  The mrbfsFileNodeRead function is called for 
 files that identify themselves as "readback", meaning
 that it's more than just a simple variable access to 
 get their value.  An example would be things that 
 need to go out to the node and read an eeprom address.
 It must return the size of things written to the buffer, 
 or 0 otherwise.

 You'll need to set it as the callback in the fileNode
 structure, and associate the MRBFSBusNode pointer for
 this bus node with the MRBFSFileNode's local storage.

 If your node doesn't implement any readback file nodes,
 then you can omit this function.

*******************************************************/

int filterProgramReadPkt(MRBusPacket* rxPkt, uint8_t srcAddress, void* otherFilterData)
{
	uint8_t program = *(uint8_t*)otherFilterData;
	if (rxPkt->pkt[MRBUS_PKT_SRC] == srcAddress 
		&& 'c' == rxPkt->pkt[MRBUS_PKT_TYPE]
		&& 'r' == rxPkt->pkt[MRBUS_PKT_DATA]
		&& program == rxPkt->pkt[MRBUS_PKT_DATA+1])
		return(1);
	return(0);
}

size_t mrbfsFileProgramRead(MRBFSFileNode* mrbfsFileNode, char *buf, size_t size, off_t offset)
{
	MRBFSBusNode* mrbfsNode = (MRBFSBusNode*)(mrbfsFileNode->nodeLocalStorage);
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)(mrbfsNode->nodeLocalStorage);
	MRBusPacket rxPkt, txPkt;
	int i;
	uint8_t program = 0xFF;
	int foundResponse = 0;
	char responseBuffer[256];
	size_t len=0;
	time_t currentTime = time(NULL);

	memset(responseBuffer, 0, sizeof(responseBuffer));
	
	for(i=0; i<nodeLocalStorage->programsUsed; i++)
	{
		if (mrbfsFileNode == nodeLocalStorage->file_programs[i])
		{
			program = i;
			break;
		}
	}
	
	// Didn't find that program, bail out
	if (0xFF == program)
		return(0);

	if ((currentTime - nodeLocalStorage->programCacheTimers[program]) <= nodeLocalStorage->cacheSeconds)
	{
		// If the cache is still hot, use it
		strncpy(responseBuffer, nodeLocalStorage->programValueStr[program], sizeof(responseBuffer)-1);
	}
	else
	{
		// Not cached, go over the network to get it
		// Set up the packet - initialize and fill in a few key values
		memset(&txPkt, 0, sizeof(MRBusPacket));
		txPkt.bus = mrbfsNode->bus;    // Important to set the transmit pkt's bus number so it goes to the right transmitters
		txPkt.pkt[MRBUS_PKT_SRC] = 0;  // A source of 0 will be replaced by the transmit drivers with the interface addresses
		txPkt.pkt[MRBUS_PKT_DEST] = mrbfsNode->address; // The destination is the node's current address
		txPkt.pkt[MRBUS_PKT_LEN] = 8;     // Length of 7
		txPkt.pkt[MRBUS_PKT_TYPE] = 'C';  // Packet type of Command
		txPkt.pkt[MRBUS_PKT_DATA] = 'R';  // Subtype of 'R' - program read
		txPkt.pkt[MRBUS_PKT_DATA+1] = (uint8_t)program;  // Subtype of 'R' - program read

		foundResponse = mrbfsNodeTxAndGetResponse(mrbfsNode, &nodeLocalStorage->rxq, &nodeLocalStorage->requestRXFeed, &txPkt, &rxPkt, 1000, 3, &filterProgramReadPkt, (void*)&program);

		// If we didn't get an answer, just log a warning (MRBus is not guaranteed communications, after all)
		// A smarter node could implement retry logic
		if(!foundResponse)
		{
			(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_WARNING, "Node [%s], no response to program %d read request", mrbfsNode->nodeName, program);
			sprintf(responseBuffer, "No data\n");			
			nodeLocalStorage->programCacheTimers[program] = 0;
		} 
		else if (rxPkt.pkt[9] > 23 || rxPkt.pkt[10] > 59 || rxPkt.pkt[11] > 23 || rxPkt.pkt[12] > 59)
		{
			sprintf(responseBuffer, "Bogus program\n");
			strncpy(nodeLocalStorage->programValueStr[program], responseBuffer, OUTPUT_VALUE_BUFFER_SZ-1);
			nodeLocalStorage->programCacheTimers[program]= currentTime;
		}
		else
		{
			char dayBuffer[9], *dayBufferPtr = dayBuffer;
			char zoneBuffer[MRB_H2O_ZONE_LIST_SZ+1], *zoneBufferPtr = zoneBuffer;
			
			memset(dayBuffer, 0, sizeof(dayBuffer));
			memset(zoneBuffer, 0, sizeof(zoneBuffer));
			
			// Actually got a response - parse
			for(i=7; i>=0; i--)
			{
				if ((1<<i) & rxPkt.pkt[15])
				{
					switch(i)
					{
						case 0:
							*dayBufferPtr = 'S';
							break;
						case 1:
							*dayBufferPtr = 'F';
							break;
						case 2:
							*dayBufferPtr = 'R';
							break;
						case 3:
							*dayBufferPtr = 'W';
							break;
						case 4:
							*dayBufferPtr = 'T';
							break;
						case 5:
							*dayBufferPtr = 'M';
							break;
						case 6:
							*dayBufferPtr = 'U';
							break;
						case 7:
							*dayBufferPtr = '2';
							break;
					}
					dayBufferPtr++;
				}
		
			}

			for(i=0; i<MRB_H2O_MAX_ZONES; i++)
			{
				if (rxPkt.pkt[14 - (i/8)] & (1<<(i%8)))
				{
					if (zoneBufferPtr == zoneBuffer)
					{
						sprintf(zoneBufferPtr, "%02d", i+1);
						zoneBufferPtr += 2;
					}
					else
					{
						sprintf(zoneBufferPtr, ",%02d", i+1);
						zoneBufferPtr += 3;
					}
				}
			}

		
			snprintf(responseBuffer, sizeof(responseBuffer)-1, "%02d%02d-%02d%02d %s %s\n", rxPkt.pkt[9], rxPkt.pkt[10], rxPkt.pkt[11], rxPkt.pkt[12], dayBuffer, zoneBuffer);
			strncpy(nodeLocalStorage->programValueStr[program], responseBuffer, OUTPUT_VALUE_BUFFER_SZ-1);
			nodeLocalStorage->programCacheTimers[program] = currentTime;			
		}
	}	
	// This is common read() code that takes whatever's in responseBuffer and puts it into the buffer being
	// given to us by the filesystem
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

int filterEnableReadPkt(MRBusPacket* rxPkt, uint8_t srcAddress, void* otherFilterData)
{
	if (rxPkt->pkt[MRBUS_PKT_SRC] == srcAddress 
		&& 'c' == rxPkt->pkt[MRBUS_PKT_TYPE]
		&& 'p' == rxPkt->pkt[MRBUS_PKT_DATA])
		return(1);
	return(0);
}

size_t mrbfsFileEnabledProgramRead(MRBFSFileNode* mrbfsFileNode, char *buf, size_t size, off_t offset)
{
	MRBFSBusNode* mrbfsNode = (MRBFSBusNode*)(mrbfsFileNode->nodeLocalStorage);
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)(mrbfsNode->nodeLocalStorage);
	MRBusPacket rxPkt, txPkt;
	int i;
	uint8_t program = 0xFF;
	int foundResponse = 0;
	char responseBuffer[MRB_H2O_PROGRAM_LIST_SZ+1];
	size_t len=0;
	time_t currentTime = time(NULL);

	// If it doesn't match the enabled program list file pointer, throw a WTF?
	if (mrbfsFileNode != nodeLocalStorage->file_enabledProgramList)
		return -ENOENT;

	memset(responseBuffer, 0, sizeof(responseBuffer));

	if ((currentTime - nodeLocalStorage->enabledProgramCacheTimer) <= nodeLocalStorage->cacheSeconds)
	{
		// If the cache is still hot, use it
		strncpy(responseBuffer, nodeLocalStorage->enabledProgramListValueStr, sizeof(responseBuffer)-1);
	}
	else
	{
		// Not cached, go over the network to get it
		// Set up the packet - initialize and fill in a few key values
		memset(&txPkt, 0, sizeof(MRBusPacket));
		txPkt.bus = mrbfsNode->bus;    // Important to set the transmit pkt's bus number so it goes to the right transmitters
		txPkt.pkt[MRBUS_PKT_SRC] = 0;  // A source of 0 will be replaced by the transmit drivers with the interface addresses
		txPkt.pkt[MRBUS_PKT_DEST] = mrbfsNode->address; // The destination is the node's current address
		txPkt.pkt[MRBUS_PKT_LEN] = 7;     // Length of 7
		txPkt.pkt[MRBUS_PKT_TYPE] = 'C';  // Packet type of Command
		txPkt.pkt[MRBUS_PKT_DATA] = 'P';  // Subtype of 'G' - enables read

		foundResponse = mrbfsNodeTxAndGetResponse(mrbfsNode, &nodeLocalStorage->rxq, &nodeLocalStorage->requestRXFeed, &txPkt, &rxPkt, 1000, 3, &filterEnableReadPkt, NULL);

		if(!foundResponse)
		{
			(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_WARNING, "Node [%s], no response to program enable read request", mrbfsNode->nodeName);
			sprintf(responseBuffer, "No data\n");			
			nodeLocalStorage->enabledProgramCacheTimer = 0;
		} 
		else
		{
			char* responseBufferPtr = responseBuffer;
			for(i=0; i<64; i++)
			{
				if (rxPkt.pkt[(MRBUS_PKT_DATA+1) + 7 - (i/8)] & (1<<(i%8)))
				{
					if (responseBufferPtr == responseBuffer)
					{
						sprintf(responseBufferPtr, "%02d", i+1);
						responseBufferPtr += 2;
					}
					else
					{
						sprintf(responseBufferPtr, ",%02d", i+1);
						responseBufferPtr += 3;
					}
				}
			}
	
			memset(nodeLocalStorage->enabledProgramListValueStr, 0, MRB_H2O_PROGRAM_LIST_SZ);
			strncpy(nodeLocalStorage->enabledProgramListValueStr, responseBuffer, MRB_H2O_PROGRAM_LIST_SZ-1);
			nodeLocalStorage->enabledProgramCacheTimer= currentTime;
		}
	}

	// This is common read() code that takes whatever's in responseBuffer and puts it into the buffer being
	// given to us by the filesystem
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




int filterZoneTimerReadPkt(MRBusPacket* rxPkt, uint8_t srcAddress, void* otherFilterData)
{
	uint8_t zone = *((uint8_t*)otherFilterData);
	if (rxPkt->pkt[MRBUS_PKT_SRC] == srcAddress 
		&& 'c' == rxPkt->pkt[MRBUS_PKT_TYPE]
		&& 'z' == rxPkt->pkt[MRBUS_PKT_DATA]
		&& zone == rxPkt->pkt[MRBUS_PKT_DATA+1] )
		return(1);
	return(0);
}

size_t mrbfsFileZoneTimeRead(MRBFSFileNode* mrbfsFileNode, char *buf, size_t size, off_t offset)
{
	MRBFSBusNode* mrbfsNode = (MRBFSBusNode*)(mrbfsFileNode->nodeLocalStorage);
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)(mrbfsNode->nodeLocalStorage);
	MRBusPacket rxPkt, txPkt;
	int i;
	uint8_t program = 0xFF;
	int foundResponse = 0;
	char responseBuffer[MRB_H2O_PROGRAM_LIST_SZ+1];
	size_t len=0;
	uint8_t zone = 0xFF;
	uint16_t newRunTime = 0;
	
	for (i=0; i<nodeLocalStorage->zonesUsed; i++)
	{
		if (mrbfsFileNode == nodeLocalStorage->file_zoneTimes[i])
		{
			zone = i;
			break;
		}		
	}

	// If it doesn't match the enabled program list file pointer, throw a WTF?
	if (0xFF == zone)
		return -ENOENT;

	memset(responseBuffer, 0, sizeof(responseBuffer));

	// Set up the packet - initialize and fill in a few key values
	memset(&txPkt, 0, sizeof(MRBusPacket));
	txPkt.bus = mrbfsNode->bus;    // Important to set the transmit pkt's bus number so it goes to the right transmitters
	txPkt.pkt[MRBUS_PKT_SRC] = 0;  // A source of 0 will be replaced by the transmit drivers with the interface addresses
	txPkt.pkt[MRBUS_PKT_DEST] = mrbfsNode->address; // The destination is the node's current address
	txPkt.pkt[MRBUS_PKT_LEN] = 8;     // Length of 7
	txPkt.pkt[MRBUS_PKT_TYPE] = 'C';  // Packet type of Command
	txPkt.pkt[MRBUS_PKT_DATA] = 'Z';  // Subtype of 'Z' - zone time read
	txPkt.pkt[MRBUS_PKT_DATA+1] = zone;

	foundResponse = mrbfsNodeTxAndGetResponse(mrbfsNode, &nodeLocalStorage->rxq, &nodeLocalStorage->requestRXFeed, &txPkt, &rxPkt, 1000, 3, &filterZoneTimerReadPkt, (void*)&zone);

	if(!foundResponse)
	{
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_WARNING, "Node [%s], no response to program enable read request", mrbfsNode->nodeName);
		sprintf(responseBuffer, "No data\n");			
	} 
	else
	{
		snprintf(responseBuffer, sizeof(responseBuffer)-1, "%u%s", ((((uint32_t)rxPkt.pkt[8])<<8) + (uint32_t)rxPkt.pkt[9]), nodeLocalStorage->suppressUnits?"":" min\n" );		
	}

	// This is common read() code that takes whatever's in responseBuffer and puts it into the buffer being
	// given to us by the filesystem
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


size_t mrbfsFileNodeRead(MRBFSFileNode* mrbfsFileNode, char *buf, size_t size, off_t offset)
{
	MRBFSBusNode* mrbfsNode = (MRBFSBusNode*)(mrbfsFileNode->nodeLocalStorage);
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)(mrbfsNode->nodeLocalStorage);
	MRBusPacket pkt;
	int foundResponse = 0;
	char responseBuffer[256] = "";
	size_t len=0;
	
	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] responding to readback on [%s] with [%s]", mrbfsNode->nodeName, mrbfsFileNode->fileName, responseBuffer);

	// This is common read() code that takes whatever's in responseBuffer and puts it into the buffer being
	// given to us by the filesystem
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

/*******************************************************
Public Function:  mrbfsNodeInit()

Called from: Main filesystem process (single threaded mode)

Passed in values:
 mrbfsNode - a pointer to the bus node structure that
  the main program will use to track us.  This roughly
  corresponds to a /busX/nodeX directory, but isn't
  the directory itself.  It's a container filled with
  information about what driver to use for this node
  and where it's local storage is located.

Returned value:
 0 = Success
 -1 = Failure

Purpose:  The mrbfsNodeInit function is called when a
 node is first instantiated.  This may be at filesystem
 startup or later.  Regardless, the mrbfsNode passed in
 will be unique to this particular bus node.  The function
 is responsible for allocating any local storage, setting up
 files to be used by the filesystem, and initializing all
 values.  This is guaranteed to be the first function (that's
 not mrbfsNodeDriverVersionCheck()) called within the
 driver.  Once this function completes, the driver must
 assume that any other public functions could be called on it.

 The corresponding shutdown function is mrbfsNodeDestroy(),
 which must deallocate all memory allocated in mrbfsNodeInit().

*******************************************************/

int mrbfsNodeInit(MRBFSBusNode* mrbfsNode)
{
	NodeLocalStorage* nodeLocalStorage = calloc(1, sizeof(NodeLocalStorage));
	int i;

	// Announce the driver loading to the log
	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] starting up with driver [%s]", mrbfsNode->nodeName, MRBFS_NODE_DRIVER_NAME);

	// If we failed to allocate nodeLocalStorage, we're going for a segfault as things are seriously wrong.  Bail out.
	if (NULL == nodeLocalStorage)
	{
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Node [%s] cannot allocate nodeLocalStorage, dying", mrbfsNode->nodeName);
		return(-1);
	}

	// Associate the storage allocated with the mrbfsNode that the main application tracks and passes back
	// to us every time this node is called
	mrbfsNode->nodeLocalStorage = (void*)nodeLocalStorage;
	nodeLocalStorage->requestRXFeed = 0;

	// Get configuration options from the file
	nodeLocalStorage->timeout = atoi(mrbfsNodeOptionGet(mrbfsNode, "timeout", "none"));
	nodeLocalStorage->decimalPositions = atoi(mrbfsNodeOptionGet(mrbfsNode, "decimal_positions", "2"));
	nodeLocalStorage->suppressUnits = 0;
	if (0 == strcmp(mrbfsNodeOptionGet(mrbfsNode, "suppress_units", "no"), "yes"))
		nodeLocalStorage->suppressUnits = 1;
	nodeLocalStorage->cacheSeconds = atoi(mrbfsNodeOptionGet(mrbfsNode, "cache_seconds", "10"));
	nodeLocalStorage->zonesUsed = atoi(mrbfsNodeOptionGet(mrbfsNode, "zones_used", "16"));
	nodeLocalStorage->programsUsed = atoi(mrbfsNodeOptionGet(mrbfsNode, "programs_used", "64"));

	nodeLocalStorage->lastUpdated = 0;

	// File "rxCounter" - the rxCounter file node will be a simple read/write integer.  Writing a value to it will reset both
	//  it and the rxPackets log file
	nodeLocalStorage->file_rxCounter = mrbfsNodeCreateFile_RW_INT(mrbfsNode, "rxCounter", &mrbfsFileNodeWrite);

	// File "rxPackets" - the rxPackets file node will be a read-only string node that holds a log of the last 25
	//  packets received.  It will be backed by a buffer in nodeLocalStorage.
	nodeLocalStorage->file_rxPackets = (*mrbfsNode->mrbfsFilesystemAddFile)("rxPackets", FNODE_RO_VALUE_STR, mrbfsNode->path);
	nodeLocalStorage->file_rxPackets->value.valueStr = nodeLocalStorage->rxPacketStr;

	// Initialize the 16 zones
	if (nodeLocalStorage->zonesUsed < 0 || nodeLocalStorage->zonesUsed > MRB_H2O_MAX_ZONES)
	{
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Node [%s] - zones_used must be between 0-%d, not [%d] - defaulting to %d", 
			mrbfsNode->nodeName, MRB_H2O_MAX_ZONES, nodeLocalStorage->zonesUsed, MRB_H2O_MAX_ZONES);
		nodeLocalStorage->zonesUsed = MRB_H2O_MAX_ZONES;
	}

	for(i=0; i<nodeLocalStorage->zonesUsed; i++)
	{
		char zoneDefaultFilename[32];
		char zoneKeyname[32];
		char zoneTimeFilename[128];		
		const char* zoneFilename = zoneDefaultFilename;

		sprintf(zoneDefaultFilename, "zone%02d", i+1);
		sprintf(zoneKeyname, "zone%02d_name", i+1);
		zoneFilename = mrbfsNodeOptionGet(mrbfsNode, zoneKeyname, zoneDefaultFilename);

		snprintf(zoneTimeFilename, sizeof(zoneTimeFilename)-1, "%s_totalRuntime", zoneFilename);

		nodeLocalStorage->file_zones[i] = mrbfsNodeCreateFile_RW_STR(mrbfsNode, zoneFilename, &nodeLocalStorage->zoneValueStr[i], OUTPUT_VALUE_BUFFER_SZ, &mrbfsFileZoneWrite);
		nodeLocalStorage->file_zoneTimes[i] = mrbfsNodeCreateFile_RW_READBACK(mrbfsNode, zoneTimeFilename, &mrbfsFileZoneTimeRead, &mrbfsFileZoneTimeWrite);
	}

	nodeLocalStorage->file_activeZoneList = mrbfsNodeCreateFile_RO_STR(mrbfsNode, "activeZoneList", &nodeLocalStorage->activeZoneListValueStr, MRB_H2O_ZONE_LIST_SZ);
	nodeLocalStorage->file_activeZoneBitmask = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "activeZoneBitmask");

	// Initialize the 64 "program" files
	if (nodeLocalStorage->programsUsed < 0 || nodeLocalStorage->programsUsed > MRB_H2O_MAX_PROGRAMS)
	{
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Node [%s] - programs_used must be between 0-%d, not [%d] - defaulting to %d", 
			mrbfsNode->nodeName, MRB_H2O_MAX_PROGRAMS, nodeLocalStorage->programsUsed, MRB_H2O_MAX_PROGRAMS);
		nodeLocalStorage->programsUsed = MRB_H2O_MAX_PROGRAMS;
	}

	for(i=0; i<nodeLocalStorage->programsUsed; i++)
	{
		char programDefaultFilename[32];
		char programKeyname[32];
		const char* programFilename = programDefaultFilename;

		sprintf(programDefaultFilename, "program%02d", i+1);
		sprintf(programKeyname, "program%02d_name", i+1);

		programFilename = mrbfsNodeOptionGet(mrbfsNode, programKeyname, programDefaultFilename);
		nodeLocalStorage->file_programs[i] = mrbfsNodeCreateFile_RW_READBACK(mrbfsNode, programFilename, &mrbfsFileProgramRead, &mrbfsFileProgramWrite);
				// Readbacks generally don't have attached storage - since we use it for caching, allocate and assign here
		nodeLocalStorage->file_programs[i]->value.valueStr = nodeLocalStorage->programValueStr[i] = calloc(1, 256);
	}

	nodeLocalStorage->file_enabledProgramList = mrbfsNodeCreateFile_RW_READBACK(mrbfsNode, "enabledProgramList", &mrbfsFileEnabledProgramRead, &mrbfsFileEnabledProgramWrite);
	nodeLocalStorage->file_enabledProgramList->value.valueStr = nodeLocalStorage->enabledProgramListValueStr = calloc(1, MRB_H2O_PROGRAM_LIST_SZ);

	nodeLocalStorage->file_activeProgramList = mrbfsNodeCreateFile_RO_STR(mrbfsNode, "activeProgramList", &nodeLocalStorage->activeProgramListValueStr, MRB_H2O_PROGRAM_LIST_SZ);
	nodeLocalStorage->file_activeProgramBitmask = mrbfsNodeCreateFile_RO_STR(mrbfsNode, "activeProgramBitmask", &nodeLocalStorage->activeProgramBitmaskValueStr, MRB_H2O_PROGRAM_LIST_SZ);
	nodeLocalStorage->file_busVoltage = mrbfsNodeCreateFile_RO_STR(mrbfsNode, "mrbusVoltage", &nodeLocalStorage->busVoltageValue, OUTPUT_VALUE_BUFFER_SZ);

	nodeResetFilesNoData(mrbfsNode);

	// Return 0 to indicate success
	return (0);
}


/*******************************************************
Public Function:  mrbfsNodeTick()

Called from: Ticker thread (meaning it's asynchronous)

Passed in values:
 mrbfsNode - a pointer to the bus node structure that
  the main program will use to track us.  This roughly
  corresponds to a /busX/nodeX directory, but isn't
  the directory itself.  It's a container filled with
  information about what driver to use for this node
  and where it's local storage is located.
  
 currentTime - the current GMT time expressed in 
  seconds since 1 Jan 1970 (standard time_t).  

Returned value:
 0 = Success
 -1 = Failure

Purpose:  The mrbfsNodeTick() function gets called 
 once per second by a thread running off the main mrbfs
 process.  This function is optional - if the node
 doesn't need to do anything based on a system timer, it
 can omit this method and everything will continue working.

*******************************************************/

int mrbfsNodeTick(MRBFSBusNode* mrbfsNode, time_t currentTime)
{
	NodeLocalStorage* nodeLocalStorage = mrbfsNode->nodeLocalStorage;
	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] received tick", mrbfsNode->nodeName);

	// If the node receive timeout is 0, that means it's not set and data should live forever
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


/*******************************************************
Public Function:  mrbfsNodeDestroy()

Called from: Main filesystem process (single threaded mode)

Passed in values:
 mrbfsNode - a pointer to the bus node structure that
  the main program will use to track us.  This roughly
  corresponds to a /busX/nodeX directory, but isn't
  the directory itself.  It's a container filled with
  information about what driver to use for this node
  and where it's local storage is located.

Returned value:
 0 = Success
 -1 = Failure

Purpose:  The mrbfsNodeDestroy() function is called as a
 given node is being taken out of the filesystem to deallocate
 any memory that its corresponding mrbfsNodeInit() call
 allocated.

 Currently this is relatively unimportant, as the only way
 that nodes go away is to shut down the filesystem.  That
 inherently trashes any memory we had, so it's not a big deal.

 For future use where nodes can dynamically go in and out of
 the bus, please fill this function in, however.

*******************************************************/

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



/*******************************************************
Public Function:  mrbfsNodeRxPacket()

Called from: Interface driver threads

Passed in values:
 mrbfsNode - a pointer to the bus node structure that
  the main program will use to track us.  This roughly
  corresponds to a /busX/nodeX directory, but isn't
  the directory itself.  It's a container filled with
  information about what driver to use for this node
  and where it's local storage is located.

 rxPkt - a pointer to an MRBusPacket structure containing
  a packet that was just received from this node number
  by some interface

Returned value:
 0 = Success
 -1 = Failure

Purpose:  The mrbfsNodeRxPacket() function is called by 
 various interface driver threads as they receive packets
 from the bus that are sent by this node.

 Be very careful.  This function may be called simultaneously 
 by multiple packet receivers.  Make sure anything affecting
 the node as a whole is interlocked with mutexes or is simple
 integer interactions (for practical purposes, integer ops are
 atomic)

*******************************************************/

int mrbfsNodeRxPacket(MRBFSBusNode* mrbfsNode, MRBusPacket* rxPkt)
{
	int i;
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)mrbfsNode->nodeLocalStorage;

	// Store off the current local time that the packet was processed.  System time calls
	// tend to be kind of heavy, so let's not do this more than needed.
	time_t currentTime = time(NULL);

	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] received packet", mrbfsNode->nodeName);

	// Mutex the mrbfsNode to make sure we're the only one talking to it right now
	pthread_mutex_lock(&mrbfsNode->nodeLock);

	switch(rxPkt->pkt[MRBUS_PKT_TYPE])
	{
		case 'S':
		{
			uint32_t zoneState = nodeLocalStorage->file_activeZoneBitmask->value.valueInt = (((uint32_t)rxPkt->pkt[14])<<8) + (uint32_t)rxPkt->pkt[15];
			char* zoneList = nodeLocalStorage->activeZoneListValueStr;
			char* programList = nodeLocalStorage->activeProgramListValueStr;
							
			memset(nodeLocalStorage->activeZoneListValueStr, 0, MRB_H2O_ZONE_LIST_SZ);
			for(i=0; i<nodeLocalStorage->zonesUsed; i++)
			{
				if (zoneState & 1<<(i))
				{
					strcpy(nodeLocalStorage->zoneValueStr[i], "On");
					if (zoneList == nodeLocalStorage->activeZoneListValueStr)
					{
						sprintf(zoneList, "%02d", i+1);
						zoneList += 2;
					}
					else
					{
						sprintf(zoneList, ",%02d", i+1);
						zoneList += 3;
					}
				}
				else
					strcpy(nodeLocalStorage->zoneValueStr[i], "Off");		
			}

			nodeLocalStorage->activeProgramBitmask = 0;
			for (i=0; i<8; i++)
			{
				nodeLocalStorage->activeProgramBitmask <<= 8;
				nodeLocalStorage->activeProgramBitmask += rxPkt->pkt[6+i];
			}
			
			sprintf(nodeLocalStorage->activeProgramBitmaskValueStr, "%" PRIu64 "\n", nodeLocalStorage->activeProgramBitmask);
			memset(nodeLocalStorage->activeProgramListValueStr, 0, MRB_H2O_PROGRAM_LIST_SZ);
			for(i=0; i<nodeLocalStorage->programsUsed; i++)
			{
				if (nodeLocalStorage->activeProgramBitmask & ((uint64_t)1)<<(i))
				{
					if (programList == nodeLocalStorage->activeProgramListValueStr)
					{
						sprintf(programList, "%02d", i+1);
						programList += 2;
					}
					else
					{
						sprintf(programList, ",%02d", i+1);
						programList += 3;
					}
				}
			}

			nodeLocalStorage->lastUpdated = currentTime;			
			snprintf(nodeLocalStorage->busVoltageValue, OUTPUT_VALUE_BUFFER_SZ-1, "%.*f%s", nodeLocalStorage->decimalPositions, ((double)rxPkt->pkt[16])/10.0, nodeLocalStorage->suppressUnits?"":" V\n" );
			nodeLocalStorage->file_busVoltage->updateTime = currentTime;				
		}
		break;			
	}

	// If some filesystem function (usually a readback file waiting for a response) has requested we
	// give them a feed, push the current packet onto our receive queue
	if (nodeLocalStorage->requestRXFeed)
		mrbusPacketQueuePush(&nodeLocalStorage->rxq, rxPkt, 0);

	// Log the receipt of the current packet into the buffer backing "rxPackets"
	{
		size_t rxPacketLen = strlen(nodeLocalStorage->rxPacketStr), newLen=rxPacketLen;
		char newPacket[100];
		int b;
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

	// Update the number of packets received and the file time, reflecting the time the packet was received
	nodeLocalStorage->file_rxCounter->updateTime = currentTime;
	nodeLocalStorage->file_rxCounter->value.valueInt++;
	// Release our lock, we're done updating things
	pthread_mutex_unlock(&mrbfsNode->nodeLock);
	return(0);
}

/*******************************************************
Internal Function:  nodeResetFilesNoData()

Purpose: Node helper function that resets the default values for files
 This way the timeout/default/no data values are easily shared between
 the initialization functions and the timeout call from mrbfsNodeTick().

*******************************************************/

void nodeResetFilesNoData(MRBFSBusNode* mrbfsNode)
{
	NodeLocalStorage* nodeLocalStorage = mrbfsNode->nodeLocalStorage;
	int i;
	
	for(i=0; i<nodeLocalStorage->zonesUsed; i++)
		strcpy(nodeLocalStorage->zoneValueStr[i], "No Data\n");

	for(i=0; i<nodeLocalStorage->programsUsed; i++)
		strcpy(nodeLocalStorage->programValueStr[i], "No Data\n");

	for(i=0; i<MRB_H2O_MAX_PROGRAMS; i++)
		nodeLocalStorage->programCacheTimers[i] = 0;

	strcpy(nodeLocalStorage->busVoltageValue, "No Data\n");
	strcpy(nodeLocalStorage->activeZoneListValueStr, "No Data\n");
	nodeLocalStorage->file_activeZoneBitmask->value.valueInt = 0;

	strcpy(nodeLocalStorage->activeProgramListValueStr, "No Data\n");
	strcpy(nodeLocalStorage->activeProgramBitmaskValueStr, "0\n");
	nodeLocalStorage->activeProgramBitmask = 0;
}




