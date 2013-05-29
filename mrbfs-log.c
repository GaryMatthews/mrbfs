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
#include <confuse.h>
#include "mrbfs.h"

int mrbfsLogMessage(mrbfsLogLevel logLevel, const char* format, ...)
{
	va_list argptr;
	time_t logTime = time(NULL);
	struct tm logTimeTM;
	char logTimeStr[256];

	localtime_r(&logTime, &logTimeTM);
	strftime(logTimeStr, sizeof(logTimeStr), "[%Y-%b-%d %H:%M:%S %z] ", &logTimeTM);
	
	// Ignore anything logging at a higher (less important) level than we're running at
	if (logLevel > gMrbfsConfig->logLevel)
		return; 
		
	pthread_mutex_lock(&gMrbfsConfig->logLock);

	switch(logLevel)
	{
		case MRBFS_LOG_SYSTEM:
			fprintf(gMrbfsConfig->logFile, "%s**SYSTEM** ", logTimeStr);
			break;
					
		case MRBFS_LOG_ERROR:
			fprintf(gMrbfsConfig->logFile, "%s**ERROR** ", logTimeStr);
			break;
		case MRBFS_LOG_WARNING:
			fprintf(gMrbfsConfig->logFile, "%s**WARNING** ", logTimeStr);
			break;
		
		case MRBFS_LOG_INFO:
		case MRBFS_LOG_DEBUG:
		default:
			fprintf(gMrbfsConfig->logFile, "%s", logTimeStr);
			break;
	}


	va_start(argptr, format);
	vfprintf(gMrbfsConfig->logFile, format, argptr);
	va_end(argptr);
	fprintf(gMrbfsConfig->logFile, "\n");
	fflush(gMrbfsConfig->logFile);
	pthread_mutex_unlock(&gMrbfsConfig->logLock);

}

void mrbfsSingleInitLogging()
{
	const char* logFileStr = cfg_getstr(gMrbfsConfig->cfgParms, "log-file");
	int ret;
	pthread_mutexattr_t lockAttr;
	
	if (NULL == logFileStr || 0 == strlen(logFileStr))
		logFileStr = "mrbfs.log";

	if (NULL == (gMrbfsConfig->logFile = fopen(logFileStr, "w")))
	{
		char* errorStr = "Cannot open log file";
		ret = asprintf(&errorStr, "Cannot open log file [%s], exiting...", logFileStr);
		perror(errorStr);
		free(errorStr);
		exit(1);
	}

	// Initialize the log lock
	pthread_mutexattr_init(&lockAttr);
	pthread_mutexattr_settype(&lockAttr, PTHREAD_MUTEX_ADAPTIVE_NP);
	pthread_mutex_init(&gMrbfsConfig->logLock, &lockAttr);
	pthread_mutexattr_destroy(&lockAttr);	
	
	mrbfsLogMessage(MRBFS_LOG_SYSTEM, "Logging started at level %d", gMrbfsConfig->logLevel);
	
}

