#ifndef _MRBFS_LOG_H
#define _MRBFS_LOG_H
int mrbfsLogMessage(mrbfsLogLevel logLevel, const char* format, ...);
void mrbfsSingleInitLogging();
#endif

