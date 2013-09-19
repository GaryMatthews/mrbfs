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




int mrbfsInterfaceDriverVersionCheck(int ifaceVersion)
{
	if (ifaceVersion != MRBFS_INTERFACE_DRIVER_VERSION)
		return(0);
	return(1);
}

static void mrbfsCI2SerialClose(MRBFSInterfaceDriver* mrbfsInterfaceDriver, int fd)
{
	if (-1 != fd)
	{
		(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface [%s] closing port", mrbfsInterfaceDriver->interfaceName);
		close(fd);
	}
	return;
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
		return(-1); 
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
	options.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);
	options.c_oflag &= ~OPOST;
	options.c_cc[VTIME] = 0;
   options.c_cc[VMIN]   = 1;   // blocking read until 5 chars received
   
   tcflush(fd, TCIFLUSH);
	tcsetattr(fd, TCSAFLUSH, &options);

	nbytes = write(fd, "\x0A\x0D", strlen("\x0A\x0D"));
	nbytes = write(fd, "\x0A\x0D", strlen("\x0A\x0D"));

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

const char* mrbfsInterfaceOptionGet(MRBFSInterfaceDriver* mrbfsInterfaceDriver, const char* interfaceOptionKey, const char* defaultValue)
{
	int i;
	(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] - [%d] node options, looking for [%s]", mrbfsInterfaceDriver->interfaceName, mrbfsInterfaceDriver->interfaceOptions, interfaceOptionKey);

	for(i=0; i<mrbfsInterfaceDriver->interfaceOptions; i++)
	{
		(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Node [%s] - node option [%d], comparing key [%s] to [%s]", mrbfsInterfaceDriver->interfaceName, i, interfaceOptionKey, mrbfsInterfaceDriver->interfaceOptionList[i].key);
		if (0 == strcmp(interfaceOptionKey, mrbfsInterfaceDriver->interfaceOptionList[i].key))
			return(mrbfsInterfaceDriver->interfaceOptionList[i].value);
	}
	return(defaultValue);
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
	time_t processingPacket=0;
	uint8_t resetSerial = 0;
	uint32_t timeoutSeconds = 5;
	
	int fd = -1, nbytes=0, i=0;	

	(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface [%s] confirms startup", mrbfsInterfaceDriver->interfaceName);

   memset(buffer, 0, sizeof(buffer));
   bufptr = buffer;

	timeoutSeconds = atoi(mrbfsInterfaceOptionGet(mrbfsInterfaceDriver, "timeout", "2"));

	if (timeoutSeconds < 2 || timeoutSeconds >= 120)
	{
		(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_WARNING, "Interface [%s] - Timeout of %d not valid (range 2-120), setting to minimum of 2 seconds", mrbfsInterfaceDriver->interfaceName, timeoutSeconds);
		timeoutSeconds = 2;
	}
	else
	{
		(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_WARNING, "Interface [%s] - Setting timeout to [%d] seconds", mrbfsInterfaceDriver->interfaceName, timeoutSeconds);	
	}

	fd = mrbfsCI2SerialOpen(mrbfsInterfaceDriver);

   while(!mrbfsInterfaceDriver->terminate)
   {
      usleep(1000);
      
      if(-1 == fd || ((nbytes = write(fd, "  ", 0)) < 0))
      {
      	// Signals don't seem to get generated with USB device removal
      	// Maybe I'm doing it wrong
      	// That said, writing 0 bytes to a closed terminal gets us an error
			resetSerial = 1;
		}
      
      if (resetSerial)
      {
			mrbfsCI2SerialClose(mrbfsInterfaceDriver, fd);
	      
	      // Nothing we can do until we get our port back
			do
			{
				usleep(100000);
				fd = mrbfsCI2SerialOpen(mrbfsInterfaceDriver);
			} while (0 == fd);

			memset(buffer, 0, sizeof(buffer));
			bufptr = buffer;
			processingPacket = 0;
			resetSerial = 0;
      }      
      
      while ((nbytes = read(fd, incomingByte, 1)) > 0)
      {
        (*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_ANNOYING, "Interface [%s] got byte [0x%02X]", mrbfsInterfaceDriver->interfaceName, incomingByte[0]);

         switch(incomingByte[0])
         {
            case 0x00:
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
							
/*
                                                        //  MDP: Code to enable a very crude running packet log.
                                                        //  It worked in a pinch - didn't promise it was any good.
							FILE *fptr;
							fptr = fopen("/home/house/mrbfs.pktlog", "a");
							fputs(newPacket, fptr);
							fclose(fptr);
*/

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
					processingPacket = time(NULL);
               *bufptr++ = incomingByte[0];
               if (bufptr >= buffer + sizeof(buffer))
               {
						processingPacket = 0;
						bufptr = buffer;
               }
               break;
         }
      }

		if (processingPacket && ((time(NULL) - processingPacket) > timeoutSeconds) )
		{
			// Timeout on read, do something
			resetSerial = 1;
			(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_WARNING, "Interface [%s] timed out receiving packet, resetting", mrbfsInterfaceDriver->interfaceName);
			continue;
		}


		if ((0 == processingPacket) && mrbusPacketQueueDepth(&nodeLocalStorage->txq) )
		{
			MRBusPacket txPkt;
			uint8_t txPktBuffer[256];
			uint32_t txPktBufferLen=0;
			uint32_t i;
			uint32_t bytesWritten = 0;

			mrbusPacketQueuePop(&nodeLocalStorage->txq, &txPkt);
			sprintf(txPktBuffer, ":%02X->%02X %02X", txPkt.pkt[MRBUS_PKT_SRC], txPkt.pkt[MRBUS_PKT_DEST], txPkt.pkt[MRBUS_PKT_TYPE]);
			for (i=MRBUS_PKT_DATA; i<txPkt.pkt[MRBUS_PKT_LEN]; i++)
				sprintf(txPktBuffer + strlen(txPktBuffer), " %02X", txPkt.pkt[i]);
			sprintf(txPktBuffer + strlen(txPktBuffer), ";\x0D");
			(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface driver [%s] transmitting %d bytes", mrbfsInterfaceDriver->interfaceName, strlen(txPktBuffer)); 
	
			txPktBufferLen = strlen(txPktBuffer);
			bytesWritten = 0;

			processingPacket = time(NULL);

			do
			{
				nbytes = write(fd, txPktBuffer + bytesWritten, txPktBufferLen - bytesWritten);
				if (nbytes >= 0)
				{
					(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_ANNOYING, "Interface driver [%s] transmitting %d bytes of %d byte packet", mrbfsInterfaceDriver->interfaceName, bytesWritten, txPktBufferLen); 
					bytesWritten += nbytes;
				}
				else if (-1 == nbytes && !(EAGAIN == errno || EWOULDBLOCK == errno))
				{
					(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Interface driver [%s] got errno=%d on write of %d bytes", mrbfsInterfaceDriver->interfaceName, errno, txPktBufferLen); 
					resetSerial = 1;
				}
				else if (-1 == nbytes && (EAGAIN == errno || EWOULDBLOCK == errno))
				{
					(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Interface driver [%s] delaying transmission due blocking", mrbfsInterfaceDriver->interfaceName, errno, txPktBufferLen); 
			      usleep(50);
				}
				else
				{
					// Do nothing, really, it's 
					(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Interface driver [%s] got retval=%d and errno=%d - no comprende!", mrbfsInterfaceDriver->interfaceName, nbytes, errno);
				}
			
			} while (!resetSerial 
				&& (bytesWritten < txPktBufferLen) 
				&& ((time(NULL) - processingPacket) > timeoutSeconds));
	
			processingPacket = 0;
	
			if (bytesWritten < txPktBufferLen)
			{
				resetSerial = 1;
				(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_ERROR, "Interface driver [%s] didn't transmit enough bytes (%d != %d), resetting serial", mrbfsInterfaceDriver->interfaceName, bytesWritten, txPktBufferLen);
			}
			else
				(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_ANNOYING, "Interface driver [%s] actually transmitted %d bytes", mrbfsInterfaceDriver->interfaceName, bytesWritten);          
		}

   }
   
	(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface driver [%s] terminating", mrbfsInterfaceDriver->interfaceName);   
	mrbfsCI2SerialClose(mrbfsInterfaceDriver, fd);  
	phtread_exit(NULL);
}


