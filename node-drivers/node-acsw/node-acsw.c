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
#include "mrbfs-module.h"
#include "mrbfs-pktqueue.h"
#include "node-helpers.h"

#define MRBFS_NODE_DRIVER_NAME   "node-acsw"
#define MRB_ACSW_MAX_INPUTS    4
#define MRB_ACSW_MAX_OUTPUTS   4
/*******************************************************
 Internal Helper Function Headers - may or may not be helpful to your module
*******************************************************/

int trimNewlines(char* str, int trimval);
int nodeQueueTransmitPacket(MRBFSBusNode* mrbfsNode, MRBusPacket* txPkt);
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

typedef struct
{
	MRBFSFileNode* file_rxCounter;
	MRBFSFileNode* file_rxPackets;
	MRBFSFileNode* file_eepromNodeAddr;
	MRBFSFileNode* file_sendPing;

	MRBFSFileNode* file_output[MRB_ACSW_MAX_OUTPUTS];
	char* outputValueStr[MRB_ACSW_MAX_OUTPUTS];
	MRBFSFileNode* file_input[MRB_ACSW_MAX_INPUTS];
	char* inputValueStr[MRB_ACSW_MAX_INPUTS];	
	MRBFSFileNode* file_busVoltage;
	char* busVoltageValue;

	MRBFSFileNode* file_counterA;
	MRBFSFileNode* file_counterB;
	char* counterAValueStr;
	char* counterBValueStr;

	
	char rxPacketStr[RX_PKT_BUFFER_SZ];
	UINT8 requestRXFeed;
	MRBusPacketQueue rxq;
	int timeout;
	time_t lastUpdated;	
	
	UINT8 suppressUnits;
	UINT8 decimalPositions;
	UINT8 outputsConnected;
	UINT8 inputsConnected;
	UINT8 inputsInverted;

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
	int i,j;
	char commandStr[17];
	MRBusPacket txPkt;
	UINT8 xmit=0;
	// Set up the packet - initialize and fill in a few key values
	memset(&txPkt, 0, sizeof(MRBusPacket));
	txPkt.bus = mrbfsNode->bus;
	txPkt.pkt[MRBUS_PKT_SRC] = 0;  // A source of 0 will be replaced by the transmit drivers with the interface addresses
	txPkt.pkt[MRBUS_PKT_DEST] = mrbfsNode->address;

	memset(commandStr, 0, sizeof(commandStr));
	
	for(i=0, j=0; i<sizeof(commandStr)-1; i++)
	{
		if (j >= dataSz)
			break;

		if (data[j] == 0x0A || data[j] == 0x0C || data[j] == 0)
			break;

		if (isalnum(data[j]))
			commandStr[i] = toupper(data[j]);

		j++;
	}



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
	else if (mrbfsFileNode == nodeLocalStorage->file_counterA || mrbfsFileNode == nodeLocalStorage->file_counterB)
	{
		txPkt.pkt[MRBUS_PKT_TYPE] = 'C';
		txPkt.pkt[MRBUS_PKT_LEN] = 8;

		if (mrbfsFileNode == nodeLocalStorage->file_counterA)
			txPkt.pkt[6] = 0x10;
		else if (mrbfsFileNode == nodeLocalStorage->file_counterB)
			txPkt.pkt[6] = 0x11;

		if (0 == strcmp(commandStr, "ON") || 0 == strcmp(commandStr, "1") || 0 == strcmp(commandStr, "ENABLE") || 0 == strcmp(commandStr, "START"))
		{
			txPkt.pkt[7] = '1';
			xmit = 1;
		}
		else if (0 == strcmp(commandStr, "OFF") || 0 == strcmp(commandStr, "0") || 0 == strcmp(commandStr, "DISABLE") || 0 == strcmp(commandStr, "PAUSE"))
		{
			txPkt.pkt[7] = '0';
			xmit = 1;
		}	
		else if (0 == strcmp(commandStr, "SAVE"))
		{
			txPkt.pkt[7] = 'S';
			xmit = 1;
		}	
		else if (0 == strcmp(commandStr, "LOAD"))
		{
			txPkt.pkt[7] = 'L';
			xmit = 1;
		}	
		else if (0 == strcmp(commandStr, "RESET"))
		{
			txPkt.pkt[7] = 'R';
			xmit = 1;
		}	
		else if (0 == strcmp(commandStr, "CLEAR"))
		{
			txPkt.pkt[7] = 'F';
			xmit = 1;
		}	

		if(xmit)
		{
			if (nodeQueueTransmitPacket(mrbfsNode, &txPkt) < 0)
				(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Node [%s] failed to send packet", mrbfsNode->nodeName);
		}	

	}
	else
	{
		for (i=0; i<nodeLocalStorage->outputsConnected; i++)
		{
			if (mrbfsFileNode == nodeLocalStorage->file_output[i])
			{
				txPkt.pkt[MRBUS_PKT_TYPE] = 'C';
				txPkt.pkt[MRBUS_PKT_LEN] = 8;
				txPkt.pkt[6] = i;

				if (0 == strcmp(commandStr, "ON") || 0 == strcmp(commandStr, "1") || 0 == strcmp(commandStr, "FORCEDON"))
				{
					txPkt.pkt[7] = '1';
					xmit=1;			
				}
				else if (0 == strcmp(commandStr, "OFF") || 0 == strcmp(commandStr, "0") || 0 == strcmp(commandStr, "FORCEDOFF"))
				{
					txPkt.pkt[7] = '0';
					xmit=1;			
				}
				else if (0 == strcmp(commandStr, "LOCAL") || 0 == strcmp(commandStr, "X") || 0 == strcmp(commandStr, "RESET"))
				{
					txPkt.pkt[7] = 'X';
					xmit=1;			
				}
				else
				{
					(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] didn't understand command [%s]", mrbfsNode->nodeName, commandStr);
				}

				if(xmit)
				{
					if (nodeQueueTransmitPacket(mrbfsNode, &txPkt) < 0)
						(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Node [%s] failed to send packet", mrbfsNode->nodeName);
				}	
						
				break;
			}		
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

	// Initialize pieces of the local storage and create the files our node will use to communicate with the user
	nodeLocalStorage->timeout = atoi(mrbfsNodeOptionGet(mrbfsNode, "timeout", "none"));
	nodeLocalStorage->lastUpdated = 0;

	nodeLocalStorage->suppressUnits = 0;
	if (0 == strcmp(mrbfsNodeOptionGet(mrbfsNode, "suppress_units", "no"), "yes"))
		nodeLocalStorage->suppressUnits = 1;

	nodeLocalStorage->decimalPositions = atoi(mrbfsNodeOptionGet(mrbfsNode, "decimal_positions", "2"));


	nodeLocalStorage->outputsConnected = atoi(mrbfsNodeOptionGet(mrbfsNode, "outputs_connected", "4"));
	nodeLocalStorage->inputsConnected = atoi(mrbfsNodeOptionGet(mrbfsNode, "inputs_connected", "4"));
	nodeLocalStorage->inputsInverted = atoi(mrbfsNodeOptionGet(mrbfsNode, "inputs_inverted", "1"));
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

	// Initialize the input files
	if (nodeLocalStorage->inputsConnected < 0 || nodeLocalStorage->inputsConnected > MRB_ACSW_MAX_INPUTS)
	{
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Node [%s] - inputs_connected must be between 0-%d, not [%d] - defaulting to %d", 
			mrbfsNode->nodeName, MRB_ACSW_MAX_OUTPUTS, nodeLocalStorage->inputsConnected, MRB_ACSW_MAX_INPUTS);
		nodeLocalStorage->inputsConnected = MRB_ACSW_MAX_INPUTS;
	}

	for(i=0; i<nodeLocalStorage->inputsConnected; i++)
	{
		char inputDefaultFilename[32];
		char inputKeyname[32];
		const char* inputFilename = inputDefaultFilename;

		sprintf(inputDefaultFilename, "in%d", i+1);
		sprintf(inputKeyname, "in%d_name", i+1);

		inputFilename = mrbfsNodeOptionGet(mrbfsNode, inputKeyname, inputDefaultFilename);
		nodeLocalStorage->file_input[i] = (*mrbfsNode->mrbfsFilesystemAddFile)(inputFilename, FNODE_RO_VALUE_STR, mrbfsNode->path);
		nodeLocalStorage->file_input[i]->mrbfsFileNodeWrite = &mrbfsFileNodeWrite;
		nodeLocalStorage->file_input[i]->nodeLocalStorage = (void*)mrbfsNode;
		nodeLocalStorage->inputValueStr[i] = calloc(1, OUTPUT_VALUE_BUFFER_SZ);
		nodeLocalStorage->file_input[i]->value.valueStr = nodeLocalStorage->inputValueStr[i];		
	}

	// Initialize the output files
	if (nodeLocalStorage->outputsConnected < 0 || nodeLocalStorage->outputsConnected > MRB_ACSW_MAX_INPUTS)
	{
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Node [%s] - outputs_connected must be between 0-%d, not [%d] - defaulting to %d", 
			mrbfsNode->nodeName, MRB_ACSW_MAX_OUTPUTS, nodeLocalStorage->outputsConnected, MRB_ACSW_MAX_OUTPUTS);
		nodeLocalStorage->outputsConnected = MRB_ACSW_MAX_INPUTS;
	}


	for(i=0; i<nodeLocalStorage->outputsConnected; i++)
	{
		char outputDefaultFilename[32];
		char outputKeyname[32];
		const char* outputFilename = outputDefaultFilename;
		switch(i)
		{
			case 0:
			case 1:
				sprintf(outputDefaultFilename, "ac%d", i+1);
				sprintf(outputKeyname, "ac%d_name", i+1);
				break;	
			
			case 2:
			case 3:
			default:
				sprintf(outputDefaultFilename, "out%d", i-1);
				sprintf(outputKeyname, "out%d_name", i-1);
				break;	
		}

		outputFilename = mrbfsNodeOptionGet(mrbfsNode, outputKeyname, outputDefaultFilename);
		nodeLocalStorage->file_output[i] = (*mrbfsNode->mrbfsFilesystemAddFile)(outputFilename, FNODE_RW_VALUE_STR, mrbfsNode->path);
		nodeLocalStorage->file_output[i]->mrbfsFileNodeWrite = &mrbfsFileNodeWrite;
		nodeLocalStorage->file_output[i]->nodeLocalStorage = (void*)mrbfsNode;
		nodeLocalStorage->outputValueStr[i] = calloc(1, OUTPUT_VALUE_BUFFER_SZ);
		nodeLocalStorage->file_output[i]->value.valueStr = nodeLocalStorage->outputValueStr[i];
	}


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


	nodeLocalStorage->busVoltageValue = calloc(1, OUTPUT_VALUE_BUFFER_SZ);	
	nodeLocalStorage->file_busVoltage = (*mrbfsNode->mrbfsFilesystemAddFile)("mrbus_voltage", FNODE_RO_VALUE_STR, mrbfsNode->path);
	nodeLocalStorage->file_busVoltage->value.valueStr = nodeLocalStorage->busVoltageValue;

	nodeLocalStorage->counterAValueStr = calloc(1, OUTPUT_VALUE_BUFFER_SZ);	
	nodeLocalStorage->file_counterA = (*mrbfsNode->mrbfsFilesystemAddFile)("counterA", FNODE_RW_VALUE_STR, mrbfsNode->path);
	nodeLocalStorage->file_counterA->value.valueStr = nodeLocalStorage->counterAValueStr;
	nodeLocalStorage->file_counterA->nodeLocalStorage = (void*)mrbfsNode;
	nodeLocalStorage->file_counterA->mrbfsFileNodeWrite = &mrbfsFileNodeWrite; // Associate this node's mrbfsFileNodeWrite for write callbacks
	
	nodeLocalStorage->counterBValueStr = calloc(1, OUTPUT_VALUE_BUFFER_SZ);	
	nodeLocalStorage->file_counterB = (*mrbfsNode->mrbfsFilesystemAddFile)("counterB", FNODE_RW_VALUE_STR, mrbfsNode->path);
	nodeLocalStorage->file_counterB->value.valueStr = nodeLocalStorage->counterBValueStr;
	nodeLocalStorage->file_counterB->nodeLocalStorage = (void*)mrbfsNode;	
	nodeLocalStorage->file_counterB->mrbfsFileNodeWrite = &mrbfsFileNodeWrite; // Associate this node's mrbfsFileNodeWrite for write callbacks
	
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
				int i;
				UINT8 inputs = 0;
				for(i=0; i<nodeLocalStorage->outputsConnected; i++)
				{
					nodeLocalStorage->file_output[i]->updateTime = currentTime;
					if (rxPkt->pkt[7] & (1<<i))
					{
						if (rxPkt->pkt[7] & (1<<(i+4)))
							strcpy(nodeLocalStorage->outputValueStr[i], "ForcedOn\n");
						else
							strcpy(nodeLocalStorage->outputValueStr[i], "On\n");
					} else {
						if (rxPkt->pkt[7] & (1<<(i+4)))
							strcpy(nodeLocalStorage->outputValueStr[i], "ForcedOff\n");
						else
							strcpy(nodeLocalStorage->outputValueStr[i], "Off\n");
					}											
				}

				if (nodeLocalStorage->inputsInverted)
					inputs = rxPkt->pkt[6] ^ 0x0F;
				else
					inputs = rxPkt->pkt[6];

				for(i=0; i<nodeLocalStorage->inputsConnected; i++)
				{
					nodeLocalStorage->file_input[i]->updateTime = currentTime;
					strcpy(nodeLocalStorage->inputValueStr[i], (inputs & (1<<i))?"On\n":"Off\n");
				}

				{
					unsigned int counterAValue=(((unsigned int)rxPkt->pkt[10])<<24) + (((unsigned int)rxPkt->pkt[11])<<16) + (((unsigned int)rxPkt->pkt[12])<<8) + (((unsigned int)rxPkt->pkt[13]));
					unsigned int counterBValue=(((unsigned int)rxPkt->pkt[14])<<24) + (((unsigned int)rxPkt->pkt[15])<<16) + (((unsigned int)rxPkt->pkt[16])<<8) + (((unsigned int)rxPkt->pkt[17]));
					nodeLocalStorage->file_counterA->updateTime = nodeLocalStorage->file_counterB->updateTime = currentTime;
					if (rxPkt->pkt[6] & 0x20)
						sprintf(nodeLocalStorage->counterAValueStr, "%d OVERFLOW\n", counterAValue);
					else
						sprintf(nodeLocalStorage->counterAValueStr, "%d\n", counterAValue);

					if (rxPkt->pkt[6] & 0x80)
						sprintf(nodeLocalStorage->counterBValueStr, "%d OVERFLOW\n", counterBValue);
					else
						sprintf(nodeLocalStorage->counterBValueStr, "%d\n", counterBValue);
				}

				nodeLocalStorage->lastUpdated = currentTime;			
				snprintf(nodeLocalStorage->busVoltageValue, OUTPUT_VALUE_BUFFER_SZ-1, "%.*f%s", nodeLocalStorage->decimalPositions, ((double)rxPkt->pkt[9])/10.0, nodeLocalStorage->suppressUnits?"":" V\n" );
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
	int i;
	
	for(i=0; i<nodeLocalStorage->inputsConnected; i++)
		strcpy(nodeLocalStorage->inputValueStr[i], "No Data\n");

	for(i=0; i<nodeLocalStorage->outputsConnected; i++)
		strcpy(nodeLocalStorage->outputValueStr[i], "No Data\n");

	strcpy(nodeLocalStorage->busVoltageValue, "No Data\n");

	strcpy(nodeLocalStorage->counterAValueStr, "No Data\n");	
	strcpy(nodeLocalStorage->counterBValueStr, "No Data\n");
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
	else
	{
		(*mrbfsNode->mrbfsLogMessage)(MRBFS_LOG_INFO, "Node [%s] sending packet (dest=0x%02X)", mrbfsNode->nodeName, mrbfsNode->address);
		(*mrbfsNode->mrbfsNodeTxPacket)(txPkt);
		return(0);
	}
	return(-1);
}



