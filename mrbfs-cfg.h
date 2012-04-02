#ifndef _MRBFS_CFG_H
#define _MRBFS_CFG_H
#include "confuse.h"

enum {
     KEY_HELP,
     KEY_VERSION,
};

#define MRBFS_OPT(t, p, v) { t, offsetof(MRBFSConfig, p), v }



static struct fuse_opt mrbfs_opts[] = {
     MRBFS_OPT("-c %s",        configFileStr, 0),
     MRBFS_OPT("-d %i",       logLevel, 0),
     FUSE_OPT_KEY("-V",             KEY_VERSION),
     FUSE_OPT_KEY("--version",      KEY_VERSION),
     FUSE_OPT_KEY("-h",             KEY_HELP),
     FUSE_OPT_KEY("--help",         KEY_HELP),
     FUSE_OPT_END     
};

cfg_opt_t opts[] =
{
   CFG_STR("log-file", "mrbfs.log", CFGF_NONE),
//   CFG_SEC("module", module_opts, CFGF_MULTI | CFGF_TITLE),
//   CFG_SEC("page", page_opts, CFGF_MULTI | CFGF_TITLE),
   CFG_END()
};

#endif
