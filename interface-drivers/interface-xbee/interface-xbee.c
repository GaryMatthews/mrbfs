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
	int dBm;
	time_t lastUpdate;
} NodeRxRSSI;

typedef struct
{
	UINT32 pktsReceived;
	MRBFSFileNode* file_pktCounter;
	MRBFSFileNode* file_pktLog;
	MRBFSFileNode* file_nodeRSSI;
	char pktLogStr[RX_PKT_BUFFER_SZ];
	MRBusPacketQueue txq;
	NodeRxRSSI rssi[256];
	char* nodeRSSIStr;
} NodeLocalStorage;

const uint8_t MRBus_CRC16_HighTable[16] =
{
	0x00, 0xA0, 0xE0, 0x40, 0x60, 0xC0, 0x80, 0x20,
	0xC0, 0x60, 0x20, 0x80, 0xA0, 0x00, 0x40, 0xE0
};
const uint8_t MRBus_CRC16_LowTable[16] =
{
	0x00, 0x01, 0x03, 0x02, 0x07, 0x06, 0x04, 0x05,
	0x0E, 0x0F, 0x0D, 0x0C, 0x09, 0x08, 0x0A, 0x0B
};

uint16_t mrbusCRC16Update(uint16_t crc, uint8_t a)
{
	uint8_t t;
	uint8_t i = 0;

	uint8_t W;
	uint8_t crc16_high = (crc >> 8) & 0xFF;
	uint8_t crc16_low = crc & 0xFF;

	while (i < 2)
	{
		if (i)
		{
			W = ((crc16_high << 4) & 0xF0) | ((crc16_high >> 4) & 0x0F);
			W = W ^ a;
			W = W & 0x0F;
			t = W;
		}
		else
		{
			W = crc16_high;
			W = W ^ a;
			W = W & 0xF0;
			t = W;
			t = ((t << 4) & 0xF0) | ((t >> 4) & 0x0F);
		}

		crc16_high = crc16_high << 4; 
		crc16_high |= (crc16_low >> 4);
		crc16_low = crc16_low << 4;

		crc16_high = crc16_high ^ MRBus_CRC16_HighTable[t];
		crc16_low = crc16_low ^ MRBus_CRC16_LowTable[t];

		i++;
	}

	return ( ((crc16_high << 8) & 0xFF00) + crc16_low );
}


int mrbfsInterfaceDriverVersionCheck(int ifaceVersion)
{
	if (ifaceVersion != MRBFS_INTERFACE_DRIVER_VERSION)
		return(0);
	return(1);
}

static void mrbfsCI2SerialClose(int fd)
{
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
	options.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);
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

	nodeLocalStorage->file_nodeRSSI = (*mrbfsInterfaceDriver->mrbfsFilesystemAddFile)("rssi", FNODE_RO_VALUE_STR, mrbfsInterfaceDriver->path);
	nodeLocalStorage->file_nodeRSSI->value.valueStr = nodeLocalStorage->nodeRSSIStr;

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
	uint8_t buffer[256];
	uint8_t *bufptr;      // Current char in buffer 
   uint8_t pktBuf[256];
   uint8_t incomingByte[2];
	const char* device="/dev/ttyUSB0";
	NodeLocalStorage* nodeLocalStorage = (NodeLocalStorage*)mrbfsInterfaceDriver->nodeLocalStorage;
	struct termios options;
	struct timeval timeout;
	int processingPacket=0;
	int fd = -1, nbytes=0, i=0;	
	uint32_t expectedPktLen = 0;
	uint8_t escapeNextByte = 0;
	

	(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface [%s] confirms startup", mrbfsInterfaceDriver->interfaceName);

   memset(buffer, 0, sizeof(buffer));
   bufptr = buffer;

	fd = mrbfsCI2SerialOpen(mrbfsInterfaceDriver);

   while(!mrbfsInterfaceDriver->terminate)
   {
      usleep(1000);
      while ((nbytes = read(fd, incomingByte, 1)) > 0)
      {
			(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface [%s] got byte [0x%02X]", mrbfsInterfaceDriver->interfaceName, incomingByte[0]);

         switch(incomingByte[0])
         {
            case 0x7E:
					// Start of API frame
               memset(buffer, 0, sizeof(buffer));
               bufptr = buffer;
               *bufptr++ = incomingByte[0];               
					processingPacket = 1;
					expectedPktLen = 0;
					escapeNextByte = 0;
					(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface [%s] starting packet parse on 0x7E", mrbfsInterfaceDriver->interfaceName);
               break;
               
				case 0x7D:
					// Escape character
					(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface [%s] setting escape", mrbfsInterfaceDriver->interfaceName);
					escapeNextByte = 1;
					break;
					
            default:
					if (escapeNextByte)
						incomingByte[0] ^= 0x20;
					escapeNextByte = 0;

					processingPacket = 1;
               *bufptr++ = incomingByte[0];
               if (bufptr >= buffer + sizeof(buffer))
               {
						processingPacket = 0;
						memset(buffer, 0, sizeof(buffer));	                
						bufptr = buffer;
						break;
               }

		        (*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface [%s] buffer len is %d, looking for %d", mrbfsInterfaceDriver->interfaceName, bufptr - buffer, expectedPktLen);

					if (3 == (bufptr - buffer))
						expectedPktLen = (((unsigned int)buffer[1])<<8) + buffer[2] + 4; // length is 3 bytes of header + 1 byte of check + data len

					if ((bufptr - buffer) == expectedPktLen) 
					{
						// Theoretical end of packet
						unsigned char pktChecksum = 0;
						for (i=3; i<expectedPktLen; i++)
							pktChecksum += buffer[i];
						
						if (0xFF != pktChecksum)
						{
							(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface [%s] got pkt with bad checksum - actual=0x%02X rcvd=0x%02X", mrbfsInterfaceDriver->interfaceName, pktChecksum, buffer[expectedPktLen-1]);
						}
						else
						{
							unsigned int pktDataOffset = 8;
							// Finished packet, good checksum
							(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Interface [%s] got pkt with good checksum, API frame type 0x%02X", mrbfsInterfaceDriver->interfaceName, buffer[3]);
							
							switch(buffer[3]) // Handle different API frame types
							{
								// Various packet receive frames, based on address type
								case 0x80: // 64 bit addressing frame
									pktDataOffset = 14;
									// Intentional fall-through
								case 0x81: // 16 bit addressing frame
									{
				                  // It's a data packet
										// Give it back to the control thread
										time_t currentTime = time(NULL);
										MRBusPacket rxPkt;

										memset(&rxPkt, 0, sizeof(MRBusPacket));
										rxPkt.bus = mrbfsInterfaceDriver->bus;
										rxPkt.len = buffer[pktDataOffset + MRBUS_PKT_LEN];
										for(i=0; i<rxPkt.len; i++)
											rxPkt.pkt[i] = buffer[pktDataOffset + i];
										
										nodeLocalStorage->rssi[buffer[pktDataOffset + MRBUS_PKT_SRC]].dBm = -(buffer[pktDataOffset - 2]);
										nodeLocalStorage->rssi[buffer[pktDataOffset + MRBUS_PKT_SRC]].lastUpdate = currentTime;
										
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
									break;
								
								default:
									(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Interface [%s] got API frame 0x%02X, ignoring", mrbfsInterfaceDriver->interfaceName, buffer[3]);
									break;
							}
							
						}
						memset(buffer, 0, sizeof(buffer));
						bufptr = buffer;
						expectedPktLen = 0;
						processingPacket = 0;
					}
					break;
				}
      }

		if (!processingPacket && mrbusPacketQueueDepth(&nodeLocalStorage->txq) )
		{
			MRBusPacket txPkt;
			uint8_t txPktBuffer[32];
			uint8_t txPktBufferEscaped[64];
			uint8_t *txPktPtr, *txPktEscapedPtr;
			uint8_t txPktLen=0, txPktLenWithEscapes=0;
			uint16_t crc16_value = 0;
			uint8_t xbeeChecksum = 0;
			uint32_t i = 0;
			

			
			mrbusPacketQueuePop(&nodeLocalStorage->txq, &txPkt);

			// First, calculate MRBus CRC16 
		   for (i = 0; i < txPkt.pkt[MRBUS_PKT_LEN]; i++)
			{
		      if ((i != MRBUS_PKT_CRC_H) && (i != MRBUS_PKT_CRC_L))
		         crc16_value = mrbusCRC16Update(crc16_value, txPkt.pkt[i]);
			}			
			txPkt.pkt[MRBUS_PKT_CRC_L] = (crc16_value & 0xFF);
			txPkt.pkt[MRBUS_PKT_CRC_H] = ((crc16_value >> 8) & 0xFF);			

			// Now figure out the length of the data segment, before escaping
			txPktLen = txPkt.pkt[MRBUS_PKT_LEN] + 4; 

			txPktPtr = txPktBuffer;
			memset(txPktBuffer, 0, sizeof(txPktBuffer));
			
			*txPktPtr++ = 0x7E; // 0 - Start 
			*txPktPtr++ = 0xFF & (txPktLen>>8);  // 1 - Len MSB
			*txPktPtr++ = 0xFF & txPktLen; // 2 - Len LSB
			*txPktPtr++ = 0x01;  // 3 - API being called - transmit by 16 bit address
			*txPktPtr++ = 0xFF;  // 4 - MSB of dest address - broadcast 0xFFFF
			*txPktPtr++ = 0xFF;  // 5 - LSB of dest address - broadcast 0xFFFF
			*txPktPtr++ = 0x00; 	// 6 - Transmit options

			// Copy over actual packet			
			for(i=0; i<txPkt.pkt[MRBUS_PKT_LEN]; i++)
				*txPktPtr++ = txPkt.pkt[i];
			
			// Add up checksum
			for(i=3; i<(txPktPtr - txPktBuffer); i++)
				xbeeChecksum += txPktBuffer[i];

			xbeeChecksum = 0xFF - xbeeChecksum;
			*txPktPtr++ = xbeeChecksum;
			
			txPktEscapedPtr = txPktBufferEscaped;
			memset(txPktBufferEscaped, 0, sizeof(txPktBufferEscaped));
			
			for(i=0; i<(txPktPtr - txPktBuffer); i++)
			{
				switch(txPktBuffer[i])
				{
					case 0x7E:
					case 0x7D:
					case 0x11:
					case 0x13:
						*txPktEscapedPtr++ = 0x7D;
						*txPktEscapedPtr++ = 0x20 ^ txPktBuffer[i];
						break;
					
					default:
						*txPktEscapedPtr++ = txPktBuffer[i];
						break;
				}
			}

			(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface driver [%s] transmitting %d bytes", mrbfsInterfaceDriver->interfaceName, txPktEscapedPtr - txPktBufferEscaped); 
			write(fd, txPktBufferEscaped, txPktEscapedPtr - txPktBufferEscaped);
		}

   }
   
	(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface driver [%s] terminating", mrbfsInterfaceDriver->interfaceName);
	phtread_exit(NULL);
}


