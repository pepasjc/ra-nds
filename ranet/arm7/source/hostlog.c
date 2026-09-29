// Routes calico's dietPrint to the host log (separate file: calico headers
// don't mix with libnds')
#include <calico/types.h>
#include <calico/system/dietprint.h>
#include "ranet_host.h"

static void hostPrint(const char* buf, size_t size)
{
	ranetHostLog(buf, size);
}

void dietPrintSetFuncHost(void)
{
	dietPrintSetFunc(hostPrint);
}
