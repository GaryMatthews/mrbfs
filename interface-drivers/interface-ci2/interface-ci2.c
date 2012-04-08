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

// ~85 bytes per packet, and hold 512
#define RX_PKT_BUFFER_SZ  (83 * 512)  

typedef struct
{
	UINT32 pktsReceived;
	MRBFSFileNode* file_pktCounter;
	MRBFSFileNode* file_pktLog;
	char pktLogStr[RX_PKT_BUFFER_SZ];
	MRBusPacketQueue txq;
} NodeLocalStorage;


void mrbusPacketQueueInitialize(MRBusPacketQueue* q)
{
	pthread_mutexattr_t lockAttr;
	memset(q, 0, sizeof(MRBusPacketQueue));
	// Initialize the queue lock
	pthread_mutexattr_init(&lockAttr);
	pthread_mutexattr_settype(&lockAttr, PTHREAD_MUTEX_ADAPTIVE_NP);
	pthread_mutex_init(&q->queueLock, &lockAttr);
	pthread_mutexattr_destroy(&lockAttr);		

}


int mrbusPacketQueueDepth(MRBusPacketQueue* q)
{
	int depth = 0;
	
	pthread_mutex_lock(&q->queueLock);
	depth = (q->headIdx - q->tailIdx) % MRBUS_PACKET_QUEUE_SIZE; 
	pthread_mutex_unlock(&q->queueLock);

	return(depth);
}

void mrbusPacketQueuePush(MRBusPacketQueue* q, MRBusPacket* txPkt, UINT8 srcAddress)
{
	pthread_mutex_lock(&q->queueLock);

	memcpy(&q->pkts[q->headIdx], txPkt, sizeof(MRBusPacket));
	if (0 != srcAddress && 0 == txPkt->pkt[MRBUS_PKT_SRC])
		q->pkts[q->headIdx].pkt[MRBUS_PKT_SRC] = srcAddress;

	if( ++q->headIdx >= MRBUS_PACKET_QUEUE_SIZE )
		q->headIdx = 0;

	pthread_mutex_unlock(&q->queueLock);
}

MRBusPacket* mrbusPacketQueuePop(MRBusPacketQueue* q, MRBusPacket* pkt)
{
	memcpy(pkt, &q->pkts[q->tailIdx], sizeof(MRBusPacket));
	if( ++q->tailIdx >= MRBUS_PACKET_QUEUE_SIZE )
		q->tailIdx = 0;

	return(pkt);
}


int mrbfsInterfaceDriverVersionCheck(int ifaceVersion)
{
	if (ifaceVersion != MRBFS_INTERFACE_DRIVER_VERSION)
		return(0);
	return(1);
}

static void mrbfsCI2SerialClose(int fd)
{
}

static int mrbfsCI2SerialOpen(MRBFSInterfaceDriver* mrbfsInterfaceDriver)
{
	int fd, n;
	struct termios options;
	int  nbytes;       // Number of bytes read 
	struct timeval timeout;
	char* device = mrbfsInterfaceDriver->port;

	(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface [%s] - Starting serial port setup on [%s]", mrbfsInterfaceDriver->interfaceName, device);

	fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY); 
   if (fd < 0)
	{
		(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Ser, %d", fd);
		perror(device); 
		return(0); 
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	tcgetattr(fd,&options); // save current serial port settings 

	(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Interface [%s] - Serial port [%s] opened, not yet configured", mrbfsInterfaceDriver->interfaceName, device);

	cfsetispeed(&options, B115200);
	cfsetospeed(&options, B115200);

   options.c_cflag &= ~CSIZE; // Mask the character size bits 
   options.c_cflag |= CS8;    // Select 8 data bits 

	options.c_cflag &= ~CRTSCTS;
	options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
	options.c_iflag &= ~(IXON | IXOFF | IXANY);
	options.c_oflag &= ~OPOST;
	options.c_cc[VTIME] = 0;
   options.c_cc[VMIN]   = 1;   // blocking read until 5 chars received
   
   tcflush(fd, TCIFLUSH);
	tcsetattr(fd, TCSAFLUSH, &options);

	(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface [%s] - Serial startup complete", mrbfsInterfaceDriver->interfaceName);

	return(fd);
}

void mrbfsInterfaceDriverInit(MRBFSInterfaceDriver* mrbfsInterfaceDriver)
{
	NodeLocalStorage* nodeLocalStorage = calloc(1, sizeof(NodeLocalStorage));
	mrbfsInterfaceDriver->nodeLocalStorage = (void*)nodeLocalStorage;

	nodeLocalStorage->file_pktCounter = (*mrbfsInterfaceDriver->mrbfsFilesystemAddFile)("pktCounter", FNODE_RO_VALUE_INT, mrbfsInterfaceDriver->path);
	nodeLocalStorage->file_pktLog = (*mrbfsInterfaceDriver->mrbfsFilesystemAddFile)("pktLog", FNODE_RO_VALUE_STR, mrbfsInterfaceDriver->path);

	nodeLocalStorage->file_pktLog->value.valueStr = nodeLocalStorage->pktLogStr;
	mrbusPacketQueueInitialize(&nodeLocalStorage->txq);
}

void mrbfsInterfacePacketTransmit(MRBFSInterfaceDriver* mrbfsInterfaceDriver, MRBusPacket* txPkt)
{
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)mrbfsInterfaceDriver->nodeLocalStorage;
	// This will be called from the main process, not the interface thread
	// This thing probably should just enqueue the packet and let the main loop take care of it.
	(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Interface [%s] enqueuing pkt for transmit (src=%02X)", mrbfsInterfaceDriver->interfaceName, mrbfsInterfaceDriver->addr);
	mrbusPacketQueuePush(&nodeLocalStorage->txq, txPkt, mrbfsInterfaceDriver->addr);
}

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

void mrbfsInterfaceDriverRun(MRBFSInterfaceDriver* mrbfsInterfaceDriver)
{
	UINT8 buffer[256];
	UINT8 *bufptr;      // Current char in buffer 
   UINT8 pktBuf[256];
   UINT8 incomingByte[2];
	const char* device="/dev/ttyUSB0";
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)mrbfsInterfaceDriver->nodeLocalStorage;
	struct termios options;
	struct timeval timeout;
	int processingPacket=0;
	int fd = -1, nbytes=0, i=0;	

	(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface [%s] confirms startup", mrbfsInterfaceDriver->interfaceName);

   memset(buffer, 0, sizeof(buffer));
   bufptr = buffer;

	fd = mrbfsCI2SerialOpen(mrbfsInterfaceDriver);

   while(!mrbfsInterfaceDriver->terminate)
   {
      usleep(1000);
      while ((nbytes = read(fd, incomingByte, 1)) > 0)
      {
         switch(incomingByte[0])
         {
            case ' ': 
            case 0x0A:
               break;

            case 0x0D:
               // Try to parse whatever's in there
               if ('P' == buffer[0])
               {
						time_t currentTime = time(NULL);
                  // It's a packet
						// Give it back to the control thread
						MRBusPacket rxPkt;
						UINT8* ptr = buffer+2;
						memset(&rxPkt, 0, sizeof(MRBusPacket));
						rxPkt.bus = mrbfsInterfaceDriver->bus;
						rxPkt.len = (bufptr - ptr)/2;
						for(i=0; i<rxPkt.len; i++, ptr+=2)
						{
							char hexByte[3];
							hexByte[2] = 0;
							memcpy(hexByte, ptr, 2);
							rxPkt.pkt[i] = strtol(hexByte, NULL, 16);
						}
						(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Interface [%s] got packet [%s], txq depth=[%d]", mrbfsInterfaceDriver->interfaceName, buffer+2, mrbusPacketQueueDepth(&nodeLocalStorage->txq));

						// Store the packet in the receive queue
						{
							char *newStart = nodeLocalStorage->pktLogStr;
							size_t rxPacketLen = strlen(nodeLocalStorage->pktLogStr), newLen=rxPacketLen, newRemaining=RX_PKT_BUFFER_SZ-rxPacketLen;
							char timeString[64];
							char newPacket[100];
							int b;
							size_t timeSize=0;
							struct tm pktTimeTM;

							localtime_r(&currentTime, &pktTimeTM);
							memset(newPacket, 0, sizeof(newPacket));
							strftime(newPacket, sizeof(newPacket), "[%Y%m%d %H%M%S] R ", &pktTimeTM);
	
							for(b=0; b<rxPkt.len; b++)
								sprintf(newPacket + 20 + b*3, "%02X ", rxPkt.pkt[b]);
							*(newPacket + 20 + b*3-1) = '\n';
							*(newPacket + 20 + b*3) = 0;
							newLen = 20 + b*3;

							// Trim rear of existing string
							trimNewlines(nodeLocalStorage->pktLogStr, 511);

							memmove(nodeLocalStorage->pktLogStr + newLen, nodeLocalStorage->pktLogStr, strlen(nodeLocalStorage->pktLogStr));
							memcpy(nodeLocalStorage->pktLogStr, newPacket, newLen);
							nodeLocalStorage->file_pktLog->updateTime = currentTime;

							nodeLocalStorage->file_pktCounter->updateTime = currentTime;
							nodeLocalStorage->file_pktCounter->value.valueInt = ++nodeLocalStorage->pktsReceived;
						}
						(*mrbfsInterfaceDriver->mrbfsPacketReceive)(&rxPkt);
               }
					else
					{
						(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Interface [%s] got non-packet response [%s]", mrbfsInterfaceDriver->interfaceName, buffer);
					}


               memset(buffer, 0, sizeof(buffer));
               bufptr = buffer;
					processingPacket = 0;

               break;

            default:
					processingPacket = 1;
               *bufptr++ = incomingByte[0];
               break;
         }
      }
		if (!processingPacket && mrbusPacketQueueDepth(&nodeLocalStorage->txq) )
		{
			MRBusPacket txPkt;
			UINT8 txPktBuffer[32];
			UINT8 txPktBufferLen=0;
			mrbusPacketQueuePop(&nodeLocalStorage->txq, &txPkt);
			sprintf(txPktBuffer, ":%02X->%02X %02X", txPkt.pkt[MRBUS_PKT_SRC], txPkt.pkt[MRBUS_PKT_DEST], txPkt.pkt[MRBUS_PKT_TYPE]);
			for (i=MRBUS_PKT_DATA; i<txPkt.pkt[MRBUS_PKT_LEN]; i++)
				sprintf(txPktBuffer + strlen(txPktBuffer), " %02X", txPkt.pkt[i]);
			sprintf(txPktBuffer + strlen(txPktBuffer), ";\x0D");
			(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface driver [%s] transmitting %d bytes", mrbfsInterfaceDriver->interfaceName, strlen(txPktBuffer)); 
			write(fd, txPktBuffer, strlen(txPktBuffer));
		}

   }
   
	(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface driver [%s] terminating", mrbfsInterfaceDriver->interfaceName);   
	phtread_exit(NULL);
}


