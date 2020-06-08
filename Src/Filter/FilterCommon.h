#include <ntddk.h>
#include <wdf.h>
#include <Usbioctl.h>
#include <usb.h>

#define NT_INCLUDED
#include "WinDef.h"

#include "FilterUserCommon.h"
#include "FileData.h"

#if !defined(_FILTER_COMMON_H_)
#define _FILTER_COMMON_H_

#define DRIVERNAME "filter.sys: "

#define NTDEVICE_NAME_STRING      L"\\Device\\Filter"
#define SYMBOLIC_NAME_STRING      L"\\DosDevices\\Filter"

#define DEVICE_CONTEXT_MAGIC    0x98761234

typedef struct _FILTER_CONTEXT {
    ULONG       MagicNumber;
    BOOLEAN     HasDeviceId;
    ULONG       DeviceId;
    BOOLEAN     FilterEnabled;
    WDFIOTARGET TargetToSendRequestsTo;
} FILTER_CONTEXT, *PFILTER_CONTEXT;

#define IS_DEVICE_CONTEXT(_DC_) (((_DC_)->MagicNumber) == DEVICE_CONTEXT_MAGIC)

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FILTER_CONTEXT, FilterGetDeviceContext)

typedef struct _CONTROL_DEVICE_EXTENSION {
    PVOID ControlData;
} CONTROL_DEVICE_EXTENSION, *PCONTROL_DEVICE_EXTENSION;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CONTROL_DEVICE_EXTENSION, ControlGetData)

typedef struct{
    ULONG ItemSize;
} FILTER_TRACE_FIFO_ITEM, *PFILTER_FILTER_TRACE_FIFO_ITEM;

#define FILTER_TRACE_FIFO_LENGTH 100

typedef struct{
    PFILTER_FILTER_TRACE_FIFO_ITEM  TraceItems[FILTER_TRACE_FIFO_LENGTH];
    ULONG                           WriteIndex;
    ULONG                           ReadIndex;
} FILTER_TRACE_FIFO;

DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_UNLOAD FilterDriverUnload;
EVT_WDF_DRIVER_DEVICE_ADD FilterDeviceAdd;
EVT_WDF_DRIVER_UNLOAD FilterDriverUnload;
EVT_WDF_DEVICE_CONTEXT_CLEANUP FilterDeviceContextCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL FilterIoDeviceControl;

NTSTATUS FilterCreateControlDevice(WDFDEVICE Device);
VOID     FilterDeleteControlDevice(WDFDEVICE Device);
VOID     FilterIoRead(IN WDFQUEUE Queue, IN WDFREQUEST Request, IN size_t Length);
VOID     FilterReadComplete(IN WDFREQUEST Request, IN WDFIOTARGET Target, IN PWDF_REQUEST_COMPLETION_PARAMS Params, IN WDFCONTEXT Context);
VOID     FilterIoWrite(IN WDFQUEUE Queue, IN WDFREQUEST Request, IN size_t Length);
VOID     FilterForwardRequest(IN WDFDEVICE Device, IN WDFREQUEST Request);
VOID     FilterForwardRequestWithCompletion(IN WDFDEVICE Device, IN WDFREQUEST Request, IN PFN_WDF_REQUEST_COMPLETION_ROUTINE CompletionRoutine, IN WDFCONTEXT CompletionContext);
VOID     FilterIoInternalDeviceControl(IN WDFQUEUE  Queue, IN WDFREQUEST  Request, IN size_t  OutputBufferLength, IN size_t  InputBufferLength, IN ULONG  IoControlCode);
VOID     FilterIoInternalDeviceControlComplete(IN WDFREQUEST Request, IN WDFIOTARGET Target, IN PWDF_REQUEST_COMPLETION_PARAMS Params, IN WDFCONTEXT Context);

VOID 	 FilterProcessInternalDeviceControl(IN WDFDEVICE Device, IN PFILTER_CONTEXT Context, IN WDFREQUEST  Request, IN ULONG  IoControlCode, IN BOOLEAN bCompletion, OUT BOOLEAN* bRead);

VOID	 PrintChars(__in_ecount(CountChars) PCHAR BufferAddress, __in ULONG CountChars);

FILTER_TIMESTAMP FilterGetTimeStamp(VOID);
VOID 			 FilterUpdateDeviceIds(VOID);
BOOLEAN	         FilterFillBufferWithDeviceIds(PVOID Buffer, size_t BufferSize, size_t* BytesWritten, size_t* BytesNeeded);

NTSTATUS FilterTraceFifoInit(WDFDRIVER Driver);
VOID     FilterTraceFifoCleanUp(VOID);

VOID     FilterAddTraceToFifo(WDFDEVICE device, ULONG DeviceId, FILTER_REQUEST_TYPE Type, FILTER_REQUEST_PARAMS Params, PVOID TraceBuffer, ULONG BufferLength);

NTSTATUS FilterFufillRequestWithTraces(IN WDFREQUEST Request, OUT size_t* bytesWritten);

#define FlagOn(F,SF) ( \
    (((F) & (SF)))     \
)

#if DBG
#define FilterPrint(dbglevel, fmt, ...) {  \
    if (FilterDebugLevel >= dbglevel)      \
    {                                      \
        DbgPrint(DRIVERNAME);              \
        DbgPrint(fmt, __VA_ARGS__);        \
    }                                      \
}
#else
#define FilterPrint(dbglevel, fmt, ...) {  \
}
#endif

#endif
