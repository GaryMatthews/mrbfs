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


int mrbfsInterfaceDriverVersionCheck(int ifaceVersion)
{
	if (ifaceVersion != MRBFS_INTERFACE_DRIVER_VERSION)
		return(0);
	return(1);
}

static void mrbfsCI2SerialClose(int fd)
{
}

static int mrbfsCI2SerialOpen(const char* device, speed_t baudrate, int (*mrbfsLogMessage)(mrbfsLogLevel, const char*, ...))
{
	int fd, n;
	struct termios options;
	int  nbytes;       // Number of bytes read 
	struct timeval timeout;

	(*mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Begin serial startup - opening [%s]", device);

//	fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY | O_NONBLOCK); 
	fd = open("/dev/ttyUSB0", O_RDWR | O_NOCTTY | O_NDELAY); 
   if (fd < 0)
	{
		(*mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Oooh, fd is bad, %d", fd);
		perror(device); 
		return(0); 
	}
	(*mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Past opening serial port1");
	fcntl(fd, F_SETFL, 0);


	(*mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Past opening serial port2");
	tcgetattr(fd,&options); // save current serial port settings 



	cfsetispeed(&options, baudrate);
	cfsetospeed(&options, baudrate);

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

	return(fd);

}



void mrbfsInterfaceDriverRun(MRBFSInterfaceDriver* mrbfsInterfaceDriver)
{
	UINT8 buffer[256];
	UINT8 *bufptr;      // Current char in buffer 
   UINT8 pktBuf[256];
   UINT8 incomingByte[2];
	const char* device="/dev/ttyUSB0";
	struct termios options;
	struct timeval timeout;

	int fd = -1, nbytes=0, i=0;	

	(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface driver [%s] confirms startup", mrbfsInterfaceDriver->interfaceName);

   memset(buffer, 0, sizeof(buffer));
   bufptr = buffer;

	(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface driver [%s] alive", mrbfsInterfaceDriver->interfaceName);
	if(0);
	{

		(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Begin serial startup - opening [%s]", device);

		fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY); 
		if (fd < 0)
		{
			(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Oooh, fd is bad, %d", fd);
			perror(device); 
		}
		(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Past opening serial port1");
		fcntl(fd, F_SETFL, 0);


		(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Past opening serial port2");
		tcgetattr(fd,&options); // save current serial port settings 



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

	}


   while(1)
   {
      usleep(1000);
		i++;
		if (i>100)
		{
			i=0;
			(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface driver [%s] alive", mrbfsInterfaceDriver->interfaceName);
		}

	}



//	fd = mrbfsCI2SerialOpen("/dev/ttyUSB0", B115200, mrbfsInterfaceDriver->mrbfsLogMessage);
	(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface driver [%s] opened serial on %d", mrbfsInterfaceDriver->interfaceName, fd);


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
						(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_DEBUG, "Interface driver [%s] got packet [%s]", mrbfsInterfaceDriver->interfaceName, buffer+2);


						(*mrbfsInterfaceDriver->mrbfsPacketReceive)(&rxPkt);
//                  memset(mrbfsGlobalData.pktData, 0, sizeof(mrbfsGlobalData.pktData));
 //                 strncpy(mrbfsGlobalData.pktData, buffer, sizeof(mrbfsGlobalData.pktData)-1);
 //                 mrbfsGlobalData.pktsReceived++;
  //                mrbfsGlobalData.pktAvailable=1;

               }

               memset(buffer, 0, sizeof(buffer));
               bufptr = buffer;
               break;

            default:
               *bufptr++ = incomingByte[0];
               break;
         }
      }
   }
   
	(*mrbfsInterfaceDriver->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface driver [%s] terminating", mrbfsInterfaceDriver->interfaceName);   
	phtread_exit(NULL);
}


