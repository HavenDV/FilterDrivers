#include "FilterCommon.h"

FILTER_TRACE_FIFO    FilterTraceFifo;
WDFSPINLOCK          FilterTraceFifoLock;

extern WDFQUEUE BufferRequestQueue;

NTSTATUS __FilterFufillRequestWithTraces(IN WDFREQUEST Request, OUT size_t* bytesWritten);

NTSTATUS FilterTraceFifoInit(WDFDRIVER Driver){
    NTSTATUS status = STATUS_SUCCESS;
    WDF_OBJECT_ATTRIBUTES attributes;

    PAGED_CODE ();

    RtlZeroMemory(&FilterTraceFifo.TraceItems[0], FILTER_TRACE_FIFO_LENGTH * sizeof(PFILTER_FILTER_TRACE_FIFO_ITEM));
    FilterTraceFifo.WriteIndex = 0;
    FilterTraceFifo.ReadIndex = 0;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Driver;

    status = WdfSpinLockCreate(&attributes, &FilterTraceFifoLock);
    if(!NT_SUCCESS(status))
        FilterPrint(FILTER_DEBUG_ERROR, "WdfWaitLockCreate failed with status 0x%x\n", status);

    return status;
}

VOID FilterTraceFifoCleanUp(VOID){
    ULONG i;

    PAGED_CODE ();

    WdfSpinLockAcquire(FilterTraceFifoLock);
    for(i=0; i<FILTER_TRACE_FIFO_LENGTH; i++){
        if(FilterTraceFifo.TraceItems[i] != NULL){
            ExFreePool(FilterTraceFifo.TraceItems[i]);
            FilterTraceFifo.TraceItems[i] = NULL;
        }
    }

    FilterTraceFifo.WriteIndex = 0;
    FilterTraceFifo.ReadIndex = 0;
    WdfSpinLockRelease(FilterTraceFifoLock);
}

PFILTER_FILTER_TRACE_FIFO_ITEM __FilterCreateTrace(PFILTER_FILTER_TRACE_FIFO_ITEM pTraceItem, ULONG DeviceId, FILTER_REQUEST_TYPE Type, FILTER_REQUEST_PARAMS Params, PVOID TraceBuffer, ULONG BufferLength){
    PFILTER_FILTER_TRACE pTrace;
    ULONG requiredItemSize = sizeof(FILTER_TRACE_FIFO_ITEM)+sizeof(FILTER_TRACE)+BufferLength;
    if(pTraceItem != NULL && pTraceItem->ItemSize < requiredItemSize){
        ExFreePool(pTraceItem);
        pTraceItem = NULL;
    }

    if(pTraceItem == NULL){
        pTraceItem = ExAllocatePoolWithTag(NonPagedPool, requiredItemSize, 'GATT');
        if(pTraceItem == NULL){
            FilterPrint(FILTER_DEBUG_ERROR, "ExAllocatePoolWithTag failed\n");
            return NULL;
        }
        pTraceItem->ItemSize = requiredItemSize;
    }

    pTrace = (PFILTER_FILTER_TRACE)((PCHAR)pTraceItem + sizeof(FILTER_TRACE_FIFO_ITEM));
    pTrace->DeviceId = DeviceId;
    pTrace->Type = Type;
    pTrace->Params = Params;
    pTrace->Timestamp = FilterGetTimeStamp();
    pTrace->BufferSize = BufferLength;

    if(BufferLength > 0)
        RtlCopyMemory((PCHAR)pTrace + sizeof(FILTER_TRACE), TraceBuffer, BufferLength);
    return pTraceItem;
}

VOID FilterAddTraceToFifo(WDFDEVICE device, ULONG DeviceId, FILTER_REQUEST_TYPE Type, FILTER_REQUEST_PARAMS Params, PVOID TraceBuffer, ULONG BufferLength){
    PFILTER_FILTER_TRACE_FIFO_ITEM pTraceItem = NULL; 
    WDFREQUEST request;
    NTSTATUS status;

    WdfSpinLockAcquire(FilterTraceFifoLock);
    pTraceItem = FilterTraceFifo.TraceItems[FilterTraceFifo.WriteIndex];
    
	pTraceItem = __FilterCreateTrace(pTraceItem, DeviceId, Type, Params, TraceBuffer, BufferLength);
    FilterTraceFifo.TraceItems[FilterTraceFifo.WriteIndex]=pTraceItem;
    FilterTraceFifo.WriteIndex++;

    if(FilterTraceFifo.WriteIndex >= FILTER_TRACE_FIFO_LENGTH)
        FilterTraceFifo.WriteIndex=0;

    if(FilterTraceFifo.WriteIndex == FilterTraceFifo.ReadIndex)
        FilterPrint(FILTER_DEBUG_ERROR, "On noes! We have overflow\n");

    status = WdfIoQueueRetrieveNextRequest(BufferRequestQueue, &request);
    if (NT_SUCCESS(status)){
        size_t bytesWritten;
        status = __FilterFufillRequestWithTraces(request, &bytesWritten);

        WdfRequestCompleteWithInformation(request, status, bytesWritten);
    }
    else{
        if(status != STATUS_NO_MORE_ENTRIES)
            FilterPrint(FILTER_DEBUG_ERROR, "WdfIoQueueRetrieveNextRequest failed - 0x%x\n", status);
    }

    WdfSpinLockRelease(FilterTraceFifoLock);
}

PFILTER_FILTER_TRACE _FilterRetrieveTrace(VOID){
    PFILTER_FILTER_TRACE pTrace = NULL;

    if(FilterTraceFifo.ReadIndex != FilterTraceFifo.WriteIndex){
        PFILTER_FILTER_TRACE_FIFO_ITEM pTraceItem = FilterTraceFifo.TraceItems[FilterTraceFifo.ReadIndex];
        if (pTraceItem == NULL){
            FilterPrint(FILTER_DEBUG_ERROR, "On noes! invalid trace\n");
            return NULL;
        }

        pTrace=(PFILTER_FILTER_TRACE)((PCHAR)pTraceItem + sizeof(FILTER_TRACE_FIFO_ITEM));
        FilterTraceFifo.ReadIndex++;

        if(FilterTraceFifo.ReadIndex >= FILTER_TRACE_FIFO_LENGTH)
            FilterTraceFifo.ReadIndex = 0;
    }

    return pTrace;
}

size_t __FilterRetrieveTraceSize(VOID){
    PFILTER_FILTER_TRACE_FIFO_ITEM pTraceItem=FilterTraceFifo.TraceItems[FilterTraceFifo.ReadIndex];
    PFILTER_FILTER_TRACE pTrace;
    if (pTraceItem == NULL){
        FilterPrint(FILTER_DEBUG_ERROR, "On noes! invalid trace\n");
        return 0;
    }

    pTrace=(PFILTER_FILTER_TRACE)((PCHAR)pTraceItem + sizeof(FILTER_TRACE_FIFO_ITEM));
    return sizeof(FILTER_TRACE) + pTrace->BufferSize;
}

NTSTATUS __FilterFillBufferWithTraces(PVOID Buffer, size_t BufferSize, OUT size_t* BytesWritten){
    FILTER_TRACE pTrace=NULL;
    NTSTATUS status=STATUS_SUCCESS;
    size_t TraceSize;

    *BytesWritten = 0;
    while(TRUE){
        TraceSize = __FilterRetrieveTraceSize();
        if (TraceSize > BufferSize - *BytesWritten){
            FilterPrint(FILTER_DEBUG_WARN, "No room for next trace\n");
            if(*BytesWritten == 0)
                status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        pTrace=__FilterRetrieveTrace();
        if(pTrace == NULL){
            FilterPrint(FILTER_DEBUG_INFO, "No more traces\n");

            if(*BytesWritten == 0)
                status = STATUS_NO_DATA_DETECTED;
            break;
        }

        FilterPrint(FILTER_DEBUG_INFO, "Got trace %d\n", pTrace);

        RtlCopyMemory((PCHAR)Buffer + *BytesWritten, pTrace, TraceSize);
        *BytesWritten += TraceSize;
        FilterPrint(FILTER_DEBUG_INFO, "     Bytes written %d\n", *BytesWritten);
    }

    return status;
}

NTSTATUS __FilterFufillRequestWithTraces(IN WDFREQUEST Request, OUT size_t* bytesWritten){
    PVOID    outputBuffer = NULL;
    size_t   realLength;
    NTSTATUS status = STATUS_SUCCESS;    

    *bytesWritten = 0;
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(FILTER_TRACE), &outputBuffer, &realLength);

    if (NT_SUCCESS(status)) 
        status = __FilterFillBufferWithTraces(outputBuffer, realLength, bytesWritten);
    else
        FilterPrint(FILTER_DEBUG_ERROR, "WdfRequestRetrieveOutputBuffer failed - 0x%x\n", status);

    return status;
}

NTSTATUS FilterFufillRequestWithTraces(IN WDFREQUEST Request, OUT size_t* bytesWritten){
    NTSTATUS status;    

    WdfSpinLockAcquire(FilterTraceFifoLock);
    status = __FilterFufillRequestWithTraces(Request, bytesWritten);
    WdfSpinLockRelease(FilterTraceFifoLock);

    return status;
}


