#ifndef _MRBFS_H
#define _MRBFS_H

#include "confuse.h"
#include "mrbfs-types.h"

typedef unsigned long UINT32 ;
typedef unsigned char UINT8 ;



typedef struct 
{
   mrbfsLogLevel logLevel;
	const char *configFileStr;
   cfg_t* cfgParms;
   FILE* logFile;
  	pthread_mutex_t logLock;
} MRBFSConfig;



extern MRBFSConfig* gMrbfsConfig;

#endif

