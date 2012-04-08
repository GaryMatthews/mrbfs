/*************************************************************************
Title:    MRBus Filesystem Node Driver Template
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

#define MRBFS_NODE_DRIVER_NAME   "node-template"

/*******************************************************
 Internal Helper Function Headers - may or may not be helpful to your module
*******************************************************/

int trimNewlines(char* str, int trimval);
int nodeQueueTransmitPacket(MRBFSBusNode* mrbfsNode, MRBusPacket* txPkt);
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
	MRBFSFileNode* file_eepromNodeAddr;
	MRBFSFileNode* file_sendPing;
	char rxPacketStr[RX_PKT_BUFFER_SZ];
	UINT8 requestRXFeed;
	MRBusPacketQueue rxq;
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
	else if (mrbfsFileNode == nodeLocalStorage->file_sendPing)
	{
		// Example of a more complex scenario, where we want to send a bus packet in response
		// to a write.  Normally this would do something useful, but in the interest of safety,
      // the example will just send a ping.

		MRBusPacket txPkt;

		// Set up the packet - initialize and fill in a few key values
		memset(&txPkt, 0, sizeof(MRBusPacket));
		txPkt.bus = mrbfsNode->bus;
		txPkt.pkt[MRBUS_PKT_SRC] = 0;  // A source of 0 will be replaced by the transmit drivers with the interface addresses
		txPkt.pkt[MRBUS_PKT_DEST] = mrbfsNode->address;
		txPkt.pkt[MRBUS_PKT_LEN] = 6;
		txPkt.pkt[MRBUS_PKT_TYPE] = 'A';
		if (nodeQueueTransmitPacket(mrbfsNode, &txPkt) < 0)
			(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Node [%s] failed to send packet", mrbfsNode->nodeName);
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
		// When the eepromNodeAddr file is read, it sends a packet out to the node to read
		// its eeprom address 0x00.
		MRBusPacket txPkt;
		// Set up the packet - initialize and fill in a few key values
		memset(&txPkt, 0, sizeof(MRBusPacket));
		txPkt.bus = mrbfsNode->bus;    // Important to set the transmit pkt's bus number so it goes to the right transmitters
		txPkt.pkt[MRBUS_PKT_SRC] = 0;  // A source of 0 will be replaced by the transmit drivers with the interface addresses
		txPkt.pkt[MRBUS_PKT_DEST] = mrbfsNode->address; // The destination is the node's current address
		txPkt.pkt[MRBUS_PKT_LEN] = 7;     // Length of 7
		txPkt.pkt[MRBUS_PKT_TYPE] = 'R';  // Packet type of EEPROM read
		txPkt.pkt[MRBUS_PKT_DATA] = 0;    // EEPROM read address 0, the node's address byte

		// Spin on requestRXFeed - we need to make sure we're the only one listening
		// This should probably be a mutex
		while(nodeLocalStorage->requestRXFeed);

		// Once nobody else is using the rx feed, grab it and initialize the queue
		nodeLocalStorage->requestRXFeed = 1;
		mrbusPacketQueueInitialize(&nodeLocalStorage->rxq);

		// Once we're listening to the bus for responses, send our query
		if (nodeQueueTransmitPacket(mrbfsNode, &txPkt) < 0)
			(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Node [%s] failed to send packet", mrbfsNode->nodeName);

		// Now, wait.  Spin in a loop with a sleep so we don't hog the processor
		// Currently this yields a 1s timeout.  Break as soon as we have our answer packet.
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
		// We're done, somebody else can have the RX feed
		nodeLocalStorage->requestRXFeed = 0;

		// If we didn't get an answer, just log a warning (MRBus is not guaranteed communications, after all)
		// A smarter node could implement retry logic
		if(!foundResponse)
		{
			(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_WARNING, "Node [%s], no response to EEPROM read request", mrbfsNode->nodeName);
			return(size = 0);
		}
		// If we're here, we have a response.  Write it to the response buffer (locally) and the end of this function will put it in the
		// actual file read buffer
		sprintf(responseBuffer, "0x%02X\n", pkt.pkt[MRBUS_PKT_DATA]);
	}
	
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
Internal Function:  nodeOptionGet()

Purpose:  
 Returns a const pointer to the string value corresponding
 to requested key value, or equal to the defaultValue
 being passed in if not found.  These come out of 
 "option" sections in the mrbfs.conf file

 If your node doesn't implement any optional parameters,
 or reads them in some other way, this can be omitted.

*******************************************************/

const char* nodeOptionGet(MRBFSBusNode* mrbfsNode, const char* nodeOptionKey, const char* defaultValue)
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

	// File "sendPing" will send a ping when written.  Reading it doesn't mean much, so we'll just make it a r/w integer
	nodeLocalStorage->file_sendPing = (*mrbfsNode->mrbfsFilesystemAddFile)("sendPing", FNODE_RW_VALUE_INT, mrbfsNode->path);
	nodeLocalStorage->file_sendPing->mrbfsFileNodeWrite = &mrbfsFileNodeWrite; // Associate this node's mrbfsFileNodeWrite for write callbacks
	nodeLocalStorage->file_sendPing->nodeLocalStorage = (void*)mrbfsNode; // Associate this node's memory with the filenode's local storage

	// File "eepromNodeAddr" demonstrates a complex readback file that must communicate across the bus to get its answer
	//  It's a readback file.
	nodeLocalStorage->file_eepromNodeAddr = (*mrbfsNode->mrbfsFilesystemAddFile)("eepromNodeAddr", FNODE_RO_VALUE_READBACK, mrbfsNode->path);
	nodeLocalStorage->file_eepromNodeAddr->mrbfsFileNodeWrite = &mrbfsFileNodeWrite; // Associate this node's mrbfsFileNodeWrite for write callbacks
	nodeLocalStorage->file_eepromNodeAddr->mrbfsFileNodeRead = &mrbfsFileNodeRead; // Associate this node's mrbfsFileNodeWrite for read callbacks
	nodeLocalStorage->file_eepromNodeAddr->nodeLocalStorage = (void*)mrbfsNode;// Associate this node's memory with the filenode's local storage

	// Return 0 to indicate success
	return (0);
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
				// FIXME: Do Stuff
				// What stuff you're doing is going to depend upon what sort of node you're
				// writing for.  However, most emit a 'S' (status) packet that will be used
				// to feed statuses reflected in various files
			}
			break;			
	}

	// If some filesystem function (usually a readback file waiting for a response) has requested we
	// give them a feed, push the current packet onto our receive queue
	if (nodeLocalStorage->requestRXFeed)
		mrbusPacketQueuePush(&nodeLocalStorage->rxq, rxPkt, 0);

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
Internal Function:  nodeQueueTransmitPacket()

Purpose: Node helper function that wraps some of the 
 transmission error checking

*******************************************************/

int nodeQueueTransmitPacket(MRBFSBusNode* mrbfsNode, MRBusPacket* txPkt)
{
	int success = 0;
	if (NULL == mrbfsNode->mrbfsNodeTxPacket)
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] can't transmit - no mrbfsNodeTxPacket function defined", mrbfsNode->nodeName);
	else if (NULL == (txPkt = calloc(1, sizeof(MRBusPacket))))
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] can't transmit - failed txPkt allocation", mrbfsNode->nodeName);
	else
	{
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] sending packet (dest=0x%02X)", mrbfsNode->nodeName, mrbfsNode->address);
		(*mrbfsNode->mrbfsNodeTxPacket)(txPkt);
		return(0);
	}
	return(-1);
}

/*******************************************************
Internal Function:  trimNewlines()

Purpose: Used to trim the logging buffer for file 'rxPackets'
 to (trimval) number of lines.

*******************************************************/

int trimNewlines(char* str, int trimval)
{
	int newlines=0;
	while(0 != *str)
	{
		if ('\n' == *str)
			newlines++;
		if (newlines >= trimval)
			*++str = 0;
		else
			++str;
	}
	return(newlines);
}


