#if !defined(_FILTER_DATA_H_)
#define _FILTER_DATA_H_

extern BOOLEAN        FilterFiltering;
extern DWORD          FilterDebugLevel;

extern WDFCOLLECTION  FilterDeviceCollection;
extern WDFWAITLOCK    FilterDeviceCollectionLock;

#endif

