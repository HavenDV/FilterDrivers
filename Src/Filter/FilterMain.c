#include "FilterCommon.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, FilterDeviceAdd)
#pragma alloc_text (PAGE, FilterDeviceContextCleanup)

#endif

WDFDEVICE       ControlDevice = NULL;
WDFQUEUE        BufferRequestQueue;
BOOLEAN         FilterAutoTrace = FALSE;

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, FilterIoDeviceControl)
#pragma alloc_text (PAGE, FilterCreateControlDevice)
#pragma alloc_text (PAGE, FilterDeleteControlDevice)
#endif

NTSTATUS DriverEntry(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath){
    WDF_DRIVER_CONFIG   config;
    NTSTATUS            status;
    WDF_OBJECT_ATTRIBUTES colAttributes;
    WDFDRIVER   hDriver;

    FilterPrint(FILTER_DEBUG_INFO, "DriverEntry\n");
    FilterPrint(FILTER_DEBUG_INFO, "Built %s %s\n", __DATE__, __TIME__);

    WDF_DRIVER_CONFIG_INIT(&config, FilterDeviceAdd);
    config.EvtDriverUnload = FilterDriverUnload;
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &hDriver);
    if(!NT_SUCCESS(status))
        FilterPrint(FILTER_DEBUG_ERROR, "WdfDriverCreate failed with status 0x%x\n", status);

    WDF_OBJECT_ATTRIBUTES_INIT(&colAttributes);
    colAttributes.ParentObject = hDriver;

    status = WdfCollectionCreate(&colAttributes, &FilterDeviceCollection);
    if (!NT_SUCCESS(status)){
        FilterPrint(FILTER_DEBUG_ERROR, "WdfCollectionCreate failed with status 0x%x\n", status);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&colAttributes);
    colAttributes.ParentObject = hDriver;
    status = WdfWaitLockCreate(&colAttributes, &FilterDeviceCollectionLock);
    if (!NT_SUCCESS(status)){
        FilterPrint(FILTER_DEBUG_ERROR, "WdfWaitLockCreate failed with status 0x%x\n", status);
        return status;
    }
    status=FilterTraceFifoInit(hDriver);

    return status;
}

VOID FilterDriverUnload(IN WDFDRIVER  Driver){
    FilterPrint(FILTER_DEBUG_INFO, "DriverUnload.\n");
    FilterTraceFifoCleanUp();
}

NTSTATUS FilterDeviceAdd(IN WDFDRIVER Driver, IN PWDFDEVICE_INIT DeviceInit){
    WDF_OBJECT_ATTRIBUTES   deviceAttributes;
    PFILTER_CONTEXT         context;
    NTSTATUS                status;
    WDFDEVICE               device;
    ULONG                   returnSize;
    WDF_IO_QUEUE_CONFIG     ioQueueConfig;

    PAGED_CODE ();

    UNREFERENCED_PARAMETER(Driver);
    WdfFdoInitSetFilter(DeviceInit);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, FILTER_CONTEXT);
    deviceAttributes.EvtCleanupCallback = FilterDeviceContextCleanup;

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)){
        FilterPrint(FILTER_DEBUG_ERROR, "WdfDeviceCreate failed with status code 0x%x\n", status);
        return status;
    }

    context = FilterGetDeviceContext(device);
    context->MagicNumber = DEVICE_CONTEXT_MAGIC;
    context->HasDeviceId = FALSE;
    context->DeviceId = -1;
    context->FilterEnabled = FilterAutoTrace;
    context->TargetToSendRequestsTo = WdfDeviceGetIoTarget(device);
    
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&ioQueueConfig, WdfIoQueueDispatchParallel);

    ioQueueConfig.EvtIoRead = FilterIoRead;
    ioQueueConfig.EvtIoWrite = FilterIoWrite;
    ioQueueConfig.EvtIoInternalDeviceControl = FilterIoInternalDeviceControl;
	
	status = WdfIoQueueCreate(device, &ioQueueConfig, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(status)){
        FilterPrint(FILTER_DEBUG_ERROR, "WdfIoQueueCreate failed with status code 0x%x\n", status);
        status = STATUS_SUCCESS;
    }    

    WdfWaitLockAcquire(FilterDeviceCollectionLock, NULL);
    status = WdfCollectionAdd(FilterDeviceCollection, device);
    if (!NT_SUCCESS(status))
        FilterPrint(FILTER_DEBUG_ERROR, "WdfCollectionAdd failed with status code 0x%x\n", status);
    WdfWaitLockRelease(FilterDeviceCollectionLock);

    status = FilterCreateControlDevice(device);
    if (!NT_SUCCESS(status)){
        FilterPrint(FILTER_DEBUG_ERROR, "FilterCreateControlDevice failed with status 0x%x\n", status);
        status = STATUS_SUCCESS;
    }
    FilterUpdateDeviceIds();

    return status;
}

VOID FilterDeviceContextCleanup(IN WDFDEVICE Device){
    ULONG   count;

    PAGED_CODE();

    FilterPrint(FILTER_DEBUG_INFO, "Entered FilterDeviceContextCleanup\n");
    WdfWaitLockAcquire(FilterDeviceCollectionLock, NULL);

    count = WdfCollectionGetCount(FilterDeviceCollection);
    if (count == 1)
        FilterDeleteControlDevice(Device);

    WdfCollectionRemove(FilterDeviceCollection, Device);
    WdfWaitLockRelease(FilterDeviceCollectionLock);
}

NTSTATUS FilterCreateControlDevice(WDFDEVICE Device){
    PWDFDEVICE_INIT             pInit = NULL;
    WDFDEVICE                   controlDevice = NULL;
    WDF_OBJECT_ATTRIBUTES       controlAttributes;
    WDF_IO_QUEUE_CONFIG         ioQueueConfig;
    BOOLEAN                     bCreate = FALSE;
    NTSTATUS                    status;
    WDFQUEUE                    queue;
    DECLARE_CONST_UNICODE_STRING(ntDeviceName, NTDEVICE_NAME_STRING) ;
    DECLARE_CONST_UNICODE_STRING(symbolicLinkName, SYMBOLIC_NAME_STRING) ;

    PAGED_CODE();

    WdfWaitLockAcquire(FilterDeviceCollectionLock, NULL);
    if(WdfCollectionGetCount(FilterDeviceCollection) == 1)
		bCreate = TRUE;

    WdfWaitLockRelease(FilterDeviceCollectionLock);
    if(!bCreate)
        return STATUS_SUCCESS;

    FilterPrint(FILTER_DEBUG_INFO, "Creating Control Device\n");

    pInit = WdfControlDeviceInitAllocate(WdfDeviceGetDriver(Device), &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R);
    if (pInit == NULL){
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Error;
    }

    WdfDeviceInitSetExclusive(pInit, FALSE);
    status = WdfDeviceInitAssignName(pInit, &ntDeviceName);
    if (!NT_SUCCESS(status))
		goto Error;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&controlAttributes, CONTROL_DEVICE_EXTENSION);
    status = WdfDeviceCreate(&pInit, &controlAttributes, &controlDevice);
    if (!NT_SUCCESS(status))
		goto Error;

    status = WdfDeviceCreateSymbolicLink(controlDevice, &symbolicLinkName);
    if (!NT_SUCCESS(status))
        goto Error;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&ioQueueConfig, WdfIoQueueDispatchSequential);
    ioQueueConfig.EvtIoDeviceControl = FilterIoDeviceControl;
    status = WdfIoQueueCreate(controlDevice, &ioQueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status))
        goto Error;

    WDF_IO_QUEUE_CONFIG_INIT(&ioQueueConfig, WdfIoQueueDispatchManual);
    ioQueueConfig.PowerManaged = WdfFalse;
    status = WdfIoQueueCreate(controlDevice, &ioQueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &BufferRequestQueue);
    if (!NT_SUCCESS(status)){
        FilterPrint(FILTER_DEBUG_ERROR, "WdfIoQueueCreate failed 0x%x\n", status);
        goto Error;
    }

    WdfControlFinishInitializing(controlDevice);
    ControlDevice = controlDevice;
    return STATUS_SUCCESS;

Error:
    if(pInit != NULL)
        WdfDeviceInitFree(pInit);
    if(controlDevice != NULL){
        WdfObjectDelete(controlDevice);
        controlDevice = NULL;
    }
    return status;
}

VOID FilterDeleteControlDevice(WDFDEVICE Device){
    UNREFERENCED_PARAMETER(Device);

    PAGED_CODE();

    FilterPrint(FILTER_DEBUG_INFO, "Deleting Control Device\n");
    if(ControlDevice){
        WdfObjectDelete(ControlDevice);
        ControlDevice = NULL;
    }
}

VOID FilterIoDeviceControl(IN WDFQUEUE Queue, IN WDFREQUEST Request, IN size_t OutputBufferLength, IN size_t InputBufferLength, IN ULONG IoControlCode){
    NTSTATUS               status = STATUS_SUCCESS;
    ULONG                  i;
    ULONG                  noItems;
    WDFDEVICE              device;
    PFILTER_CONTEXT        context;
    PVOID                  outputBuffer = NULL;
    PFILTER_FILTER_ENABLED filterEnabledBuffer;
    PFILTER_DEBUG_LEVEL    debugLevelBuffer;
    PFILTER_AUTOTRACE      autotraceBuffer;
    size_t                 realLength;
    size_t                 bytesWritten;
    size_t                 bytesNeeded;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    PAGED_CODE();

    FilterPrint(FILTER_DEBUG_INFO, "Ioctl recieved into filter control object.\n");
    switch(IoControlCode){
        case IOCTL_FILTER_GET_BUFFER:
            if(InputBufferLength){
                FilterPrint(FILTER_DEBUG_WARN, "Sorry buddy...No input buffers allowed\n");
                WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
                return;

            }
            FilterPrint(FILTER_DEBUG_INFO, "Get buffer\n");

            status = FilterFufillRequestWithTraces(Request, &bytesWritten);
            if(!NT_SUCCESS(status)){
                if (status == STATUS_NO_DATA_DETECTED){
                    
                    status = WdfRequestForwardToIoQueue(Request, BufferRequestQueue);
                    if (NT_SUCCESS(status))
                        return;
                    else
                        FilterPrint(FILTER_DEBUG_ERROR, "WdfRequestForwardToIoQueue failed Status 0x%x\n", status);
                }
                else
                    FilterPrint(FILTER_DEBUG_ERROR, "FilterFufillRequestWithTraces failed Status 0x%x\n", status);
            }

            if(NT_SUCCESS(status))
                WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, bytesWritten);
            else
                WdfRequestComplete(Request, status);
            return;

        case IOCTL_FILTER_START_FILTERING:
            FilterPrint(FILTER_DEBUG_INFO, "Filtering enabled\n");
            FilterFiltering = TRUE;
            FilterTraceFifoCleanUp();
            WdfRequestComplete(Request, STATUS_SUCCESS);
            return;

        case IOCTL_FILTER_STOP_FILTERING:
            FilterPrint(FILTER_DEBUG_INFO, "Filtering disabled\n");
            FilterFiltering = FALSE;
            FilterTraceFifoCleanUp();
            WdfRequestComplete(Request, STATUS_SUCCESS);
            return;

        case IOCTL_FILTER_PRINT_DEVICES: 
            FilterPrint(FILTER_DEBUG_WARN, "IOCTL_FILTER_PRINT_DEVICES is depreciated\n");
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;

        case IOCTL_FILTER_SET_DEVICE_FILTER_ENABLED: {
            BOOLEAN foundDevice = FALSE;

            if (!InputBufferLength){
                WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
                return;
            }

            status = WdfRequestRetrieveInputBuffer(Request, sizeof(FILTER_FILTER_ENABLED), (PVOID *)&filterEnabledBuffer, &realLength);
            if(!NT_SUCCESS(status)){
                FilterPrint(FILTER_DEBUG_ERROR, "WdfRequestRetrieveOutputBuffer failed - 0x%x\n", status);
                WdfRequestComplete(Request, status);
                return;
            }

            WdfWaitLockAcquire(FilterDeviceCollectionLock, NULL);
            noItems = WdfCollectionGetCount(FilterDeviceCollection);
            for (i = 0; i < noItems; i++){
                device = WdfCollectionGetItem(FilterDeviceCollection, i);
                context = FilterGetDeviceContext(device);

                if (filterEnabledBuffer->DeviceId == context->DeviceId){
                    context->FilterEnabled = filterEnabledBuffer->FilterEnabled;
                    FilterPrint(FILTER_DEBUG_INFO, "%d - FilterEnabled: %d\n", filterEnabledBuffer->DeviceId, context->FilterEnabled);
                    foundDevice = TRUE;
                    break;
                }
            }

            if(!foundDevice){
                FilterPrint(FILTER_DEBUG_WARN, "Error DeviceId (%d) is not valid\n", filterEnabledBuffer->DeviceId);
                status = STATUS_NO_SUCH_DEVICE;
            }
            WdfWaitLockRelease(FilterDeviceCollectionLock);
            WdfRequestCompleteWithInformation(Request, status, realLength);
            return;
        }

        case IOCTL_FILTER_GET_DEVICE_LIST:
            if (InputBufferLength){
                FilterPrint(FILTER_DEBUG_WARN, "Sorry buddy...No input buffers allowed\n");
                WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
                return;
            }

            FilterPrint(FILTER_DEBUG_INFO, "get device list\n");
            status = WdfRequestRetrieveOutputBuffer(Request, sizeof(FILTER_FILTER_TRACE), &outputBuffer, &realLength);
            if (!NT_SUCCESS(status)){
                FilterPrint(FILTER_DEBUG_ERROR, "WdfRequestRetrieveOutputBuffer failed - 0x%x\n", status);
                WdfRequestComplete(Request, status);
                return;
            }

            if (FilterFillBufferWithDeviceIds(outputBuffer,  realLength, &bytesWritten, &bytesNeeded))
                WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, bytesWritten);
            else
                WdfRequestCompleteWithInformation(Request, STATUS_BUFFER_TOO_SMALL, bytesNeeded);
            return;

        case IOCTL_FILTER_GET_DEBUG_LEVEL:
            if (InputBufferLength){
                FilterPrint(FILTER_DEBUG_WARN, "Sorry buddy...No input buffers allowed\n");
                WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
                return;
            }
            FilterPrint(FILTER_DEBUG_INFO, "get debug level\n");

            status = WdfRequestRetrieveOutputBuffer(Request, sizeof(FILTER_DEBUG_LEVEL), &outputBuffer, &realLength);
            if (!NT_SUCCESS(status)){
                FilterPrint(FILTER_DEBUG_ERROR, "WdfRequestRetrieveOutputBuffer failed - 0x%x\n", status);
                WdfRequestComplete(Request, status);
                return;
            }
           
            ((PFILTER_DEBUG_LEVEL)outputBuffer)->DebugLevel = FilterDebugLevel;
            WdfRequestCompleteWithInformation(Request,  STATUS_SUCCESS, realLength);
            return;

        case IOCTL_FILTER_SET_DEBUG_LEVEL:
            if (!InputBufferLength){
                WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
                return;
            }

            status = WdfRequestRetrieveInputBuffer(Request, sizeof(FILTER_DEBUG_LEVEL), (PVOID *)&debugLevelBuffer, &realLength);
            if (!NT_SUCCESS(status)){
                FilterPrint(FILTER_DEBUG_ERROR, "WdfRequestRetrieveOutputBuffer failed - 0x%x\n", status);
                WdfRequestComplete(Request, status);
                return;
            }
            FilterDebugLevel = debugLevelBuffer->DebugLevel;
            WdfRequestCompleteWithInformation(Request, status, realLength);
            return;

        case IOCTL_FILTER_GET_AUTOTRACE:
            if (InputBufferLength){
                FilterPrint(FILTER_DEBUG_WARN, "Sorry buddy...No input buffers allowed\n");
                WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
                return;
            }
            FilterPrint(FILTER_DEBUG_INFO, "get autotrace value\n");

            status = WdfRequestRetrieveOutputBuffer(Request, sizeof(FILTER_AUTOTRACE), &outputBuffer, &realLength);
            if (!NT_SUCCESS(status)){
                FilterPrint(FILTER_DEBUG_ERROR, "WdfRequestRetrieveOutputBuffer failed - 0x%x\n", status);
                WdfRequestComplete(Request, status);
                return;
            }

            ((PFILTER_AUTOTRACE)outputBuffer)->AutoTrace = FilterAutoTrace;
            WdfRequestCompleteWithInformation(Request,  STATUS_SUCCESS, realLength);
            return;
        case IOCTL_FILTER_SET_AUTOTRACE:
            if (!InputBufferLength){
                WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
                return;
            }

            status = WdfRequestRetrieveInputBuffer(Request, sizeof(FILTER_AUTOTRACE), (PVOID *)&autotraceBuffer, &realLength);
            if (!NT_SUCCESS(status)){
                FilterPrint(FILTER_DEBUG_ERROR, "WdfRequestRetrieveOutputBuffer failed - 0x%x\n", status);
                WdfRequestComplete(Request, status);
                return;
            }
            FilterAutoTrace = autotraceBuffer->AutoTrace;
            WdfRequestCompleteWithInformation(Request, status, realLength);
            return;

        default:
            break;
    }
    WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
}

VOID FilterIoRead(IN WDFQUEUE Queue, IN WDFREQUEST Request, IN size_t Length){
    NTSTATUS              status;
    PCHAR                 dataBuffer = NULL;
    WDFDEVICE             device = WdfIoQueueGetDevice(Queue);
    PFILTER_CONTEXT       context = FilterGetDeviceContext(device);

    if (FilterFiltering && context->FilterEnabled){
        if (Length != 0){
            status = WdfRequestRetrieveOutputBuffer(Request, Length, (PVOID *)&dataBuffer, NULL);
            if (NT_SUCCESS(status)){
                FILTER_REQUEST_PARAMS params;
                FILTER_REQUEST_PARAMS_INIT(&params);

                FilterPrint(FILTER_DEBUG_INFO, "FilterIoRead       %2d: Length-0x%x Data-", context->DeviceId, Length);
                PrintChars(dataBuffer, Length);

                FilterAddTraceToFifo(device,  context->DeviceId, FilterReadRequest, params, dataBuffer, Length);
            }
            else
                FilterPrint(FILTER_DEBUG_INFO, "RetrieveOutputBuffer failed - 0x%x\n", status);
        }
    }

    FilterForwardRequestWithCompletion(WdfIoQueueGetDevice(Queue), Request, FilterReadComplete, NULL);
    return;
}

VOID FilterReadComplete(IN WDFREQUEST Request, IN WDFIOTARGET Target, IN PWDF_REQUEST_COMPLETION_PARAMS Params, IN WDFCONTEXT Context){
    PFILTER_CONTEXT context = FilterGetDeviceContext(WdfIoQueueGetDevice(WdfRequestGetIoQueue(Request)));

    if(FilterFiltering && context->FilterEnabled)
        FilterPrint(FILTER_DEBUG_INFO, "FilterReadComplete %2d: Status-0x%x; Information-0x%x\n", context->DeviceId, Params->IoStatus.Status, Params->IoStatus.Information);
    WdfRequestComplete(Request, Params->IoStatus.Status);
}

VOID FilterIoWrite(IN WDFQUEUE Queue, IN WDFREQUEST Request, IN size_t Length){
    NTSTATUS        status;
    PUCHAR          dataBuffer = NULL;
    WDFDEVICE       device = WdfIoQueueGetDevice(Queue);
    PFILTER_CONTEXT context = FilterGetDeviceContext(device);

    if(FilterFiltering && context->FilterEnbled){
        if(Length != 0){
            status = WdfRequestRetrieveInputBuffer(Request, Length, (PVOID *)&dataBuffer, NULL);
            if (NT_SUCCESS(status)){
                FILTER_REQUEST_PARAMS params;
                FILTER_REQUEST_PARAMS_INIT(&params);

                FilterPrint(FILTER_DEBUG_INFO, "FilterIoWrite      %2d: Length-0x%x Data-", context->DeviceId, Length);
                PrintChars(dataBuffer, Length);
                FilterAddTraceToFifo(device, context->DeviceId, FilterWriteRequest, params, dataBuffer, Length);
            }
            else
				FilterPrint(FILTER_DEBUG_ERROR, "RetrieveInputBuffer failed - 0x%x\n", status);
        }
    }
    FilterForwardRequest(WdfIoQueueGetDevice(Queue),  Request);
    return;
}

VOID FilterForwardRequest(IN WDFDEVICE Device, IN WDFREQUEST Request){
    WDF_REQUEST_SEND_OPTIONS options;
    PFILTER_CONTEXT          context;
    NTSTATUS                 status;

    context = FilterGetDeviceContext(Device);
    ASSERT(IS_DEVICE_CONTEXT(context));

    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

	if (!WdfRequestSend(Request, context->TargetToSendRequestsTo, &options)){
        status = WdfRequestGetStatus(Request);
        FilterPrint(FILTER_DEBUG_ERROR, "WdfRequestSend failed - 0x%x\n", status);
        WdfRequestComplete(Request, status);
    }
    return;
}

VOID FilterForwardRequestWithCompletion(IN WDFDEVICE Device, IN WDFREQUEST Request, IN PFN_WDF_REQUEST_COMPLETION_ROUTINE CompletionRoutine, IN WDFCONTEXT CompletionContext){
    PFILTER_CONTEXT context;
    NTSTATUS        status;

    context = FilterGetDeviceContext(Device);

    ASSERT(IS_DEVICE_CONTEXT(context));
    WdfRequestFormatRequestUsingCurrentType(Request);
    WdfRequestSetCompletionRoutine(Request, CompletionRoutine, CompletionContext);

    if (!WdfRequestSend(Request, context->TargetToSendRequestsTo, NULL)) {
        status = WdfRequestGetStatus(Request);
        FilterPrint(FILTER_DEBUG_ERROR, "WdfRequestSend failed - 0x%x\n", status);
        WdfRequestComplete(Request, status);
    }
    return;
}

VOID FilterIoInternalDeviceControl(IN WDFQUEUE  Queue, IN WDFREQUEST  Request, IN size_t  OutputBufferLength, IN size_t  InputBufferLength, IN ULONG  IoControlCode){
    NTSTATUS              status = STATUS_SUCCESS;
    WDFDEVICE             device = WdfIoQueueGetDevice(Queue);
    PFILTER_CONTEXT       context = FilterGetDeviceContext(device);
    BOOLEAN               bRead = FALSE;

    FilterProcessInternalDeviceControl(device, context, Request, IoControlCode, FALSE, &bRead);
    if(bRead)
        FilterForwardRequestWithCompletion(WdfIoQueueGetDevice(Queue), Request, FilterIoInternalDeviceControlComplete,(WDFCONTEXT)IoControlCode);
    else
        FilterForwardRequest(WdfIoQueueGetDevice(Queue), Request);
}

VOID FilterIoInternalDeviceControlComplete(IN WDFREQUEST Request, IN WDFIOTARGET Target, IN PWDF_REQUEST_COMPLETION_PARAMS Params, IN WDFCONTEXT Context){
    WDFDEVICE             device = WdfIoQueueGetDevice(WdfRequestGetIoQueue(Request));
    PFILTER_CONTEXT       context = FilterGetDeviceContext(device);
    BOOLEAN               bRead = TRUE;
    
    FilterProcessInternalDeviceControl(device, context, Request, (ULONG)Context, TRUE, &bRead);
    WdfRequestComplete(Request, Params->IoStatus.Status);
}

VOID __FilterProcessUrbDescriptorRequest(IN WDFDEVICE Device, IN PFILTER_CONTEXT Context, IN PURB pUrb, IN BOOLEAN bCompletion, IN BOOLEAN bRead){
    struct _URB_CONTROL_DESCRIPTOR_REQUEST* pDescReq = (struct _URB_CONTROL_DESCRIPTOR_REQUEST*)pUrb;

    PVOID pTransferBuffer = pDescReq->TransferBuffer;
    PMDL pTransferBufferMDL = pDescReq->TransferBufferMDL;
    ULONG TransferBufferLength = pDescReq->TransferBufferLength;

    if (bCompletion && bRead || !bCompletion && !bRead){
        FILTER_REQUEST_PARAMS params;
        FILTER_REQUEST_PARAMS_INIT(&params);

        FilterPrint(FILTER_DEBUG_INFO, "        TransferBufferLength: %d\n", TransferBufferLength);
        FilterPrint(FILTER_DEBUG_INFO, "        MDL: %d\n", pTransferBufferMDL != NULL);

        params.p1 = FilterUSBSubmitURB;
        params.p2 = (pDescReq->Index << 8) | pDescReq->DescriptorType;
        params.p3 = pUrb->UrbHeader.Function;
        params.p4 = pDescReq->LanguageId;

        if (pTransferBuffer != NULL){
            PrintChars((PCHAR)pTransferBuffer, TransferBufferLength);
            FilterAddTraceToFifo(Device, Context->DeviceId, FilterInternalDeviceControlRequest, params, pTransferBuffer, TransferBufferLength);
        }
        else if (pTransferBufferMDL != NULL){
            PCHAR pMDLBuf = (PCHAR)MmGetSystemAddressForMdlSafe(pTransferBufferMDL, NormalPagePriority);
            PrintChars(pMDLBuf, TransferBufferLength);
            FilterAddTraceToFifo(Device, Context->DeviceId, FilterInternalDeviceControlRequest, params, pMDLBuf, TransferBufferLength);
        }
        else
            FilterPrint(FILTER_DEBUG_ERROR, "Buffer error!\n");
    }
}

VOID __FilterProcessUrbTransfer(IN WDFDEVICE Device, IN PFILTER_CONTEXT Context, IN PURB pUrb, IN PVOID pTransferBuffer, IN PMDL pTransferBufferMDL, IN ULONG TransferBufferLength, IN BOOLEAN bCompletion, IN BOOLEAN bRead){
    FilterPrint(FILTER_DEBUG_INFO, "        TransferBufferLength: %d\n", TransferBufferLength);
    FilterPrint(FILTER_DEBUG_INFO, "        In/Out: %s, MDL: %d\n", bRead ? "in" : "out", pTransferBufferMDL != NULL);

    if (bCompletion && bRead || !bCompletion && !bRead){
        FILTER_REQUEST_PARAMS params;
        FILTER_REQUEST_PARAMS_INIT(&params);

        params.p1 = FilterUSBSubmitURB;
        params.p2 = bRead ? FilterUsbIn : FilterUsbOut;
        params.p3 = pUrb->UrbHeader.Function;

        FilterPrint(FILTER_DEBUG_INFO, "        Data: ");
        if (pTransferBuffer != NULL){
            PrintChars((PCHAR)pTransferBuffer, TransferBufferLength);
            FilterAddTraceToFifo(Device, Context->DeviceId, FilterInternalDeviceControlRequest, params, pTransferBuffer, TransferBufferLength);
        }
        else if (pTransferBufferMDL != NULL){
            PCHAR pMDLBuf = (PCHAR)MmGetSystemAddressForMdlSafe(pTransferBufferMDL, NormalPagePriority);

            PrintChars(pMDLBuf, TransferBufferLength);
            FilterAddTraceToFifo(Device, Context->DeviceId, FilterInternalDeviceControlRequest, params, pMDLBuf, TransferBufferLength);
        }
        else
            FilterPrint(FILTER_DEBUG_ERROR, "Buffer error!\n");
    }
}

VOID FilterProcessInternalDeviceControl(IN WDFDEVICE Device, IN PFILTER_CONTEXT Context, IN WDFREQUEST  Request, IN ULONG  IoControlCode, IN BOOLEAN bCompletion, OUT BOOLEAN* bRead){
    *bRead = FALSE;

    if (FilterFiltering && Context->FilterEnabled){
        if (bCompletion)
            FilterPrint(FILTER_DEBUG_INFO, "FilterIoInternalDeviceControlComplete - Id: %d, IOCTL: %d\n",  Context->DeviceId, IoControlCode);
        else
            FilterPrint(FILTER_DEBUG_INFO, "FilterIoInternalDeviceControl - Id: %d, IOCTL: %d\n",  Context->DeviceId, IoControlCode);

        switch (IoControlCode){
            case IOCTL_INTERNAL_USB_SUBMIT_URB: {
                PURB pUrb;

                FilterPrint(FILTER_DEBUG_INFO, "    IOCTL_INTERNAL_USB_SUBMIT_URB\n");
                pUrb = (PURB) IoGetCurrentIrpStackLocation(WdfRequestWdmGetIrp(Request))->Parameters.Others.Argument1;

                switch (pUrb->UrbHeader.Function){
                    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER: {
                        struct _URB_BULK_OR_INTERRUPT_TRANSFER* pTransfer = (struct _URB_BULK_OR_INTERRUPT_TRANSFER*)pUrb;
                        *bRead = (BOOLEAN)(pTransfer->TransferFlags & USBD_TRANSFER_DIRECTION_IN);
                        FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER\n");

                        __FilterProcessUrbTransfer(Device, Context, pUrb, pTransfer->TransferBuffer, pTransfer->TransferBufferMDL, pTransfer->TransferBufferLength, bCompletion, *bRead);        
                        break;
                    }
                    case URB_FUNCTION_CONTROL_TRANSFER: {
                        struct _URB_CONTROL_TRANSFER* pTransfer =  (struct _URB_CONTROL_TRANSFER*)pUrb;
                        *bRead = (BOOLEAN)(pTransfer->TransferFlags & USBD_TRANSFER_DIRECTION_IN);

                        FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_CONTROL_TRANSFER\n");

                        __FilterProcessUrbTransfer(Device, Context, pUrb, pTransfer->TransferBuffer, pTransfer->TransferBufferMDL, pTransfer->TransferBufferLength, bCompletion, *bRead);        
                        break;
                    }
                    case URB_FUNCTION_VENDOR_DEVICE:
                    case URB_FUNCTION_VENDOR_INTERFACE:
                    case URB_FUNCTION_VENDOR_ENDPOINT:
                    case URB_FUNCTION_VENDOR_OTHER:
                    case URB_FUNCTION_CLASS_DEVICE:
                    case URB_FUNCTION_CLASS_INTERFACE:
                    case URB_FUNCTION_CLASS_ENDPOINT:
                    case URB_FUNCTION_CLASS_OTHER: {
                        struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST* pTransfer =  (struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST*)pUrb;
                        *bRead = (BOOLEAN)(pTransfer->TransferFlags & USBD_TRANSFER_DIRECTION_IN);

                        switch (pUrb->UrbHeader.Function){
                            case URB_FUNCTION_VENDOR_DEVICE:
                                FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_VENDOR_DEVICE\n");
                                break;
                            case URB_FUNCTION_VENDOR_INTERFACE:
                                FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_VENDOR_INTERFACE\n");
                                break;
                            case URB_FUNCTION_VENDOR_ENDPOINT:
                                FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_VENDOR_ENDPOINT\n");
                                break;
                            case URB_FUNCTION_VENDOR_OTHER:
                                FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_VENDOR_OTHER\n");
                                break;
                            case URB_FUNCTION_CLASS_DEVICE:
                                FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_CLASS_DEVICE\n");
                                break;
                            case URB_FUNCTION_CLASS_INTERFACE:
                                FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_CLASS_INTERFACE\n");
                                break;
                            case URB_FUNCTION_CLASS_ENDPOINT:
                                FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_CLASS_ENDPOINT\n");
                                break;
                            case URB_FUNCTION_CLASS_OTHER:
                                FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_CLASS_OTHER\n");
                                break;
                        }

                        __FilterProcessUrbTransfer(Device, Context, pUrb, pTransfer->TransferBuffer, pTransfer->TransferBufferMDL, pTransfer->TransferBufferLength, bCompletion, *bRead);        
                        break;
                    }
                    case URB_FUNCTION_ISOCH_TRANSFER: {
                        struct _URB_ISOCH_TRANSFER* pTransfer =  (struct _URB_ISOCH_TRANSFER*)pUrb;
                        *bRead = (BOOLEAN)(pTransfer->TransferFlags & USBD_TRANSFER_DIRECTION_IN);

                        FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_ISOCH_TRANSFER\n");
                        __FilterProcessUrbTransfer(Device, Context, pUrb, pTransfer->TransferBuffer, pTransfer->TransferBufferMDL, pTransfer->TransferBufferLength, bCompletion, *bRead);        
                        break;
                    }
                    case URB_FUNCTION_ABORT_PIPE:
                    case URB_FUNCTION_RESET_PIPE:
                    case URB_FUNCTION_SYNC_RESET_PIPE:
                    case URB_FUNCTION_SYNC_CLEAR_STALL: {
                        FILTER_REQUEST_PARAMS params;
                        FILTER_REQUEST_PARAMS_INIT(&params);

                        switch (pUrb->UrbHeader.Function) {
                            case URB_FUNCTION_ABORT_PIPE:
                                FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_ABORT_PIPE\n");
                                break;
                            case URB_FUNCTION_RESET_PIPE:
                                FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_RESET_PIPE\n");
                                break;
                            case URB_FUNCTION_SYNC_RESET_PIPE:
                                FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_SYNC_RESET_PIPE\n");
                                break;
                            case URB_FUNCTION_SYNC_CLEAR_STALL:
                                FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_SYNC_CLEAR_STALL\n");
                                break;
                        }

                        params.p1 = FilterUSBSubmitURB;
                        params.p3 = pUrb->UrbHeader.Function;

                        FilterAddTraceToFifo(Device, Context->DeviceId,  FilterInternalDeviceControlRequest, params, NULL, 0);
                        break;
                    }

                    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
                    case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
                    case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
                    case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
                    case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT:
                    case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE: {
                        switch (pUrb->UrbHeader.Function) {
                            case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
                                FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE\n");
                                *bRead = TRUE;
                                break;
                            case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
                                FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT\n");
                                *bRead = TRUE;
                                break;
                            case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
                                FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE\n");
                                *bRead = TRUE;
                                break;
                            case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
                                FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE\n");
                                break;
                            case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT:
                                FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT\n");
                                break;
                            case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE:
                                FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE\n");
                                break;
                        }

                        __FilterProcessUrbDescriptorRequest(Device, Context, pUrb, bCompletion, *bRead);        
                        break;
                    }

                    default:
                        FilterPrint(FILTER_DEBUG_INFO, "        URB_FUNCTION: %d\n", pUrb->UrbHeader.Function);
                        break;
                }
                break;
            }

            case IOCTL_INTERNAL_USB_RESET_PORT: {
                FILTER_REQUEST_PARAMS params;
                FILTER_REQUEST_PARAMS_INIT(&params);

                params.p1 = FilterUSBResetPort;
                FilterPrint(FILTER_DEBUG_INFO, "    IOCTL_INTERNAL_USB_RESET_PORT\n");

                FilterAddTraceToFifo(Device, Context->DeviceId,  FilterInternalDeviceControlRequest, params, NULL, 0);
                break;
            }
        }

    }

}
