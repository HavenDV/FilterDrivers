#include "filter.h"

VOID PrintChars(__in_ecount(CountChars) PCHAR BufferAddress, __in ULONG CountChars){
    if(FilterDebugLevel >= FILTER_DEBUG_INFO){
        if(CountChars){
            while(CountChars--) {
                if (*BufferAddress > 31 && *BufferAddress != 127)
                    KdPrint (( "%c", *BufferAddress) );
                else
                    KdPrint(( ".") );
                BufferAddress++;
            }
            KdPrint (("\n"));
        }
    }
    return;
}

FILTER_TIMESTAMP FilterGetTimeStamp(VOID){
    LARGE_INTEGER Time, TimeFreq;
    LONG tmp;
    
    FILTER_TIMESTAMP ts;

    Time = KeQueryPerformanceCounter(&TimeFreq);
    tmp = (LONG)(Time.QuadPart / TimeFreq.QuadPart);

    ts.sec = tmp;
    ts.usec = (LONG)((Time.QuadPart % TimeFreq.QuadPart) * 1000000 / TimeFreq.QuadPart);

    if (ts.usec >= 1000000){
        ts.sec++;
        ts.usec -= 1000000;
    }

    return ts;
}

VOID FilterUpdateDeviceIds(VOID){
    ULONG count;
    ULONG i;

    PAGED_CODE();

    WdfWaitLockAcquire(FilterDeviceCollectionLock, NULL);
    count = WdfCollectionGetCount(FilterDeviceCollection);

    for (i = 0; i < count; i++){
        WDFDEVICE device = WdfCollectionGetItem(FilterDeviceCollection, i);
        PFILTER_CONTEXT context = FilterGetDeviceContext(device);
        
        if (!context->HasDeviceId){
            ULONG j;
            ULONG newDevId = 0;
            BOOLEAN idColision;

            do{
                idColision = FALSE;
                for (j = 0; j < count; j++){
                    PFILTER_CONTEXT contextCompare = FilterGetDeviceContext(WdfCollectionGetItem(FilterDeviceCollection, j));
                    if (contextCompare->HasDeviceId && contextCompare->DeviceId == newDevId){
                        newDevId++;
                        idColision = TRUE;
                        break;
                    }
                }
            } while(idColision);

            context->HasDeviceId = TRUE;
            context->DeviceId = newDevId;
            FilterPrint(FILTER_DEBUG_INFO, "New DeviceId: %2d\n", newDevId);
        }
    }

    WdfWaitLockRelease(FilterDeviceCollectionLock);
}

BOOLEAN FilterFillBufferWithDeviceIds(PVOID Buffer, size_t BufferSize, size_t* BytesWritten, size_t* BytesNeeded){
    ULONG count;
    ULONG i;
    BOOLEAN Result = TRUE;

    PAGED_CODE();

    *BytesWritten = 0;
    *BytesNeeded = 0;

    WdfWaitLockAcquire(FilterDeviceCollectionLock, NULL);
    count = WdfCollectionGetCount(FilterDeviceCollection);
    for (i = 0; i < count ; i++){
        NTSTATUS status;
        WDFDEVICE device;
        PFILTER_CONTEXT context;
        WDF_OBJECT_ATTRIBUTES attributes;
        WDFMEMORY memory;
        size_t hwidMemorySize;
        PVOID pdoNameBuffer;
        UNICODE_STRING pdoName;
        size_t deviceIdSize;
        PFILTER_DEVICE_ID pDeviceId;

        device = WdfCollectionGetItem(FilterDeviceCollection, i);
        context = FilterGetDeviceContext(device);

        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = device;

        status = WdfDeviceAllocAndQueryProperty(device, DevicePropertyPhysicalDeviceObjectName, NonPagedPool, &attributes, &memory);
        if(!NT_SUCCESS(status)){
            FilterPrint(FILTER_DEBUG_ERROR, "WdfDeviceAllocAndQueryProperty failed - 0x%x\n", status);
            continue;
        }
        pdoNameBuffer = WdfMemoryGetBuffer(memory, &hwidMemorySize);
        if (pdoNameBuffer == NULL){   
            FilterPrint(FILTER_DEBUG_ERROR, "WdfMemoryGetBuffer failed\n");
            continue;   
        }   

        RtlInitUnicodeString(&pdoName, pdoNameBuffer);
        FilterPrint(FILTER_DEBUG_INFO, "%2d - Enabled: %d, PDO Name: %wZ\n", context->DeviceId, context->FilterEnabled, &pdoName);

        deviceIdSize=sizeof(FILTER_DEVICE_ID) + pdoName.Length;
        if(deviceIdSize > BufferSize - *BytesWritten){
            Result = FALSE;
            FilterPrint(FILTER_DEBUG_WARN, "No room for device id\n");
        }
        else{
            pDeviceId = (PFILTER_DEVICE_ID)((PCHAR)Buffer + *BytesWritten);
            pDeviceId->DeviceId = context->DeviceId;
            pDeviceId->Enabled = context->FilterEnabled;
            pDeviceId->PhysicalDeviceObjectNameSize = pdoName.Length;
            RtlCopyMemory((PCHAR)Buffer + *BytesWritten + sizeof(FILTER_DEVICE_ID), pdoName.Buffer, pdoName.Length);
            *BytesWritten += deviceIdSize;
        }
        *BytesNeeded += deviceIdSize;
    }
    WdfWaitLockRelease(FilterDeviceCollectionLock);

    return Result;
}
