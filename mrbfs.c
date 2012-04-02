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
#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <confuse.h>
#include "mrbfs.h"
#include "mrbfs-log.h"
#include "mrbfs-filesys.h"
#include "mrbfs-cfg.h"


// Globals
MRBFSConfig* gMrbfsConfig = NULL;

static void* mrbfsInit(struct fuse_conn_info *conn)
{
	int err;
//	err = pthread_create(&mrbfsGlobalListenerThread, NULL, (void*)&mrbfsListener, NULL);
}

static void mrbfsDestroy(void* v)
{
//	pthread_cancel(mrbfsGlobalListenerThread);
   cfg_free(gMrbfsConfig->cfgParms);	
}


static struct fuse_operations mrbfsOperations = 
{
	.getattr	= mrbfsGetattr,
	.readdir	= mrbfsReaddir,
	.open		= mrbfsOpen,
	.read		= mrbfsRead,
	.init    = mrbfsInit,
	.destroy = mrbfsDestroy,
};

static int mrbfs_opt_proc(void *data, const char *arg, int key, struct fuse_args *outargs)
{
     switch (key) 
     {
		  case KEY_HELP:
		          fprintf(stderr,
		                  "usage: %s mountpoint [options]\n"
		                  "\n"
		                  "general options:\n"
		                  "    -o opt,[opt...]  mount options\n"
		                  "    -h   --help      print help\n"
		                  "    -V   --version   print version\n"
		                  "\n"
		                  "MRBFS options:\n"
		                  "    -d <DEBUG LEVEL 0-2, 0=errors only, 1=warnings, 2=debug\n"
		                  "    -c <CONFIG FILE LOCATION>mybool=BOOL    same as 'mybool' or 'nomybool'\n"
		                  , outargs->argv[0]);
		          fuse_opt_add_arg(outargs, "-hocd");
		          fuse_main(outargs->argc, outargs->argv, &mrbfsOperations, NULL);
		          exit(1);

		  case KEY_VERSION:
		          fprintf(stderr, "MRBFS version %s\n", MRBFS_VERSION);
		          fuse_opt_add_arg(outargs, "--version");
		          fuse_main(outargs->argc, outargs->argv, &mrbfsOperations, NULL);
		          exit(0);
     }
     return 1;
}

void mrbfsSingleInitConfig()
{
	const char* configFileStr = "mrbfs.conf";
	int ret=0;
		
	if (NULL != gMrbfsConfig->configFileStr && 0 != strlen(gMrbfsConfig->configFileStr))
		configFileStr = gMrbfsConfig->configFileStr;

	gMrbfsConfig->cfgParms = cfg_init(opts, CFGF_NOCASE);
	ret = cfg_parse(gMrbfsConfig->cfgParms, configFileStr);

	if (ret == CFG_FILE_ERROR)
	{
		char* errorStr = "Config file not found";
		ret = asprintf(&errorStr, "Config file [%s] not found, exiting...", configFileStr);
		perror(errorStr);
		free(errorStr);
		exit(1);
	} else if (ret == CFG_PARSE_ERROR) {
		char* errorStr = "Config file error";
		ret = asprintf(&errorStr, "Config file [%s] failed parsing, exiting...", configFileStr);
		perror(errorStr);
		free(errorStr);
		exit(1);
	}
}

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	
	if (NULL == (gMrbfsConfig = calloc(1, sizeof(MRBFSConfig))))
	{
		perror("Failed allocation of global configuration structure, exiting...\n");
		exit(1);
	}
	fuse_opt_parse(&args, &gMrbfsConfig, mrbfs_opts, NULL);

	// Okay, we've theoretically parsed any configuration options from the command line.
	// Go try to load our configuration file
	mrbfsSingleInitConfig();

	// Okay, configuration file is loaded, start logging
	mrbfsSingleInitLogging();
	
	// At this point, we've got our configuration and logging is active
	// Log a startup message and get on with starting the filesystem
	mrbfsLogMessage(MRBFS_LOG_ERROR, "MRBFS Startup");

	return fuse_main(args.argc, args.argv, &mrbfsOperations, mrbfs_opt_proc);
}

int fileExists(const char* filename)
{
	struct stat info;
	if (0 == stat(filename, &info))
		return(1);
	return(0);
}

int mrbfsOpenInterfaces()
{
	int interfaces = cfg_size(gMrbfsConfig->cfgParms, "interface");
	int i=0;

	gMrbfsConfig->mrbfsUsedInterfaces = 0;
	
	if (0 == interfaces)
		mrbfsLogMessage(MRBFS_LOG_WARNING, "No interfaces configured - proceeding, but this is slightly nuts");


	for(i=0; i<interfaces; i++)
	{
		char* modulePath = NULL;
		void* interfaceDriverHandle = NULL;
		MRBFSInterfaceModule* mrbfsInterfaceModule = NULL;
		int (*mrbfsInterfaceModuleVersionCheck)(int);
		int ret;
		cfg_t *cfgInterface = cfg_getnsec(gMrbfsConfig->cfgParms, "interface", i);
		
		mrbfsLogMessage(MRBFS_LOG_INFO, "Setting up interface [%s]", cfg_title(cfgInterface));
		ret = asprintf(&modulePath, "%s/%s", cfg_getstr(gMrbfsConfig->cfgParms, "module-directory"), cfg_getstr(cfgInterface, "driver"));
				
		// First, test if the driver module exists
		if (!fileExists(modulePath))
		{
			mrbfsLogMessage(MRBFS_LOG_ERROR, "Interface [%s] - driver module not found at [%s]", cfg_title(cfgInterface), modulePath);
			free(modulePath);
			continue;
		}

		// Test to make sure the dynamic linker can open it
		if (NULL == (interfaceDriverHandle= (void*)dlopen(modulePath, RTLD_LAZY))) 
		{
			mrbfsLogMessage(MRBFS_LOG_ERROR, "Interface [%s] - driver module failed dlopen [%s]", cfg_title(cfgInterface), NULL!=dlerror()?dlerror():"");
			free(modulePath);
			continue;
		}

		free(modulePath);

		// Now do some cursory version checks
		mrbfsInterfaceModuleVersionCheck = dlsym(interfaceDriverHandle, "mrbfsInterfaceModuleVersionCheck");
		if(NULL == mrbfsInterfaceModuleVersionCheck || !(*mrbfsInterfaceModuleVersionCheck)(MRBFS_INTERFACE_MODULE_VERSION))
		{
			mrbfsLogMessage(MRBFS_LOG_ERROR, "Interface [%s] - module version check failed");
			continue;
		}
		
		// Okay, looks good, add it to the interface list and run the init function

		mrbfsInterfaceModule = calloc(1, sizeof(MRBFSInterfaceModule));
		mrbfsInterfaceModule->interfaceDriverHandle = interfaceDriverHandle;
		mrbfsInterfaceModule->bus = cfg_getint(cfgInterface, "driver");
		mrbfsInterfaceModule->port = strdup(cfg_getstr(cfgInterface, "port"));
		mrbfsInterfaceModule->addr = strtol(cfg_getstr(cfgInterface, "interface-address"), NULL, 36);

		// Hook up the callbacks for the driver to talk to the main thread
		//mrbfsInterfaceModule->mrbfsGetNode = &mrbfsGetNode;
		mrbfsInterfaceModule->mrbfsLogMessage = &mrbfsLogMessage;
	
		
		gMrbfsConfig->mrbfsInterfaceModules[gMrbfsConfig->mrbfsUsedInterfaces++] = mrbfsInterfaceModule;

		mrbfsLogMessage(MRBFS_LOG_INFO, "Interface [%s] successfully set up in slot %d", cfg_title(cfgInterface), gMrbfsConfig->mrbfsUsedInterfaces-1);

		
	}


}

