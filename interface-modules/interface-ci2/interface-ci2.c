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


int mrbfsInterfaceModuleVersionCheck(int ifaceVersion)
{
	if (ifaceVersion != MRBFS_INTERFACE_MODULE_VERSION)
		return(0);
	return(1);
}

static void mrbfsCI2SerialClose(int fd)
{
}

static int mrbfsCI2SerialOpen(const char* device, speed_t baudrate)
{
	int fd, n;
	struct termios options;
	int  nbytes;       // Number of bytes read 
	struct timeval timeout;

	fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY); 
   if (fd < 0)
	{
		perror(device); exit(-1); 
	}
	fcntl(fd, F_SETFL, 0);

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



void mrbfsInterfaceModuleRun(MRBFSInterfaceModule* mrbfsInterfaceModule)
{
	UINT8 buffer[256];
	UINT8 *bufptr;      // Current char in buffer 
   UINT8 pktBuf[256];
   UINT8 incomingByte[2];
	int fd = -1, nbytes=0;	

	(*mrbfsInterfaceModule->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface module [%s] confirms startup", mrbfsInterfaceModule->interfaceName);

   memset(buffer, 0, sizeof(buffer));
   bufptr = buffer;

	fd = mrbfsCI2SerialOpen("/dev/ttyS0", B115200);
	(*mrbfsInterfaceModule->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface module [%s] opened serial on %d", mrbfsInterfaceModule->interfaceName, fd);


   while(!mrbfsInterfaceModule->terminate)
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
   
	(*mrbfsInterfaceModule->mrbfsLogMessage)(MRBFS_LOG_INFO, "Interface module [%s] terminating", mrbfsInterfaceModule->interfaceName);   
	phtread_exit(NULL);
}


