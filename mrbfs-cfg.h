#include "confuse.h"

cfg_opt_t opts[] =
{
   CFG_STR("log-file", "mrbfs.log", CFGF_NONE),
   CFG_SEC("module", module_opts, CFGF_MULTI | CFGF_TITLE),
   CFG_SEC("page", page_opts, CFGF_MULTI | CFGF_TITLE),
   CFG_END()
};

