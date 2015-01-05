/*************************************************************************
Title:    MRBus Filesystem MRB-IIAB Driver
Authors:  Nathan Holmes <maverick@drgw.net>
File:     node-iiab.c
License:  GNU General Public License v3

LICENSE:
    Copyright (C) 2014 Nathan Holmes

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
#include "mrbfs-module.h"
#include "mrbfs-pktqueue.h"
#include "node-helpers.h"

#define MRBFS_NODE_DRIVER_NAME   "node-iiab"
#define OUTPUT_VALUE_BUFFER_SZ 33
/*******************************************************
 Internal Helper Function Headers - may or may not be helpful to your module
*******************************************************/

void nodeResetFilesNoData(MRBFSBusNode* mrbfsNode);
void mrbfsFileNodeWrite(MRBFSFileNode* mrbfsFileNode, const char* data, int dataSz);
// ~83 bytes per packet, and hold 25
#define RX_PKT_BUFFER_SZ  (83 * 25)  




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

typedef struct
{
	MRBFSFileNode* file_rxCounter;
	MRBFSFileNode* file_rxPackets;
	MRBFSFileNode* file_occ[7];
	MRBFSFileNode* file_switchPos[4];
	char* switchPosValue[4];
	MRBFSFileNode* file_soundOut[2];
	MRBFSFileNode* file_state[4];
	MRBFSFileNode* file_simEnable[4];
	MRBFSFileNode* file_timelockTimer;
	MRBFSFileNode* file_debounceTimer;
	MRBFSFileNode* file_signalAspect[8];
	MRBFSFileNode* file_timeoutTimerStatus[4];
	MRBFSFileNode* file_lockoutTimerStatus[4];

	char* signalAspectValue[8];
	MRBFSFileNode* file_busVoltage;
	char* busVoltageValue;	
	
	
	char rxPacketStr[RX_PKT_BUFFER_SZ];
	pthread_mutex_t rxFeedLock;
	volatile uint8_t requestRXFeed;
	MRBusPacketQueue rxq;
	int timeout;
	time_t lastUpdated;	
	uint8_t decimalPositions;
	uint8_t suppressUnits;
} NodeLocalStorage;

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

	// Read-back functions need access to the receive queue, which needs a lock.  Hey look, here's a lock!
	nodeLocalStorage->requestRXFeed = 0;
	mrbfsNodeMutexInit(&nodeLocalStorage->rxFeedLock);

	// Initialize pieces of the local storage and create the files our node will use to communicate with the user
	nodeLocalStorage->timeout = atoi(mrbfsNodeOptionGet(mrbfsNode, "timeout", "none"));
	nodeLocalStorage->lastUpdated = 0;

	nodeLocalStorage->decimalPositions = atoi(mrbfsNodeOptionGet(mrbfsNode, "decimal_positions", "2"));
	nodeLocalStorage->suppressUnits = 0;
	if (0 == strcmp(mrbfsNodeOptionGet(mrbfsNode, "suppress_units", "no"), "yes"))
		nodeLocalStorage->suppressUnits = 1;

	// File "rxCounter" - the rxCounter file node will be a simple read/write integer.  Writing a value to it will reset both
	//  it and the rxPackets log file
	nodeLocalStorage->file_rxCounter = mrbfsNodeCreateFile_RW_INT(mrbfsNode, "rxCounter", &mrbfsFileNodeWrite);
	
	// File "rxPackets" - the rxPackets file node will be a read-only string node that holds a log of the last 25
	//  packets received.  It will be backed by a buffer in nodeLocalStorage.
	nodeLocalStorage->file_rxPackets = (*mrbfsNode->mrbfsFilesystemAddFile)("rxPackets", FNODE_RO_VALUE_STR, mrbfsNode->path);
	nodeLocalStorage->file_rxPackets->value.valueStr = nodeLocalStorage->rxPacketStr;

	for(i=0; i<7; i++)
	{
		char occupancyDefaultFilename[32];
		char occupancyKeyname[32];
		const char* occupancyFilename = occupancyDefaultFilename;

		sprintf(occupancyKeyname, "occ_%d_name", i+1);
		switch(i)
		{
			case 0:
				sprintf(occupancyDefaultFilename, "occ_dir0_main");
				break;
			case 1:
				sprintf(occupancyDefaultFilename, "occ_dir0_siding");
				break;
			case 2:
				sprintf(occupancyDefaultFilename, "occ_dir1_main");
				break;
			case 3:
				sprintf(occupancyDefaultFilename, "occ_dir2_main");
				break;
			case 4:
				sprintf(occupancyDefaultFilename, "occ_dir2_siding");
				break;
			case 5:
				sprintf(occupancyDefaultFilename, "occ_dir3_main");
				break;
			case 6:
				sprintf(occupancyDefaultFilename, "occ_interlocking_plant");
				break;
			default:
				sprintf(occupancyDefaultFilename, "occ_%d", i+1);
				break;
		}

		occupancyFilename = mrbfsNodeOptionGet(mrbfsNode, occupancyKeyname, occupancyDefaultFilename);
		nodeLocalStorage->file_occ[i] = (*mrbfsNode->mrbfsFilesystemAddFile)(occupancyFilename, FNODE_RO_VALUE_INT, mrbfsNode->path);
	}

	for(i=0; i<4; i++)
	{
		char switchPosDefaultFilename[32];
		char switchPosKeyname[32];
		const char* switchPosFilename = switchPosDefaultFilename;

		sprintf(switchPosKeyname, "turnout_%d_name", i+1);
		switch(i)
		{
			case 0:
				sprintf(switchPosDefaultFilename, "turnout_dir0");
				break;
			case 1:
				sprintf(switchPosDefaultFilename, "turnout_dir1");
				break;
			case 2:
				sprintf(switchPosDefaultFilename, "turnout_dir2");
				break;
			case 3:
				sprintf(switchPosDefaultFilename, "turnout_dir3");
				break;
			default:
				sprintf(switchPosDefaultFilename, "turnout_%d", i+1);
				break;
		}

		switchPosFilename = mrbfsNodeOptionGet(mrbfsNode, switchPosKeyname, switchPosDefaultFilename);
		nodeLocalStorage->file_switchPos[i] = mrbfsNodeCreateFile_RO_STR(mrbfsNode, switchPosFilename, &nodeLocalStorage->switchPosValue[i], OUTPUT_VALUE_BUFFER_SZ);
	}

	for(i=0; i<sizeof(nodeLocalStorage->signalAspectValue)/sizeof(nodeLocalStorage->signalAspectValue[0]); i++)
	{
		char signalAspectDefaultFilename[32];
		char signalAspectKeyname[32];
		const char* signalAspectFilename = signalAspectDefaultFilename;

		sprintf(signalAspectKeyname, "signalhead_%d_name", i);
		switch(i)
		{
			// FIXME:  Fill signal head default names in here
			default:
				sprintf(signalAspectDefaultFilename, "signalhead_%d", i);
				break;
		}

		signalAspectFilename = mrbfsNodeOptionGet(mrbfsNode, signalAspectKeyname, signalAspectDefaultFilename);
		nodeLocalStorage->file_signalAspect[i] = mrbfsNodeCreateFile_RO_STR(mrbfsNode, signalAspectFilename, &nodeLocalStorage->signalAspectValue[i], OUTPUT_VALUE_BUFFER_SZ);
	}

	nodeLocalStorage->file_soundOut[0] = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "soundOut0");
	nodeLocalStorage->file_soundOut[1] = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "soundOut1");

	nodeLocalStorage->file_state[0] = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "stateMachine0");
	nodeLocalStorage->file_state[1] = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "stateMachine1");
	nodeLocalStorage->file_state[2] = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "stateMachine2");
	nodeLocalStorage->file_state[3] = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "stateMachine3");

	nodeLocalStorage->file_simEnable[0] = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "simEnable0");
	nodeLocalStorage->file_simEnable[1] = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "simEnable1");
	nodeLocalStorage->file_simEnable[2] = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "simEnable2");
	nodeLocalStorage->file_simEnable[3] = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "simEnable3");

	nodeLocalStorage->file_timeoutTimerStatus[0] = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "timeoutStatus0");
	nodeLocalStorage->file_timeoutTimerStatus[1] = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "timeoutStatus1");
	nodeLocalStorage->file_timeoutTimerStatus[2] = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "timeoutStatus2");
	nodeLocalStorage->file_timeoutTimerStatus[3] = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "timeoutStatus3");

	nodeLocalStorage->file_lockoutTimerStatus[0] = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "lockoutStatus0");
	nodeLocalStorage->file_lockoutTimerStatus[1] = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "lockoutStatus1");
	nodeLocalStorage->file_lockoutTimerStatus[2] = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "lockoutStatus2");
	nodeLocalStorage->file_lockoutTimerStatus[3] = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "lockoutStatus3");

	nodeLocalStorage->file_timelockTimer = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "timelockTimer");
	nodeLocalStorage->file_debounceTimer = mrbfsNodeCreateFile_RO_INT(mrbfsNode, "debounceTimer");


	nodeLocalStorage->file_busVoltage = mrbfsNodeCreateFile_RO_STR(mrbfsNode, "mrbusVoltage", &nodeLocalStorage->busVoltageValue, OUTPUT_VALUE_BUFFER_SZ);

	// Return 0 to indicate success
	return (0);
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
	
	strcpy(nodeLocalStorage->file_busVoltage->value.valueStr, "No Data\n");
	strcpy(nodeLocalStorage->file_busVoltage->value.valueStr, "No Data\n");

	for(i=0; i<7; i++)
		nodeLocalStorage->file_occ[i]->value.valueInt = 0;

	for(i=0; i<8; i++)
		strcpy(nodeLocalStorage->file_signalAspect[i]->value.valueStr, "No Data\n");
	
	for(i=0; i<2; i++)
		nodeLocalStorage->file_soundOut[i]->value.valueInt = 0;

	for(i=0; i<4; i++)
		nodeLocalStorage->file_state[i]->value.valueInt = 0;

	for(i=0; i<4; i++)
		nodeLocalStorage->file_simEnable[i]->value.valueInt = 0;

	nodeLocalStorage->file_timelockTimer->value.valueInt = 0;
	nodeLocalStorage->file_debounceTimer->value.valueInt = 0;
}




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

void mrbfsFileNodeWrite(MRBFSFileNode* mrbfsFileNode, const char* data, int dataSz)
{
	MRBFSBusNode* mrbfsNode = (MRBFSBusNode*)(mrbfsFileNode->nodeLocalStorage);
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)(mrbfsNode->nodeLocalStorage);


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

// filterEepromReadPkt is a mrbfsRxPktFilter that will be used in mrbfsFileNodeRead

static int filterEepromReadPkt(MRBusPacket* rxPkt, uint8_t srcAddress, void* otherFilterData)
{
	uint8_t eepromAddressToRead = *(uint8_t*)otherFilterData;
	if (rxPkt->pkt[MRBUS_PKT_SRC] == srcAddress 
		&& 'r' == rxPkt->pkt[MRBUS_PKT_TYPE]
		&& eepromAddressToRead == rxPkt->pkt[MRBUS_PKT_DATA])
		return(1);
	return(0);
}

size_t mrbfsFileNodeRead(MRBFSFileNode* mrbfsFileNode, char *buf, size_t size, off_t offset)
{
	MRBFSBusNode* mrbfsNode = (MRBFSBusNode*)(mrbfsFileNode->nodeLocalStorage);
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)(mrbfsNode->nodeLocalStorage);
	MRBusPacket rxPkt;
	int timeout = 0;
	int foundResponse = 0;
	char responseBuffer[256];
	size_t len=0;

	memset(responseBuffer, 0, sizeof(responseBuffer));
	
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
	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_ANNOYING, "Node [%s] received tick", mrbfsNode->nodeName);

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
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)mrbfsNode->nodeLocalStorage;
	int i;
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
				for (i=0; i<7; i++)
				{
					nodeLocalStorage->file_occ[i]->value.valueInt = (rxPkt->pkt[6] & (1<<i))?1:0;	
					nodeLocalStorage->file_occ[i]->updateTime = currentTime;
				}

				for (i=0; i<4; i++)
				{
					strcpy(nodeLocalStorage->file_switchPos[i]->value.valueStr, (rxPkt->pkt[7] & (1<<i))?"diverging":"normal");;	
					nodeLocalStorage->file_switchPos[i]->updateTime = currentTime;
				}
				
				for (i=6; i<8; i++)
				{
					nodeLocalStorage->file_soundOut[i-6]->value.valueInt = (rxPkt->pkt[11] & (1<<i))?1:0;
					nodeLocalStorage->file_soundOut[i-6]->updateTime = currentTime;					
				}

				for (i=0; i<4; i++)
				{
					nodeLocalStorage->file_simEnable[i]->value.valueInt = (rxPkt->pkt[11] & (1<<i))?1:0;	
					nodeLocalStorage->file_simEnable[i]->updateTime = currentTime;
					
					nodeLocalStorage->file_lockoutTimerStatus[i]->value.valueInt = (rxPkt->pkt[16] & (1<<i))?1:0;
					nodeLocalStorage->file_lockoutTimerStatus[i]->updateTime = currentTime;
					
					nodeLocalStorage->file_timeoutTimerStatus[i]->value.valueInt = (rxPkt->pkt[17] & (1<<i))?1:0;
					nodeLocalStorage->file_timeoutTimerStatus[i]->updateTime = currentTime;
				}

				for(i=0; i<8; i++)
				{
					uint8_t aspectWires = 0;
					
					aspectWires |= rxPkt->pkt[10 - ((i*3)/8)] & (1<<((i*3)%8))?1:0;
					aspectWires |= rxPkt->pkt[10 - ((i*3 + 1)/8)] & (1<<((i*3 + 1)%8))?2:0;
					aspectWires |= rxPkt->pkt[10 - ((i*3 + 2)/8)] & (1<<((i*3 + 2)%8))?4:0;
					
					switch(aspectWires)
					{
						case 0x04:
							strcpy(nodeLocalStorage->file_signalAspect[i]->value.valueStr, "Red\n");
							break;
						case 0x02:
							strcpy(nodeLocalStorage->file_signalAspect[i]->value.valueStr, "Yellow\n");
							break;
						case 0x01:
							strcpy(nodeLocalStorage->file_signalAspect[i]->value.valueStr, "Green\n");
							break;
						default:
							sprintf(nodeLocalStorage->file_signalAspect[i]->value.valueStr, "Unknown [0x%02X]\n", aspectWires);
							break;
					}
					nodeLocalStorage->file_signalAspect[i]->updateTime = currentTime;
				
				}

				nodeLocalStorage->file_state[0]->value.valueInt = 0x0F & (rxPkt->pkt[12]>>4);
				nodeLocalStorage->file_state[1]->value.valueInt = 0x0F & (rxPkt->pkt[12]);
				nodeLocalStorage->file_state[2]->value.valueInt = 0x0F & (rxPkt->pkt[13]>>4);
				nodeLocalStorage->file_state[3]->value.valueInt = 0x0F & (rxPkt->pkt[13]);								

				for(i=0; i<4; i++)
					nodeLocalStorage->file_state[i]->updateTime = currentTime;

				nodeLocalStorage->file_timelockTimer->value.valueInt = rxPkt->pkt[14];
				nodeLocalStorage->file_timelockTimer->updateTime = currentTime;
				nodeLocalStorage->file_debounceTimer->value.valueInt = rxPkt->pkt[15];
				nodeLocalStorage->file_debounceTimer->updateTime = currentTime;
				
				snprintf(nodeLocalStorage->busVoltageValue, OUTPUT_VALUE_BUFFER_SZ-1, "%.*f%s", nodeLocalStorage->decimalPositions, ((double)rxPkt->pkt[19])/10.0, nodeLocalStorage->suppressUnits?"":" V\n" );
				nodeLocalStorage->file_busVoltage->updateTime = currentTime;

				nodeLocalStorage->lastUpdated = currentTime;			
			}
			break;			
	}

	// If some filesystem function (usually a readback file waiting for a response) has requested we
	// give them a feed, push the current packet onto our receive queue
	if (nodeLocalStorage->requestRXFeed)
		mrbusPacketQueuePush(&nodeLocalStorage->rxq, rxPkt, 0);
/*
	typedef struct
	{
		uint32_t logDepth;
		uint32_t headIdx;
		MRBusPacket* pkt;
	} MRBFSPktLog;
*/
	// Log the receipt of the current packet into the buffer backing "rxPackets"
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

	// Update the number of packets received and the file time, reflecting the time the packet was received
	nodeLocalStorage->file_rxCounter->updateTime = currentTime;
	nodeLocalStorage->file_rxCounter->value.valueInt++;
	// Release our lock, we're done updating things
	pthread_mutex_unlock(&mrbfsNode->nodeLock);
	return(0);
}



