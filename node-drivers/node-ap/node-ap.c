/*************************************************************************
Title:    MRBus Filesystem MRB-AP Node Driver
Authors:  Nathan Holmes <maverick@drgw.net>
File:     node-template.c
License:  GNU General Public License v3

LICENSE:
    Copyright (C) 2012 Nathan Holmes

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
#include "mrbfs-module.h"
#include "mrbfs-pktqueue.h"
#include "node-helpers.h"

#define MRBFS_NODE_DRIVER_NAME   "node-ap"

/*******************************************************
 Internal Helper Function Headers - may or may not be helpful to your module
*******************************************************/

void nodeResetFilesNoData(MRBFSBusNode* mrbfsNode);
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

#define SENSOR_VALUE_BUFFER_SZ 33

typedef struct
{
	MRBFSFileNode* file_wiredPackets;
	MRBFSFileNode* file_wirelessPackets;
	MRBFSFileNode* file_busVoltage;
	char busVoltageValue[SENSOR_VALUE_BUFFER_SZ];
	MRBFSFileNode* file_rxCounter;
	MRBFSFileNode* file_rxPackets;

	UINT8 suppressUnits;
	UINT8 decimalPositions;
	char rxPacketStr[RX_PKT_BUFFER_SZ];
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
			mrbfsFileNode->value.valueInt = 0;
		}
	}
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
	int nodeOccupancyDetectorsConnected;
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

	// Initialize pieces of the local storage and create the files our node will use to communicate with the user
	nodeLocalStorage->lastUpdated = 0;
	nodeLocalStorage->timeout = atoi(mrbfsNodeOptionGet(mrbfsNode, "timeout", "none"));
	nodeLocalStorage->suppressUnits = (0 == strcmp(mrbfsNodeOptionGet(mrbfsNode, "suppress_units", "no"), "yes"))?1:0;
	nodeLocalStorage->decimalPositions = atoi(mrbfsNodeOptionGet(mrbfsNode, "decimal_positions", "2"));	

	// File "rxCounter" - the rxCounter file node will be a simple read/write integer.  Writing a value to it will reset both
	//  it and the rxPackets log file
	nodeLocalStorage->file_rxCounter = (*mrbfsNode->mrbfsFilesystemAddFile)("rxCounter", FNODE_RW_VALUE_INT, mrbfsNode->path);
	nodeLocalStorage->file_rxCounter->value.valueInt = 0; // Initialize the value - initially on load we've seen no packets
	nodeLocalStorage->file_rxCounter->mrbfsFileNodeWrite = &mrbfsFileNodeWrite; // Associate this node's mrbfsFileNodeWrite for write callbacks
	nodeLocalStorage->file_rxCounter->nodeLocalStorage = (void*)mrbfsNode;  // Associate this node's memory with the filenode's local storage

	// File "rxPackets" - the rxPackets file node will be a read-only string node that holds a log of the last 25
	//  packets received.  It will be backed by a buffer in nodeLocalStorage.
	nodeLocalStorage->file_rxPackets = (*mrbfsNode->mrbfsFilesystemAddFile)("rxPackets", FNODE_RO_VALUE_STR, mrbfsNode->path);
	nodeLocalStorage->file_rxPackets->value.valueStr = nodeLocalStorage->rxPacketStr;

	nodeLocalStorage->file_wiredPackets = (*mrbfsNode->mrbfsFilesystemAddFile)("wiredPackets", FNODE_RO_VALUE_INT, mrbfsNode->path);
	nodeLocalStorage->file_wiredPackets->value.valueInt = 0; // Initialize the value - initially on load we've seen no packets

	nodeLocalStorage->file_wirelessPackets = (*mrbfsNode->mrbfsFilesystemAddFile)("wirelessPackets", FNODE_RO_VALUE_INT, mrbfsNode->path);
	nodeLocalStorage->file_wirelessPackets->value.valueInt = 0;

	nodeLocalStorage->file_busVoltage = (*mrbfsNode->mrbfsFilesystemAddFile)("mrbus_voltage", FNODE_RO_VALUE_STR, mrbfsNode->path);
	nodeLocalStorage->file_busVoltage->value.valueStr = nodeLocalStorage->busVoltageValue;


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
	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] received tick", mrbfsNode->nodeName);

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

void populateVoltageFile(NodeLocalStorage* nodeLocalStorage, double busVoltage, time_t currentTime)
{
	snprintf(nodeLocalStorage->busVoltageValue, SENSOR_VALUE_BUFFER_SZ-1, "%.*f%s", nodeLocalStorage->decimalPositions, busVoltage, nodeLocalStorage->suppressUnits?"":" V\n" );
	nodeLocalStorage->file_busVoltage->updateTime = currentTime;
}

unsigned int pktToUint32(unsigned char* pkt)
{
	unsigned int v = 0;
	v = ((unsigned int)pkt[0])<<24;
	v += ((unsigned int)pkt[1])<<16;
	v += ((unsigned int)pkt[2])<<8;
	v += ((unsigned int)pkt[3]);
	return (v);
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

	// Store off the current local time that the packet was processed.  System time calls
	// tend to be kind of heavy, so let's not do this more than needed.
	time_t currentTime = time(NULL);

	(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] received packet", mrbfsNode->nodeName);

	// Mutex the mrbfsNode to make sure we're the only one talking to it right now
	pthread_mutex_lock(&mrbfsNode->nodeLock);

	switch(rxPkt->pkt[MRBUS_PKT_TYPE])
	{
		case 'S':
			nodeLocalStorage->lastUpdated = currentTime;
			nodeLocalStorage->file_wiredPackets->updateTime = currentTime;
			nodeLocalStorage->file_wirelessPackets->updateTime = currentTime;
			nodeLocalStorage->file_wiredPackets->value.valueInt = pktToUint32(rxPkt->pkt + 6);
			nodeLocalStorage->file_wirelessPackets->value.valueInt = pktToUint32(rxPkt->pkt + 10);
			populateVoltageFile(nodeLocalStorage, ((double)rxPkt->pkt[14])/10.0, currentTime);
			break;			
	}

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

/*******************************************************
Internal Function:  nodeResetFilesNoData()

Purpose: Node helper function that resets the default values for files
 This way the timeout/default/no data values are easily shared between
 the initialization functions and the timeout call from mrbfsNodeTick().

*******************************************************/

void nodeResetFilesNoData(MRBFSBusNode* mrbfsNode)
{
	NodeLocalStorage* nodeLocalStorage = mrbfsNode->nodeLocalStorage;
//	strcpy(nodeLocalStorage->tempSensorValue, "No Data\n");
}





