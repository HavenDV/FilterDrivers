#include "FilterCommon.h"

BOOLEAN         FilterFiltering = FALSE;
DWORD           FilterDebugLevel = BUSDOG_DEBUG_WARN;

WDFCOLLECTION   FilterDeviceCollection;
WDFWAITLOCK     FilterDeviceCollectionLock;

