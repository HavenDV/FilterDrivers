#include "filter.h"

BOOLEAN         FilterFiltering = FALSE;
DWORD           FilterDebugLevel = 0;// BUSDOG_DEBUG_WARN;

WDFCOLLECTION   FilterDeviceCollection;
WDFWAITLOCK     FilterDeviceCollectionLock;

