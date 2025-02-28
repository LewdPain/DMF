/*++

    Copyright (c) Microsoft Corporation. All rights reserved.
    Licensed under the MIT license.

Module Name:

    Dmf_DeviceInterfaceMultipleTarget.c

Abstract:

    Creates a stream of asynchronous requests to a dynamic PnP IO Target. Also, there is support
    for sending synchronous requests to the same IO Target. The Module supports multiple instances
    of the same device interface target.

Environment:

    Kernel-mode Driver Framework
    User-mode Driver Framework

--*/

// DMF and this Module's Library specific definitions.
//
#include "DmfModule.h"
#include "DmfModules.Library.h"
#include "DmfModules.Library.Trace.h"

#if defined(DMF_INCLUDE_TMH)
#include "Dmf_DeviceInterfaceMultipleTarget.tmh"
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////////
// Module Private Enumerations and Structures
///////////////////////////////////////////////////////////////////////////////////////////////////////
//

#if defined(DMF_USER_MODE)
// Specific external includes for this DMF Module.
//
#include <Guiddef.h>
#include <cfgmgr32.h>
#include <ndisguid.h>
#endif // defined(DMF_USER_MODE)

typedef struct
{
    // Underlying Device Target.
    //
    WDFIOTARGET IoTarget;
    // Support proper rundown per target.
    //
    DMFMODULE DmfModuleRundown;
    // Save Symbolic Link Name to be able to deal with multiple instances of the same
    // device interface.
    //
    WDFMEMORY MemorySymbolicLink;
    UNICODE_STRING SymbolicLinkName;
    DMFMODULE DmfModuleRequestTarget;
    DeviceInterfaceMultipleTarget_Target DmfIoTarget;
    // Surprise removal path does not send a QueryRemove, only a RemoveComplete
    // notification. This flag tracks that so that RemoveComplete path properly
    // stops target and Closes Module during surprise-removal path.
    //
    BOOLEAN QueryRemoveHappened;
    // This flag tells the rest of ensures the target rundown code executes exactly one time.
    // Using this flag allows the IoTarget handle to remain set while rundown is happening,
    // but *after* the target has closed (so that all pending buffers will be canceled).
    //
    BOOLEAN TargetClosedOrClosing;
} DeviceInterfaceMultipleTarget_IoTarget;

typedef struct
{
    // Details of the Target. 
    //
    DeviceInterfaceMultipleTarget_IoTarget* Target;
    // This Module's Handle
    //
    DMFMODULE DmfModuleDeviceInterfaceMultipleTarget;
} DeviceInterfaceMultipleTarget_IoTargetContext;

WDF_DECLARE_CONTEXT_TYPE(DeviceInterfaceMultipleTarget_IoTargetContext);

typedef struct
{
    // If set to TRUE, Buffer will be removed from buffer pool if found during enumeration.
    //
    BOOLEAN RemoveBuffer;
    // Data used in the enumeration callback functions.
    //
    VOID* ContextData;
    // Set to TRUE in enumeration callback if buffer is found.
    //
    BOOLEAN BufferFound;
} DeviceInterfaceMultipleTarget_EnumerationContext;

// These are virtual Methods that are set based on the transport.
// These functions are common to both the Stream and Target transport.
// They are set to the correct version when the Module is created.
// NOTE: The DmfModule that is sent is the DeviceInterfaceMultipleTarget Module.
//

typedef
BOOLEAN
RequestSink_Cancel_Type(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target,
    _In_ RequestTarget_DmfRequest DmfRequestId
    );

typedef
_Must_inspect_result_
NTSTATUS
RequestSink_SendSynchronously_Type(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target,
    _In_reads_bytes_opt_(RequestLength) VOID* RequestBuffer,
    _In_ size_t RequestLength,
    _Out_writes_bytes_opt_(ResponseLength) VOID* ResponseBuffer,
    _In_ size_t ResponseLength,
    _In_ ContinuousRequestTarget_RequestType RequestType,
    _In_ ULONG RequestIoctl,
    _In_ ULONG RequestTimeout,
    _Out_opt_ size_t* BytesWritten
    );

typedef
_Must_inspect_result_
NTSTATUS
RequestSink_Send_Type(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target,
    _In_reads_bytes_opt_(RequestLength) VOID* RequestBuffer,
    _In_ size_t RequestLength,
    _Out_writes_bytes_opt_(ResponseLength) VOID* ResponseBuffer,
    _In_ size_t ResponseLength,
    _In_ ContinuousRequestTarget_RequestType RequestType,
    _In_ ULONG RequestIoctl,
    _In_ ULONG RequestTimeoutMilliseconds,
    _In_opt_ EVT_DMF_ContinuousRequestTarget_SendCompletion* EvtRequestSinkSingleAsynchronousRequest,
    _In_opt_ VOID* SingleAsynchronousRequestClientContext
    );

typedef
_Must_inspect_result_
NTSTATUS
RequestSink_SendEx_Type(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target,
    _In_reads_bytes_opt_(RequestLength) VOID* RequestBuffer,
    _In_ size_t RequestLength,
    _Out_writes_bytes_opt_(ResponseLength) VOID* ResponseBuffer,
    _In_ size_t ResponseLength,
    _In_ ContinuousRequestTarget_RequestType RequestType,
    _In_ ULONG RequestIoctl,
    _In_ ULONG RequestTimeoutMilliseconds,
    _In_opt_ EVT_DMF_ContinuousRequestTarget_SendCompletion* EvtRequestSinkSingleAsynchronousRequest,
    _In_opt_ VOID* SingleAsynchronousRequestClientContext,
    _Out_opt_ RequestTarget_DmfRequest* DmfRequestId
    );

typedef
VOID
RequestSink_IoTargetSet_Type(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target,
    _In_ WDFIOTARGET IoTarget
    );

typedef
VOID
RequestSink_IoTargetClear_Type(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target
    );

///////////////////////////////////////////////////////////////////////////////////////////////////////
// Module Private Context
///////////////////////////////////////////////////////////////////////////////////////////////////////
//

typedef struct _DMF_CONTEXT_DeviceInterfaceMultipleTarget
{
    // Device Interface arrival/removal notification handle.
    // 
#if defined(DMF_USER_MODE)
    HCMNOTIFICATION DeviceInterfaceNotification;
#else
    VOID* DeviceInterfaceNotification;
#endif // defined(DMF_USER_MODE)

    DMFMODULE DmfModuleBufferQueue;
    // Ensures that Module Open/Close are called a single time.
    //
    LONG NumberOfTargetsOpened;

    // Redirect Input buffer callback from ContinuousRequestTarget to this callback.
    //
    EVT_DMF_ContinuousRequestTarget_BufferInput* EvtContinuousRequestTargetBufferInput;
    // Redirect Output buffer callback from ContinuousRequestTarget to this callback.
    //
    EVT_DMF_ContinuousRequestTarget_BufferOutput* EvtContinuousRequestTargetBufferOutput;

    // This Module has two modes:
    // 1. Streaming is enabled and DmfModuleRequestTarget is valid.
    // 2. Streaming is not enabled and DmfModuleRequestTarget is used.
    //
    // In order to not check for NULL Handles, this flag is used when a choice must be made.
    // This flag is also used for assertions in case people misuse APIs.
    //
    BOOLEAN ContinuousReaderMode;

    // Indicates the mode of ContinuousRequestTarget.
    //
    ContinuousRequestTarget_ModeType ContinuousRequestTargetMode;

    // Underlying Transport Methods.
    //
    RequestSink_SendSynchronously_Type* RequestSink_SendSynchronously;
    RequestSink_Send_Type* RequestSink_Send;
    RequestSink_SendEx_Type* RequestSink_SendEx;
    RequestSink_Cancel_Type* RequestSink_Cancel;
    RequestSink_IoTargetSet_Type* RequestSink_IoTargetSet;
    RequestSink_IoTargetClear_Type* RequestSink_IoTargetClear;

    // Passive level desired by Client. This is used to instantiate underlying Child Modules.
    //
    BOOLEAN PassiveLevel;
} DMF_CONTEXT_DeviceInterfaceMultipleTarget;

// This macro declares the following function:
// DMF_CONTEXT_GET()
//
DMF_MODULE_DECLARE_CONTEXT(DeviceInterfaceMultipleTarget)

// This macro declares the following function:
// DMF_CONFIG_GET()
//
DMF_MODULE_DECLARE_CONFIG(DeviceInterfaceMultipleTarget)

#define MemoryTag 'MTID'

///////////////////////////////////////////////////////////////////////////////////////////////////////
// DMF Module Support Code
///////////////////////////////////////////////////////////////////////////////////////////////////////
//

VOID
DeviceInterfaceMultipleTarget_SymbolicLinkNameClear(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target
    )
/*++

Routine Description:

    Delete the stored symbolic link from the context. This is needed to deal with multiple instances of 
    the same device interface.

Arguments:

    DmfModule - This Module's DMF Module handle.
    Target - 

Return Value:

    None

--*/
{
    UNREFERENCED_PARAMETER(DmfModule);

    if (Target->MemorySymbolicLink != NULL)
    {
        WdfObjectDelete(Target->MemorySymbolicLink);
        Target->MemorySymbolicLink = NULL;
        Target->SymbolicLinkName.Buffer = NULL;
        Target->SymbolicLinkName.Length = 0;
        Target->SymbolicLinkName.MaximumLength = 0;
    }
}

_Must_inspect_result_
NTSTATUS
DeviceInterfaceMultipleTarget_SymbolicLinkNameStore(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target,
    _In_ UNICODE_STRING* SymbolicLinkName
    )
/*++

Routine Description:

    Create a copy of symbolic link name and store it in the given Module's context. This is needed to deal with
    multiple instances of the same device interface.

Arguments:

    DmfModule - This Module's DMF Module handle.
    Target - 
    SymbolicLinkName - The given symbolic link name.

Return Value:

    None

--*/
{
    USHORT symbolicLinkStringLength;
    WDF_OBJECT_ATTRIBUTES objectAttributes;
    NTSTATUS ntStatus;

    symbolicLinkStringLength = SymbolicLinkName->Length;
    if (0 == symbolicLinkStringLength)
    {
        DmfAssert(FALSE);
        ntStatus = STATUS_UNSUCCESSFUL;
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "Symbolic link name length is 0");
        goto Exit;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&objectAttributes);
    objectAttributes.ParentObject = DmfModule;

    ntStatus = WdfMemoryCreate(&objectAttributes,
                               NonPagedPoolNx,
                               MemoryTag,
                               symbolicLinkStringLength + sizeof(UNICODE_NULL),
                               &Target->MemorySymbolicLink,
                               (VOID**)&Target->SymbolicLinkName.Buffer);
    if (!NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR,  DMF_TRACE, "WdfMemoryCreate fails: ntStatus=%!STATUS!", ntStatus);
        goto Exit;
    }
    DmfAssert(NULL != Target->SymbolicLinkName.Buffer);

    Target->SymbolicLinkName.Length = symbolicLinkStringLength;
    Target->SymbolicLinkName.MaximumLength = symbolicLinkStringLength + sizeof(UNICODE_NULL);

#if defined(DMF_USER_MODE)
    // Overwrite with string.
    //
    RtlZeroMemory(Target->SymbolicLinkName.Buffer,
                  Target->SymbolicLinkName.MaximumLength);
    RtlCopyMemory(Target->SymbolicLinkName.Buffer,
                  SymbolicLinkName->Buffer,
                  symbolicLinkStringLength);
#else
    ntStatus = RtlUnicodeStringCopy(&Target->SymbolicLinkName,
                                    SymbolicLinkName);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "RtlUnicodeStringCopy fails: ntStatus=%!STATUS!", ntStatus);
        DeviceInterfaceMultipleTarget_SymbolicLinkNameClear(DmfModule,
                                                            Target);
        goto Exit;
    }
#endif

Exit:
    
    return ntStatus;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
static
VOID
DeviceInterfaceMultipleTarget_TargetDestroy(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target
    )
/*++

Routine Description:

    Destroy the underlying IoTarget. 
    NOTE: This code executes in two paths: 
          1. QueryRemove/RemoveComplete (Underlying target is removed.)
          2. When device is removed normally (Driver disable).
    In the first case this call is not necessary because the Module has already
    been closed, but the call is benign because the IoTarget is already NULL.
    In the second path, however, this call is necessary.
    NOTE: This function is not paged because it can acquire a spinlock.
          
Arguments:

    DmfModule - The given Module.
    Target - Stores the IoTarget to destroy.

Return Value:

    None

--*/
{
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DMF_CONFIG_DeviceInterfaceMultipleTarget* moduleConfig;

    moduleContext = DMF_CONTEXT_GET(DmfModule);
    moduleConfig = DMF_CONFIG_GET(DmfModule);

    // It is important to check the IoTarget because it may have been closed via 
    // two asynchronous removal paths: 1. Device is removed. 2. Underlying target is removed.
    //
    BOOLEAN closeTarget;
    DMF_ModuleLock(DmfModule);
    if (!Target->TargetClosedOrClosing)
    {
        // This code path indicates that target close and rundown will start.
        // Target->IoTarget can be NULL if create/open failed.
        //
        Target->TargetClosedOrClosing = TRUE;
        closeTarget = TRUE;
    }
    else
    {
        closeTarget = FALSE;
    }
    DMF_ModuleUnlock(DmfModule);

    if (closeTarget)
    {
        if (Target->DmfModuleRequestTarget != NULL)
        {
            if (moduleContext->ContinuousRequestTargetMode == ContinuousRequestTarget_Mode_Automatic)
            {
                // By calling this function here, callbacks at the Client will happen only before the Module is closed.
                //
                DMF_ContinuousRequestTarget_StopAndWait(Target->DmfModuleRequestTarget);
            }
        }

        // Destroy the underlying IoTarget. NOTE: This will cancel all pending requests including
        // synchronous requests. This needs to happen before Rundown waits otherwise Rundown waits
        // forever for synchronous requests.
        //
        if (Target->IoTarget != NULL)
        {
            WdfIoTargetClose(Target->IoTarget);

            // Ensure that all Methods running against this Target finish executing
            // and prevent new Methods from starting because the IoTarget will be 
            // set to NULL.
            //
            if (Target->DmfModuleRundown != NULL)
            {
                // This Module is only created after the target has been opened. So, if the
                // underlying target cannot open and returns error, this Module is not created.
                // In that case, this clean up function must check to see if the handle is
                // valid otherwise a BSOD will happen.
                //
                DMF_Rundown_EndAndWait(Target->DmfModuleRundown);
            }

            if (moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChange != NULL)
            {
                DmfAssert(moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChangeEx == NULL);
                moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChange(DmfModule,
                                                                            Target->DmfIoTarget,
                                                                            DeviceInterfaceMultipleTarget_StateType_Close);
            }
            else if (moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChangeEx != NULL)
            {
                moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChangeEx(DmfModule,
                                                                              Target->DmfIoTarget,
                                                                              DeviceInterfaceMultipleTarget_StateType_Close);
            }
            // The target is about to go away...Wait for all pending Methods using the target
            // to finish executing and don't let new Methods start.
            //
            moduleContext->RequestSink_IoTargetClear(DmfModule,
                                                     Target);
            WdfObjectDelete(Target->IoTarget);
            // Now, the Target's handle can be cleared because no other thread will use it.
            // (It is not necessary to clear it as it will be deleted just below.)
            //
            Target->IoTarget = NULL;
        }
        else
        {
            // This path means that the WDFIOTARGET appeared but the Client decided not to 
            // open it or it cannot be opened.
            //
        }
    }

    // Delete stored symbolic link if set. (This will never be set in User-mode.)
    //
    DeviceInterfaceMultipleTarget_SymbolicLinkNameClear(DmfModule,
                                                        Target);

    if (Target->DmfIoTarget)
    {
        WdfObjectDelete(Target->DmfIoTarget);
        Target->DmfIoTarget = NULL;
    }

    DMF_BufferQueue_Reuse(moduleContext->DmfModuleBufferQueue,
                          (VOID *)Target);
}

#pragma code_seg("PAGE")
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
static
NTSTATUS
DeviceInterfaceMultipleTarget_ModuleOpenIfNoOpenTargets(
    _In_ DMFMODULE DmfModule
    )
/*++

Routine Description:

    Open the given Module there are no open Targets.

Arguments:

    DmfModule - The given Module.

Return Value:

    NSTATUS of Module's Open callback.

--*/
{
    NTSTATUS ntStatus;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;

    PAGED_CODE();

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    ntStatus = STATUS_SUCCESS;

    // No lock is used here, since the PnP callback is synchronous.
    //
    if (InterlockedIncrement(&moduleContext->NumberOfTargetsOpened) == 1)
    {
        // Open the Module.
        //
        ntStatus = DMF_ModuleOpen(DmfModule);
    }

    return ntStatus;
}
#pragma code_seg()

#pragma code_seg("PAGE")
_IRQL_requires_max_(PASSIVE_LEVEL)
static
VOID
DeviceInterfaceMultipleTarget_ModuleCloseIfNoOpenTargets(
    _In_ DMFMODULE DmfModule
    )
/*++

Routine Description:

    Close the given Module there are no open Targets.

Arguments:

    DmfModule - The given Module.

Return Value:

    None

--*/
{
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;

    PAGED_CODE();

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    // No lock is used here, since the PnP callback is synchronous.
    //
    if (InterlockedDecrement(&moduleContext->NumberOfTargetsOpened) == 0)
    {
        // Close the Module.
        //
        DMF_ModuleClose(DmfModule);
    }
}
#pragma code_seg()

#pragma code_seg("PAGE")
_IRQL_requires_max_(PASSIVE_LEVEL)
static
VOID
DeviceInterfaceMultipleTarget_TargetDestroyAndCloseModule(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target
    )
/*++

Routine Description:

    Destroy the underlying IoTarget. 
    Reuse the target buffer.
    Close the Module if its last target.

Arguments:

    DmfModule - The given Module.
    Target - 

Return Value:

    None

--*/
{
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;

    PAGED_CODE();

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    DeviceInterfaceMultipleTarget_TargetDestroy(DmfModule,
                                                Target);

    DeviceInterfaceMultipleTarget_ModuleCloseIfNoOpenTargets(DmfModule);
}
#pragma code_seg()

// ContinuousRequestTarget Methods
// -------------------------------
//

BOOLEAN
DeviceInterfaceMultipleTarget_Stream_Cancel(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target,
    _In_ RequestTarget_DmfRequest DmfRequestId
    )
{
    BOOLEAN returnValue;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    DmfAssert(moduleContext->ContinuousReaderMode);
    returnValue = DMF_ContinuousRequestTarget_Cancel(Target->DmfModuleRequestTarget,
                                                     DmfRequestId);

    return returnValue;
}

_Must_inspect_result_
NTSTATUS
DeviceInterfaceMultipleTarget_Stream_SendSynchronously(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target,
    _In_reads_bytes_opt_(RequestLength) VOID* RequestBuffer,
    _In_ size_t RequestLength,
    _Out_writes_bytes_opt_(ResponseLength) VOID* ResponseBuffer,
    _In_ size_t ResponseLength,
    _In_ ContinuousRequestTarget_RequestType RequestType,
    _In_ ULONG RequestIoctl,
    _In_ ULONG RequestTimeout,
    _Out_opt_ size_t* BytesWritten
    )
{
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    DmfAssert(moduleContext->ContinuousReaderMode);
    return DMF_ContinuousRequestTarget_SendSynchronously(Target->DmfModuleRequestTarget,
                                                         RequestBuffer,
                                                         RequestLength,
                                                         ResponseBuffer,
                                                         ResponseLength,
                                                         RequestType,
                                                         RequestIoctl,
                                                         RequestTimeout,
                                                         BytesWritten);
}

_Must_inspect_result_
NTSTATUS
DeviceInterfaceMultipleTarget_Stream_Send(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target,
    _In_reads_bytes_opt_(RequestLength) VOID* RequestBuffer,
    _In_ size_t RequestLength,
    _Out_writes_bytes_opt_(ResponseLength) VOID* ResponseBuffer,
    _In_ size_t ResponseLength,
    _In_ ContinuousRequestTarget_RequestType RequestType,
    _In_ ULONG RequestIoctl,
    _In_ ULONG RequestTimeoutMilliseconds,
    _In_opt_ EVT_DMF_ContinuousRequestTarget_SendCompletion* EvtRequestSinkSingleAsynchronousRequest,
    _In_opt_ VOID* SingleAsynchronousRequestClientContext
    )
{
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    DmfAssert(moduleContext->ContinuousReaderMode);
    return DMF_ContinuousRequestTarget_Send(Target->DmfModuleRequestTarget,
                                            RequestBuffer,
                                            RequestLength,
                                            ResponseBuffer,
                                            ResponseLength,
                                            RequestType,
                                            RequestIoctl,
                                            RequestTimeoutMilliseconds,
                                            EvtRequestSinkSingleAsynchronousRequest,
                                            SingleAsynchronousRequestClientContext);
}

_Must_inspect_result_
NTSTATUS
DeviceInterfaceMultipleTarget_Stream_SendEx(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target,
    _In_reads_bytes_opt_(RequestLength) VOID* RequestBuffer,
    _In_ size_t RequestLength,
    _Out_writes_bytes_opt_(ResponseLength) VOID* ResponseBuffer,
    _In_ size_t ResponseLength,
    _In_ ContinuousRequestTarget_RequestType RequestType,
    _In_ ULONG RequestIoctl,
    _In_ ULONG RequestTimeoutMilliseconds,
    _In_opt_ EVT_DMF_ContinuousRequestTarget_SendCompletion* EvtRequestSinkSingleAsynchronousRequest,
    _In_opt_ VOID* SingleAsynchronousRequestClientContext,
    _Out_opt_ RequestTarget_DmfRequest* DmfRequestId
    )
{
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    DmfAssert(moduleContext->ContinuousReaderMode);
    return DMF_ContinuousRequestTarget_SendEx(Target->DmfModuleRequestTarget,
                                              RequestBuffer,
                                              RequestLength,
                                              ResponseBuffer,
                                              ResponseLength,
                                              RequestType,
                                              RequestIoctl,
                                              RequestTimeoutMilliseconds,
                                              EvtRequestSinkSingleAsynchronousRequest,
                                              SingleAsynchronousRequestClientContext,
                                              DmfRequestId);
}

VOID
DeviceInterfaceMultipleTarget_Stream_IoTargetSet(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target,
    _In_ WDFIOTARGET IoTarget
    )
{
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    DmfAssert(moduleContext->ContinuousReaderMode);
    DMF_ContinuousRequestTarget_IoTargetSet(Target->DmfModuleRequestTarget,
                                            IoTarget);
}

VOID
DeviceInterfaceMultipleTarget_Stream_IoTargetClear(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target
    )
{
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    DmfAssert(moduleContext->ContinuousReaderMode);
    DMF_ContinuousRequestTarget_IoTargetClear(Target->DmfModuleRequestTarget);
}

// RequestTarget Methods
// ---------------------
//

BOOLEAN
DeviceInterfaceMultipleTarget_Target_Cancel(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target,
    _In_ RequestTarget_DmfRequest DmfRequestId
    )
{
    BOOLEAN returnValue;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    DmfAssert(! moduleContext->ContinuousReaderMode);

    returnValue = DMF_RequestTarget_Cancel(Target->DmfModuleRequestTarget,
                                           DmfRequestId);

    return returnValue;
}

_Must_inspect_result_
NTSTATUS
DeviceInterfaceMultipleTarget_Target_SendSynchronously(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target,
    _In_reads_bytes_opt_(RequestLength) VOID* RequestBuffer,
    _In_ size_t RequestLength,
    _Out_writes_bytes_opt_(ResponseLength) VOID* ResponseBuffer,
    _In_ size_t ResponseLength,
    _In_ ContinuousRequestTarget_RequestType RequestType,
    _In_ ULONG RequestIoctl,
    _In_ ULONG RequestTimeoutMilliseconds,
    _Out_opt_ size_t* BytesWritten
    )
{
    NTSTATUS ntStatus;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    DmfAssert(! moduleContext->ContinuousReaderMode);
    ntStatus = DMF_RequestTarget_SendSynchronously(Target->DmfModuleRequestTarget,
                                                   RequestBuffer,
                                                   RequestLength,
                                                   ResponseBuffer,
                                                   ResponseLength,
                                                   RequestType,
                                                   RequestIoctl,
                                                   RequestTimeoutMilliseconds,
                                                   BytesWritten);

    return ntStatus;
}

_Must_inspect_result_
NTSTATUS
DeviceInterfaceMultipleTarget_Target_Send(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target,
    _In_reads_bytes_opt_(RequestLength) VOID* RequestBuffer,
    _In_ size_t RequestLength,
    _Out_writes_bytes_opt_(ResponseLength) VOID* ResponseBuffer,
    _In_ size_t ResponseLength,
    _In_ ContinuousRequestTarget_RequestType RequestType,
    _In_ ULONG RequestIoctl,
    _In_ ULONG RequestTimeoutMilliseconds,
    _In_opt_ EVT_DMF_ContinuousRequestTarget_SendCompletion* EvtRequestSinkSingleAsynchronousRequest,
    _In_opt_ VOID* SingleAsynchronousRequestClientContext
    )
{
    NTSTATUS ntStatus;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    DmfAssert(! moduleContext->ContinuousReaderMode);
    ntStatus = DMF_RequestTarget_Send(Target->DmfModuleRequestTarget,
                                      RequestBuffer,
                                      RequestLength,
                                      ResponseBuffer,
                                      ResponseLength,
                                      RequestType,
                                      RequestIoctl,
                                      RequestTimeoutMilliseconds,
                                      EvtRequestSinkSingleAsynchronousRequest,
                                      SingleAsynchronousRequestClientContext);

    return ntStatus;
}

_Must_inspect_result_
NTSTATUS
DeviceInterfaceMultipleTarget_Target_SendEx(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target,
    _In_reads_bytes_opt_(RequestLength) VOID* RequestBuffer,
    _In_ size_t RequestLength,
    _Out_writes_bytes_opt_(ResponseLength) VOID* ResponseBuffer,
    _In_ size_t ResponseLength,
    _In_ ContinuousRequestTarget_RequestType RequestType,
    _In_ ULONG RequestIoctl,
    _In_ ULONG RequestTimeoutMilliseconds,
    _In_opt_ EVT_DMF_ContinuousRequestTarget_SendCompletion* EvtRequestSinkSingleAsynchronousRequest,
    _In_opt_ VOID* SingleAsynchronousRequestClientContext,
    _Out_opt_ RequestTarget_DmfRequest* DmfRequestId
    )
{
    NTSTATUS ntStatus;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    DmfAssert(! moduleContext->ContinuousReaderMode);
    ntStatus = DMF_RequestTarget_SendEx(Target->DmfModuleRequestTarget,
                                        RequestBuffer,
                                        RequestLength,
                                        ResponseBuffer,
                                        ResponseLength,
                                        RequestType,
                                        RequestIoctl,
                                        RequestTimeoutMilliseconds,
                                        EvtRequestSinkSingleAsynchronousRequest,
                                        SingleAsynchronousRequestClientContext,
                                        DmfRequestId);

    return ntStatus;
}

VOID
DeviceInterfaceMultipleTarget_Target_IoTargetSet(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target,
    _In_ WDFIOTARGET IoTarget
    )
{
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    DmfAssert(! moduleContext->ContinuousReaderMode);
    DMF_RequestTarget_IoTargetSet(Target->DmfModuleRequestTarget,
                                  IoTarget);
}

VOID
DeviceInterfaceMultipleTarget_Target_IoTargetClear(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target
    )
{
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    DmfAssert(! moduleContext->ContinuousReaderMode);
    DMF_RequestTarget_IoTargetClear(Target->DmfModuleRequestTarget);
}

// General Module Support Code
// ---------------------------
//

_Function_class_(EVT_DMF_BufferPool_Enumeration)
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_same_
BufferPool_EnumerationDispositionType
DeviceInterfaceMultipleTarget_FindTarget(
    _In_ DMFMODULE DmfModule,
    _In_ VOID* ClientBuffer,
    _In_ VOID* ClientBufferContext,
    _In_opt_ VOID* ClientDriverCallbackContext
    )
/*++

Routine Description:

    Enumeration callback to check if a target is already available in the pool.

Arguments:

    DmfModule - ContinuousRequestTarget DMFMODULE.
    ClientBuffer - The given input buffer.
    ClientBufferContext - Context associated with the given input buffer.
    ClientDriverCallbackContext - Context for this enumeration.

Return Value:

    None

--*/
{
    DeviceInterfaceMultipleTarget_EnumerationContext *callbackContext;
    DeviceInterfaceMultipleTarget_IoTarget *target;
    DeviceInterfaceMultipleTarget_IoTarget *targetToCompare;
    BufferPool_EnumerationDispositionType returnValue;

    FuncEntry(DMF_TRACE);

    UNREFERENCED_PARAMETER(DmfModule);
    UNREFERENCED_PARAMETER(ClientBufferContext);

    target = (DeviceInterfaceMultipleTarget_IoTarget *)ClientBuffer;
    DmfAssert(target->SymbolicLinkName.Length != 0);
    DmfAssert(target->SymbolicLinkName.Buffer != NULL);
    // After RemoveComplete IoTarget is NULL because it was cleared in QueryRemove.
    //

    callbackContext = (DeviceInterfaceMultipleTarget_EnumerationContext*)ClientDriverCallbackContext;
    // 'Dereferencing NULL pointer. 'callbackContext'
    //
    #pragma warning(suppress:28182)
    targetToCompare = (DeviceInterfaceMultipleTarget_IoTarget*)callbackContext->ContextData;

    returnValue = BufferPool_EnumerationDisposition_ContinueEnumeration;

    if (target == targetToCompare)
    {
        callbackContext->BufferFound = TRUE;
        if (callbackContext->RemoveBuffer)
        {
            returnValue = BufferPool_EnumerationDisposition_RemoveAndStopEnumeration;
        }
        else
        {
            returnValue = BufferPool_EnumerationDisposition_StopEnumeration;
        }
    }

    FuncExit(DMF_TRACE, "Enumeration Disposition=%d", returnValue);

    return returnValue;
}

_Function_class_(EVT_DMF_BufferPool_Enumeration)
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_same_
BufferPool_EnumerationDispositionType
DeviceInterfaceMultipleTarget_FindSymbolicLink(
    _In_ DMFMODULE DmfModule,
    _In_ VOID* ClientBuffer,
    _In_ VOID* ClientBufferContext,
    _In_opt_ VOID* ClientDriverCallbackContext
    )
/*++

Routine Description:

    Enumeration callback to check if a target with the same symbolic link is already available in the pool. 

Arguments:

    DmfModule - ContinuousRequestTarget DMFMODULE.
    ClientBuffer - The given input buffer.
    ClientBufferContext - Context associated with the given input buffer.
    ClientDriverCallbackContext - Context for this enumeration.

Return Value:

    None

--*/
{
    DeviceInterfaceMultipleTarget_IoTarget *target;
    DeviceInterfaceMultipleTarget_EnumerationContext* callbackContext;
    BufferPool_EnumerationDispositionType returnValue;
    PUNICODE_STRING symbolicLinkName;
    SIZE_T matchLength;

    FuncEntry(DMF_TRACE);

    UNREFERENCED_PARAMETER(DmfModule);
    UNREFERENCED_PARAMETER(ClientBufferContext);

    target = (DeviceInterfaceMultipleTarget_IoTarget *)ClientBuffer;
    DmfAssert(target->SymbolicLinkName.Length != 0);
    DmfAssert(target->SymbolicLinkName.Buffer != NULL);
    // NOTE: target->IoTarget = NULL if IoTarget could not be opened again
    //       during "RemoveCancel" path.
    //

    callbackContext = (DeviceInterfaceMultipleTarget_EnumerationContext*)ClientDriverCallbackContext;
    // 'Dereferencing NULL pointer. 'callbackContext'
    //
    #pragma warning(suppress:28182)
    symbolicLinkName = (PUNICODE_STRING)callbackContext->ContextData;

    returnValue = BufferPool_EnumerationDisposition_ContinueEnumeration;

    if (target->SymbolicLinkName.Length == symbolicLinkName->Length)
    {
        matchLength = RtlCompareMemory((VOID*)target->SymbolicLinkName.Buffer,
                                       symbolicLinkName->Buffer,
                                       target->SymbolicLinkName.Length);
        if (target->SymbolicLinkName.Length == matchLength)
        {
            callbackContext->BufferFound = TRUE;
            if (callbackContext->RemoveBuffer)
            {
                returnValue = BufferPool_EnumerationDisposition_RemoveAndStopEnumeration;
            }
            else
            {
                returnValue = BufferPool_EnumerationDisposition_StopEnumeration;
            }
        }
    }

    FuncExit(DMF_TRACE, "Enumeration Disposition=%d", BufferPool_EnumerationDisposition_RemoveAndStopEnumeration);

    return returnValue;
}

DeviceInterfaceMultipleTarget_IoTarget*
DeviceInterfaceMultipleTarget_BufferGet(
    DeviceInterfaceMultipleTarget_Target Target
    )
/*++

Routine Description:

    Method to get the buffer associated with the given DeviceInterfaceMultipleTarget_Target handle. 

Arguments:

    Target - The given DeviceInterfaceMultipleTarget_Target handle.

Return Value:

    Pointer to buffer.

--*/
{
    DeviceInterfaceMultipleTarget_IoTarget* target;
    size_t bufferSize;

    target = (DeviceInterfaceMultipleTarget_IoTarget*)WdfMemoryGetBuffer((WDFMEMORY)Target,
                                                                         &bufferSize);

    DmfAssert(bufferSize == sizeof(DeviceInterfaceMultipleTarget_IoTarget));

    return target;
}

_Function_class_(EVT_DMF_ContinuousRequestTarget_BufferInput)
VOID
DeviceInterfaceMultipleTarget_Stream_BufferInput(
    _In_ DMFMODULE DmfModule,
    _Out_writes_(*InputBufferSize) VOID* InputBuffer,
    _Out_ size_t* InputBufferSize,
    _In_ VOID* ClientBuferContextInput
    )
/*++

Routine Description:

    Redirect input buffer callback from Request Stream to Parent Module/Device. 

Arguments:

    DmfModule - ContinuousRequestTarget DMFMODULE.
    InputBuffer - The given input buffer.
    InputBufferSize - Size of the given input buffer.
    ClientBuferContextInput - Context associated with the given input buffer.

Return Value:

    None

--*/
{
    DMFMODULE dmfModule;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;

    FuncEntry(DMF_TRACE);

    dmfModule = DMF_ParentModuleGet(DmfModule);
    DmfAssert(dmfModule != NULL);

    moduleContext = DMF_CONTEXT_GET(dmfModule);

    if (moduleContext->EvtContinuousRequestTargetBufferInput != NULL)
    {
        // 'Using uninitialized memory '*InputBufferSize''
        //
        #pragma warning(disable:6001)
        moduleContext->EvtContinuousRequestTargetBufferInput(dmfModule,
                                                             InputBuffer,
                                                             InputBufferSize,
                                                             ClientBuferContextInput);
    }
    else
    {
        // For SAL.
        //
        *InputBufferSize = 0;
    }

    FuncExitVoid(DMF_TRACE);
}

_Function_class_(EVT_DMF_ContinuousRequestTarget_BufferOutput)
ContinuousRequestTarget_BufferDisposition
DeviceInterfaceMultipleTarget_Stream_BufferOutput(
    _In_ DMFMODULE DmfModule,
    _In_reads_(OutputBufferSize) VOID* OutputBuffer,
    _In_ size_t OutputBufferSize,
    _In_ VOID* ClientBufferContextOutput,
    _In_ NTSTATUS CompletionStatus
    )
/*++

Routine Description:

    Redirect output buffer callback from Request Stream to Parent Module/Device. 

Arguments:

    DmfModule - ContinuousRequestTarget DMFMODULE.
    OutputBuffer - The given output buffer.
    OutputBufferSize - Size of the given output buffer.
    ClientBufferContextOutput - Context associated with the given output buffer.
    CompletionStatus - Request completion status.

Return Value:

    ContinuousRequestTarget_BufferDisposition.

--*/
{
    DMFMODULE dmfModule;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    ContinuousRequestTarget_BufferDisposition bufferDisposition;

    FuncEntry(DMF_TRACE);

    dmfModule = DMF_ParentModuleGet(DmfModule);
    DmfAssert(dmfModule != NULL);

    moduleContext = DMF_CONTEXT_GET(dmfModule);

    if (moduleContext->EvtContinuousRequestTargetBufferOutput != NULL)
    {
        bufferDisposition = moduleContext->EvtContinuousRequestTargetBufferOutput(dmfModule,
                                                                                  OutputBuffer,
                                                                                  OutputBufferSize,
                                                                                  ClientBufferContextOutput,
                                                                                  CompletionStatus);
    }
    else
    {
        bufferDisposition = ContinuousRequestTarget_BufferDisposition_ContinuousRequestTargetAndContinueStreaming;
    }

    FuncExit(DMF_TRACE, "bufferDisposition=%d", bufferDisposition);

    return bufferDisposition;
}

VOID
DeviceInterfaceMultipleTarget_StopTargetAndCloseModule(
    _In_ WDFIOTARGET IoTarget
    )
/*++

Routine Description:

    Stops the target and Closes the Module. This is called from QueryRemove.
    It is also called from RemoveComplete in the surprise-removal case because
    QueryRemove does not happen in that path.

Arguments:

    IoTarget - A handle to an I/O target object.

Return Value:

    None

--*/
{
    DMFMODULE dmfModule;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DMF_CONFIG_DeviceInterfaceMultipleTarget* moduleConfig;
    DeviceInterfaceMultipleTarget_IoTargetContext* targetContext;
    DeviceInterfaceMultipleTarget_IoTarget* target;

    FuncEntry(DMF_TRACE);

    // The IoTarget's Module Context area has the DMF Module.
    //
    targetContext = WdfObjectGet_DeviceInterfaceMultipleTarget_IoTargetContext(IoTarget);
    dmfModule = targetContext->DmfModuleDeviceInterfaceMultipleTarget;
    target = targetContext->Target;

    moduleContext = DMF_CONTEXT_GET(dmfModule);
    moduleConfig = DMF_CONFIG_GET(dmfModule);

    // Transparently stop the stream in automatic mode.
    //
    if (moduleContext->ContinuousRequestTargetMode == ContinuousRequestTarget_Mode_Automatic)
    {
        DMF_DeviceInterfaceMultipleTarget_StreamStop(dmfModule,
                                                     target->DmfIoTarget);
    }

    // Don't let Methods call while in QueryRemoved state.
    // 
    DMF_Rundown_EndAndWait(target->DmfModuleRundown);

    // QueryRemove will Close the Module but not remove the Target from the Queue.
    //
    DeviceInterfaceMultipleTarget_ModuleCloseIfNoOpenTargets(dmfModule);

    FuncExitVoid(DMF_TRACE);
}

EVT_WDF_IO_TARGET_QUERY_REMOVE DeviceInterfaceMultipleTarget_EvtIoTargetQueryRemove;

_Function_class_(EVT_WDF_IO_TARGET_QUERY_REMOVE)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
DeviceInterfaceMultipleTarget_EvtIoTargetQueryRemove(
    _In_ WDFIOTARGET IoTarget
    )
/*++

Routine Description:

    Indicates whether the framework can safely remove a specified remote I/O target's device.

Arguments:

    IoTarget - A handle to an I/O target object.

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS ntStatus;
    DMFMODULE dmfModule;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DMF_CONFIG_DeviceInterfaceMultipleTarget* moduleConfig;
    DeviceInterfaceMultipleTarget_IoTargetContext* targetContext;
    DeviceInterfaceMultipleTarget_IoTarget* target;

    ntStatus = STATUS_SUCCESS;

    FuncEntry(DMF_TRACE);

    // The IoTarget's Module Context area has the DMF Module.
    //
    targetContext = WdfObjectGet_DeviceInterfaceMultipleTarget_IoTargetContext(IoTarget);
    dmfModule = targetContext->DmfModuleDeviceInterfaceMultipleTarget;
    target = targetContext->Target;

    moduleContext = DMF_CONTEXT_GET(dmfModule);
    moduleConfig = DMF_CONFIG_GET(dmfModule);

    // Remember this happened so that we can adjust for cases where it does not.
    //
    ASSERT(!target->QueryRemoveHappened);
    target->QueryRemoveHappened = TRUE;

    // If the Client has registered for device interface state changes, call the notification callback.
    //
    if (moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChange != NULL)
    {
        DmfAssert(moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChangeEx == NULL);
        moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChange(dmfModule,
                                                                    target->DmfIoTarget,
                                                                    DeviceInterfaceMultipleTarget_StateType_QueryRemove);
    }
    else if (moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChangeEx != NULL)
    {
        // This version allows Client to veto the remove.
        //
        ntStatus = moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChangeEx(dmfModule,
                                                                                 target->DmfIoTarget,
                                                                                 DeviceInterfaceMultipleTarget_StateType_QueryRemove);
    }

    if (NT_SUCCESS(ntStatus))
    {
        // Stop the target and Close the Module.
        //
        DeviceInterfaceMultipleTarget_StopTargetAndCloseModule(IoTarget);

        WdfIoTargetCloseForQueryRemove(IoTarget);
    }

    FuncExit(DMF_TRACE, "ntStatus=%!STATUS!", ntStatus);

    return ntStatus;
}

EVT_WDF_IO_TARGET_REMOVE_CANCELED DeviceInterfaceMultipleTarget_EvtIoTargetRemoveCanceled;

VOID
DeviceInterfaceMultipleTarget_EvtIoTargetRemoveCanceled(
    _In_ WDFIOTARGET IoTarget
    )
/*++

Routine Description:

    Performs operations when the removal of a specified remote I/O target is canceled.

Arguments:

    IoTarget - A handle to an I/O target object.

Return Value:

    None

--*/
{
    NTSTATUS ntStatus;
    DMFMODULE dmfModule;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DMF_CONFIG_DeviceInterfaceMultipleTarget* moduleConfig;
    DeviceInterfaceMultipleTarget_IoTargetContext* targetContext;
    WDF_IO_TARGET_OPEN_PARAMS openParams;
    DeviceInterfaceMultipleTarget_IoTarget* target;

    FuncEntry(DMF_TRACE);

    // The IoTarget's Module Context area has the DMF Module.
    //
    targetContext = WdfObjectGet_DeviceInterfaceMultipleTarget_IoTargetContext(IoTarget);
    dmfModule = targetContext->DmfModuleDeviceInterfaceMultipleTarget;
    target = targetContext->Target;

    moduleContext = DMF_CONTEXT_GET(dmfModule);
    moduleConfig = DMF_CONFIG_GET(dmfModule);

    DmfAssert(target->IoTarget == IoTarget);
    target->IoTarget = IoTarget;

    // Clear this flag in case it was set during QueryRemove.
    //
    target->QueryRemoveHappened = FALSE;

    WDF_IO_TARGET_OPEN_PARAMS_INIT_REOPEN(&openParams);

    ntStatus = WdfIoTargetOpen(IoTarget,
                               &openParams);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "WdfIoTargetOpen fails: ntStatus=%!STATUS!", ntStatus);
        WdfObjectDelete(IoTarget);
        // Clear target so that Close/Delete paths do not happen as they have
        // already happened.
        //
        target->IoTarget = NULL;
        goto Exit;
    }

    // Now, the Counters which are not matched will become matched again.
    // The counters became mismatched in QueryRemove.
    //
    ntStatus = DeviceInterfaceMultipleTarget_ModuleOpenIfNoOpenTargets(dmfModule);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DeviceInterfaceMultipleTarget_ModuleOpenIfNoOpenTargets fails: ntStatus=%!STATUS!", ntStatus);
        goto Exit;
    }

    // Rundown ended in QueryRemove. Restart again. Allow Clients to call Methods.
    //
    DMF_Rundown_Start(target->DmfModuleRundown);

    // If the client has registered for device interface state changes, call the notification callback.
    //
    if (moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChange != NULL)
    {
        DmfAssert(moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChangeEx == NULL);
        moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChange(dmfModule,
                                                                    target->DmfIoTarget,
                                                                    DeviceInterfaceMultipleTarget_StateType_QueryRemoveCancelled);
    }
    else if (moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChangeEx != NULL)
    {
        moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChangeEx(dmfModule,
                                                                      target->DmfIoTarget,
                                                                      DeviceInterfaceMultipleTarget_StateType_QueryRemoveCancelled);
    }

    // Transparently restart the stream in automatic mode.
    //
    if (moduleContext->ContinuousRequestTargetMode == ContinuousRequestTarget_Mode_Automatic)
    {
        ntStatus = DMF_DeviceInterfaceMultipleTarget_StreamStart(dmfModule,
                                                                 target->DmfIoTarget);
        if (! NT_SUCCESS(ntStatus))
        {
            TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_DeviceInterfaceMultipleTarget_StreamStart fails: ntStatus=%!STATUS!", ntStatus);
        }
    }

Exit:

    FuncExitVoid(DMF_TRACE);
}

EVT_WDF_IO_TARGET_REMOVE_COMPLETE DeviceInterfaceMultipleTarget_EvtIoTargetRemoveComplete;

VOID
DeviceInterfaceMultipleTarget_EvtIoTargetRemoveComplete(
    WDFIOTARGET IoTarget
    )
/*++

Routine Description:

    Called when the Target device is removed ( either the target
    received IRP_MN_REMOVE_DEVICE or IRP_MN_SURPRISE_REMOVAL)

Arguments:

    IoTarget - A handle to an I/O target object.

Return Value:

    None

--*/
{
    DMFMODULE dmfModule;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DMF_CONFIG_DeviceInterfaceMultipleTarget* moduleConfig;
    DeviceInterfaceMultipleTarget_IoTargetContext* targetContext;
    DeviceInterfaceMultipleTarget_IoTarget* target;
    DeviceInterfaceMultipleTarget_EnumerationContext callbackContext;

    FuncEntry(DMF_TRACE);

    // The IoTarget's Module Context area has the DMF Module.
    //
    targetContext = WdfObjectGet_DeviceInterfaceMultipleTarget_IoTargetContext(IoTarget);
    dmfModule = targetContext->DmfModuleDeviceInterfaceMultipleTarget;

    moduleContext = DMF_CONTEXT_GET(dmfModule);
    moduleConfig = DMF_CONFIG_GET(dmfModule);

    callbackContext.ContextData = (VOID*)targetContext->Target;
    callbackContext.RemoveBuffer = TRUE;
    callbackContext.BufferFound = FALSE;
    DMF_BufferQueue_Enumerate(moduleContext->DmfModuleBufferQueue,
                              DeviceInterfaceMultipleTarget_FindTarget,
                              (VOID *)&callbackContext,
                              (VOID **)&target,
                              NULL);
    if (! callbackContext.BufferFound)
    {
        // The target buffer should be in the consumer pool.
        // 
        DmfAssert(FALSE);
        goto Exit;
    }

    // First, tell Client RemoveComplete is happening. In case when QueryRemove did not
    // happen, Client still has chance to access underlying WDFIOTARGET.
    //
    if (moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChange != NULL)
    {
        DmfAssert(moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChangeEx == NULL);
        moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChange(dmfModule,
                                                                    target->DmfIoTarget,
                                                                    DeviceInterfaceMultipleTarget_StateType_QueryRemoveComplete);
    }
    else if (moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChangeEx != NULL)
    {
        moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChangeEx(dmfModule,
                                                                      target->DmfIoTarget,
                                                                      DeviceInterfaceMultipleTarget_StateType_QueryRemoveComplete);
    }

    // If QueryRemove did not happen, close the underlying WDFIOTARGET.
    //
    if (!target->QueryRemoveHappened)
    {
        // Surprise Remove happened, so QueryRemove did not happen. The Target
        // still needs to be stopped and Module Closed.
        //
        DeviceInterfaceMultipleTarget_StopTargetAndCloseModule(IoTarget);
    }
    else
    {
        // Clear for next time.
        //
        target->QueryRemoveHappened = FALSE;
    }

    // The underlying target has been removed and is no longer accessible.
    // Module is already closed in QueryRemove path.
    //
    DeviceInterfaceMultipleTarget_TargetDestroy(dmfModule,
                                                target);

Exit:

    FuncExitVoid(DMF_TRACE);
}

#pragma code_seg("PAGE")
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS
DeviceInterfaceMultipleTarget_ContinuousRequestTargetCreate(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target
    )
/*++

Routine Description:

    Configure and add the required Child Modules to the given Parent Module.

Arguments:

    DmfModule - The given Parent Module.
    DmfParentModuleAttributes - Pointer to the parent DMF_MODULE_ATTRIBUTES structure.
    DmfModuleInit - Opaque structure to be passed to DMF_DmfModuleAdd.

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS ntStatus;
    WDFDEVICE device;
    DMF_MODULE_ATTRIBUTES moduleAttributes;
    DMF_CONFIG_DeviceInterfaceMultipleTarget* moduleConfig;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    WDF_OBJECT_ATTRIBUTES objectAttributes;

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    ntStatus = STATUS_SUCCESS;
    device = DMF_ParentDeviceGet(DmfModule);
    moduleConfig = DMF_CONFIG_GET(DmfModule);
    moduleContext = DMF_CONTEXT_GET(DmfModule);

    WDF_OBJECT_ATTRIBUTES_INIT(&objectAttributes);
    objectAttributes.ParentObject = DmfModule;

    // If Client has set ContinousRequestCount > 0, then it means streaming is capable.
    // Otherwise, streaming is not capable.
    //
    if (moduleConfig->ContinuousRequestTargetModuleConfig.ContinuousRequestCount > 0)
    {
        // ContinuousRequestTarget
        // -----------------------
        //
        DMF_CONFIG_ContinuousRequestTarget moduleConfigContinuousRequestTarget;

        // Store ContinuousRequestTarget callbacks from config into DeviceInterfaceMultipleTarget context for redirection.
        //
        moduleContext->EvtContinuousRequestTargetBufferInput = moduleConfig->ContinuousRequestTargetModuleConfig.EvtContinuousRequestTargetBufferInput;
        moduleContext->EvtContinuousRequestTargetBufferOutput = moduleConfig->ContinuousRequestTargetModuleConfig.EvtContinuousRequestTargetBufferOutput;

        DMF_CONFIG_ContinuousRequestTarget_AND_ATTRIBUTES_INIT(&moduleConfigContinuousRequestTarget,
                                                               &moduleAttributes);
        // Copy ContinuousRequestTarget Config from Client's Module Config.
        //
        RtlCopyMemory(&moduleConfigContinuousRequestTarget,
                      &moduleConfig->ContinuousRequestTargetModuleConfig,
                      sizeof(moduleConfig->ContinuousRequestTargetModuleConfig));
        // Replace ContinuousRequestTarget callbacks in config with DeviceInterfaceMultipleTarget callbacks.
        //
        moduleConfigContinuousRequestTarget.EvtContinuousRequestTargetBufferInput = DeviceInterfaceMultipleTarget_Stream_BufferInput;
        moduleConfigContinuousRequestTarget.EvtContinuousRequestTargetBufferOutput = DeviceInterfaceMultipleTarget_Stream_BufferOutput;

        moduleAttributes.PassiveLevel = moduleContext->PassiveLevel;
        ntStatus = DMF_ContinuousRequestTarget_Create(device,
                                                      &moduleAttributes,
                                                      &objectAttributes,
                                                      &Target->DmfModuleRequestTarget);
        if (!NT_SUCCESS(ntStatus))
        {
            TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_ContinuousRequestTarget_Create fails: ntStatus=%!STATUS!", ntStatus);
            goto Exit;
        }

        // Set the transport methods.
        //
        moduleContext->RequestSink_IoTargetClear = DeviceInterfaceMultipleTarget_Stream_IoTargetClear;
        moduleContext->RequestSink_IoTargetSet = DeviceInterfaceMultipleTarget_Stream_IoTargetSet;
        moduleContext->RequestSink_Send = DeviceInterfaceMultipleTarget_Stream_Send;
        moduleContext->RequestSink_SendEx = DeviceInterfaceMultipleTarget_Stream_SendEx;
        moduleContext->RequestSink_Cancel = DeviceInterfaceMultipleTarget_Stream_Cancel;
        moduleContext->RequestSink_SendSynchronously = DeviceInterfaceMultipleTarget_Stream_SendSynchronously;
        moduleContext->ContinuousReaderMode = TRUE;
        // Remember Client's choice so this Module can start/stop streaming appropriately.
        //
        moduleContext->ContinuousRequestTargetMode = moduleConfig->ContinuousRequestTargetModuleConfig.ContinuousRequestTargetMode;
    }
    else
    {
        // RequestTarget
        // -------------
        //

        // Streaming functionality is not required. 
        // Create DMF_RequestTarget instead of DMF_ContinuousRequestTarget.
        //

        DMF_RequestTarget_ATTRIBUTES_INIT(&moduleAttributes);
        moduleAttributes.PassiveLevel = moduleContext->PassiveLevel;
        ntStatus = DMF_RequestTarget_Create(device,
                                            &moduleAttributes,
                                            &objectAttributes,
                                            &Target->DmfModuleRequestTarget);
        if (!NT_SUCCESS(ntStatus))
        {
            TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_ContinuousRequestTarget_Create fails: ntStatus=%!STATUS!", ntStatus);
            goto Exit;
        }

        // Set the transport methods.
        //
        moduleContext->RequestSink_IoTargetClear = DeviceInterfaceMultipleTarget_Target_IoTargetClear;
        moduleContext->RequestSink_IoTargetSet = DeviceInterfaceMultipleTarget_Target_IoTargetSet;
        moduleContext->RequestSink_Send = DeviceInterfaceMultipleTarget_Target_Send;
        moduleContext->RequestSink_SendEx = DeviceInterfaceMultipleTarget_Target_SendEx;
        moduleContext->RequestSink_Cancel = DeviceInterfaceMultipleTarget_Target_Cancel;
        moduleContext->RequestSink_SendSynchronously = DeviceInterfaceMultipleTarget_Target_SendSynchronously;
        moduleContext->ContinuousReaderMode = FALSE;
    }

    DMF_Rundown_ATTRIBUTES_INIT(&moduleAttributes);
    ntStatus = DMF_Rundown_Create(device,
                                  &moduleAttributes,
                                  &objectAttributes,
                                  &Target->DmfModuleRundown);
    if (!NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_Rundown_Create fails: ntStatus=%!STATUS!", ntStatus);
        goto Exit;
    }

Exit:

    FuncExit(DMF_TRACE, "ntStatus=%!STATUS!", ntStatus);

    return ntStatus;
}
#pragma code_seg()

#pragma code_seg("PAGE")
_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
static
NTSTATUS
DeviceInterfaceMultipleTarget_DeviceCreateNewIoTargetByName(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_IoTarget* Target,
    _In_ PUNICODE_STRING SymbolicLinkName
    )
/*++

Routine Description:

    Open the target device similar to CreateFile().

Arguments:

    Device - This driver's WDFDEVICE.
    Target - 
    SymbolicLinkName - The name of the device to open.

Return Value:

    STATUS_SUCCESS or underlying failure code.

--*/
{
    NTSTATUS ntStatus;
    WDFDEVICE device;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DMF_CONFIG_DeviceInterfaceMultipleTarget* moduleConfig;
    WDF_IO_TARGET_OPEN_PARAMS openParams;
    WDF_OBJECT_ATTRIBUTES targetAttributes;
    DeviceInterfaceMultipleTarget_IoTargetContext *targetContext;

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    device = DMF_ParentDeviceGet(DmfModule);

    moduleContext = DMF_CONTEXT_GET(DmfModule);
    DmfAssert(Target->IoTarget == NULL);

    moduleConfig = DMF_CONFIG_GET(DmfModule);

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&openParams,
                                                SymbolicLinkName,
                                                GENERIC_READ | GENERIC_WRITE);
    openParams.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
    openParams.EvtIoTargetQueryRemove = DeviceInterfaceMultipleTarget_EvtIoTargetQueryRemove;
    openParams.EvtIoTargetRemoveCanceled = DeviceInterfaceMultipleTarget_EvtIoTargetRemoveCanceled;
    openParams.EvtIoTargetRemoveComplete = DeviceInterfaceMultipleTarget_EvtIoTargetRemoveComplete;

    WDF_OBJECT_ATTRIBUTES_INIT(&targetAttributes);
    WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&targetAttributes,
                                           DeviceInterfaceMultipleTarget_IoTargetContext);
    targetAttributes.ParentObject = DmfModule;

    // Create an I/O target object.
    //
    ntStatus = WdfIoTargetCreate(device,
                                 &targetAttributes,
                                 &Target->IoTarget);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "WdfIoTargetCreate fails: ntStatus=%!STATUS!", ntStatus);
        goto Exit;
    }

    targetContext = WdfObjectGet_DeviceInterfaceMultipleTarget_IoTargetContext(Target->IoTarget);
    targetContext->DmfModuleDeviceInterfaceMultipleTarget = DmfModule;
    targetContext->Target = Target;

    ntStatus = WdfIoTargetOpen(Target->IoTarget,
                               &openParams);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "WdfIoTargetOpen fails: ntStatus=%!STATUS!", ntStatus);
        goto Exit;
    }

    ntStatus = DeviceInterfaceMultipleTarget_ContinuousRequestTargetCreate(DmfModule,
                                                                           Target);
    if (!NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DeviceInterfaceMultipleTarget_ContinuousRequestTargetCreate fails: ntStatus=%!STATUS!", ntStatus);
        goto Exit;
    }

    moduleContext->RequestSink_IoTargetSet(DmfModule,
                                           Target,
                                           Target->IoTarget);

    // Allow Methods to be called against the Target.
    //
    DMF_Rundown_Start(Target->DmfModuleRundown);

    if (moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChange != NULL)
    {
        DmfAssert(moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChangeEx == NULL);
        moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChange(DmfModule,
                                                                    Target->DmfIoTarget,
                                                                    DeviceInterfaceMultipleTarget_StateType_Open);
    }
    else if (moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChangeEx != NULL)
    {
        // This version allows Client to veto the open.
        //
        ntStatus = moduleConfig->EvtDeviceInterfaceMultipleTargetOnStateChangeEx(DmfModule,
                                                                                 Target->DmfIoTarget,
                                                                                 DeviceInterfaceMultipleTarget_StateType_Open);
    }

    // Handle is still created, it must not be set to NULL so devices can still send it requests.
    //
    DmfAssert(Target->IoTarget != NULL);

Exit:

    if (!NT_SUCCESS(ntStatus))
    {
        if (Target->IoTarget != NULL)
        {
            WdfObjectDelete(Target->IoTarget);
            Target->IoTarget = NULL;
        }
    }

    FuncExit(DMF_TRACE, "ntStatus=%!STATUS!", ntStatus);

    return ntStatus;
}
#pragma code_seg()

#pragma code_seg("PAGE")
_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
static
NTSTATUS
DeviceInterfaceMultipleTarget_InitializeIoTargetIfNeeded(
    _In_ DMFMODULE DmfModule,
    _In_ PUNICODE_STRING SymbolicLinkName
    )
/*++

Routine Description:

    Ask client if target device identified by the given device name should be opened.
    If yes, initialize the target device.

Arguments:

    DmfModule - The given Parent Module.
    SymbolicLinkName - The given device name.

Return Value:

    STATUS_SUCCESS or underlying failure code.

--*/
{
    NTSTATUS ntStatus;
    WDFDEVICE device;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DMF_CONFIG_DeviceInterfaceMultipleTarget* moduleConfig;
    DeviceInterfaceMultipleTarget_EnumerationContext enumerationCallbackContext;
    BOOLEAN ioTargetOpen;
    DeviceInterfaceMultipleTarget_IoTarget* target;

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    device = DMF_ParentDeviceGet(DmfModule);
    moduleContext = DMF_CONTEXT_GET(DmfModule);
    moduleConfig = DMF_CONFIG_GET(DmfModule);
    // By default, always open the Target.
    //
    ioTargetOpen = TRUE;
    ntStatus = STATUS_SUCCESS;
    target = NULL;

    enumerationCallbackContext.ContextData = (VOID*)SymbolicLinkName;
    enumerationCallbackContext.RemoveBuffer = FALSE;
    enumerationCallbackContext.BufferFound = FALSE;
    DMF_BufferQueue_Enumerate(moduleContext->DmfModuleBufferQueue,
                              DeviceInterfaceMultipleTarget_FindSymbolicLink,
                              &enumerationCallbackContext,
                              NULL,
                              NULL);
    if (enumerationCallbackContext.BufferFound)
    {
        // Interface already part of buffer queue.
        // TODO: Can we return STATUS_SUCCESS?
        //
        TraceEvents(TRACE_LEVEL_WARNING, DMF_TRACE, "Duplicate Arrival Interface Notification. Do Nothing");
        goto Exit;
    }

    if (moduleConfig->EvtDeviceInterfaceMultipleTargetOnPnpNotification != NULL)
    {
        // Ask client if this IoTarget needs to be opened.
        //
        moduleConfig->EvtDeviceInterfaceMultipleTargetOnPnpNotification(DmfModule,
                                                                        SymbolicLinkName,
                                                                        &ioTargetOpen);
    }

    if (ioTargetOpen)
    {
        WDF_OBJECT_ATTRIBUTES objectAttributes;
        WDFMEMORY dmfIoTargetMemory;

        ntStatus = DMF_BufferQueue_Fetch(moduleContext->DmfModuleBufferQueue,
                                         (VOID **)&target,
                                         NULL);
        if (! NT_SUCCESS(ntStatus))
        {
            TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_BufferQueue_Fetch() fails: ntStatus=%!STATUS!", ntStatus);
            goto Exit;
        }

        // Clear memory because it may not have been cleared in case of reuse.
        //
        RtlZeroMemory(target,
                      sizeof(DeviceInterfaceMultipleTarget_IoTarget));

        WDF_OBJECT_ATTRIBUTES_INIT(&objectAttributes);
        objectAttributes.ParentObject = DmfModule;

        ntStatus = WdfMemoryCreatePreallocated(&objectAttributes,
                                               (VOID *)target,
                                               sizeof(DeviceInterfaceMultipleTarget_IoTarget),
                                               &dmfIoTargetMemory);
        if (! NT_SUCCESS(ntStatus))
        {
            TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "WdfMemoryCreatePreallocated() fails: ntStatus=%!STATUS!", ntStatus);
            goto Exit;
        }

        target->DmfIoTarget = (DeviceInterfaceMultipleTarget_Target)dmfIoTargetMemory;

        // Iotarget will be opened. Save symbolic link name to make sure removal is referenced to correct interface.
        //
        ntStatus = DeviceInterfaceMultipleTarget_SymbolicLinkNameStore(DmfModule,
                                                                       target,
                                                                       SymbolicLinkName);
        if (! NT_SUCCESS(ntStatus))
        {
            TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DeviceInterfaceMultipleTarget_SymbolicLinkNameStore() fails: ntStatus=%!STATUS!", ntStatus);
            goto Exit;
        }

        ntStatus = DeviceInterfaceMultipleTarget_ModuleOpenIfNoOpenTargets(DmfModule);
        if (!NT_SUCCESS(ntStatus))
        {
            TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DeviceInterfaceMultipleTarget_ModuleOpenIfNoOpenTargets() fails: ntStatus=%!STATUS!", ntStatus);
            goto Exit;
        }

        // Create and open the underlying target.
        //
        ntStatus = DeviceInterfaceMultipleTarget_DeviceCreateNewIoTargetByName(DmfModule,
                                                                               target,
                                                                               SymbolicLinkName);
        if (! NT_SUCCESS(ntStatus))
        {
            // TODO: Display SymbolicLinkName.
            //
            DeviceInterfaceMultipleTarget_ModuleCloseIfNoOpenTargets(DmfModule);
            TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DeviceInterfaceMultipleTarget_DeviceCreateNewIoTargetByName() fails: ntStatus=%!STATUS!", ntStatus);
            goto Exit;
        }

        if (moduleContext->ContinuousRequestTargetMode == ContinuousRequestTarget_Mode_Automatic)
        {
            // By calling this function here, callbacks at the Client will happen only after the Module is open.
            //
            DmfAssert(target->DmfModuleRequestTarget != NULL);
            ntStatus = DMF_ContinuousRequestTarget_Start(target->DmfModuleRequestTarget);
            if (!NT_SUCCESS(ntStatus))
            {
                DeviceInterfaceMultipleTarget_ModuleCloseIfNoOpenTargets(DmfModule);
                TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_ContinuousRequestTarget_Start fails: ntStatus=%!STATUS!", ntStatus);
                goto Exit;
            }
        }

        // Target was successfully created.
        // Enqueue target buffer into consumer pool. 
        // 
        DMF_BufferQueue_Enqueue(moduleContext->DmfModuleBufferQueue,
                                (VOID *)target);
    }

Exit:

    if (! NT_SUCCESS(ntStatus))
    {
        if (target != NULL)
        {
            DeviceInterfaceMultipleTarget_TargetDestroy(DmfModule,
                                                        target);
        }
    }

    FuncExit(DMF_TRACE, "ntStatus=%!STATUS!", ntStatus);

    return ntStatus;
}
#pragma code_seg()

#pragma code_seg("PAGE")
_IRQL_requires_max_(PASSIVE_LEVEL)
static
VOID
DeviceInterfaceMultipleTarget_UninitializeIoTargetIfNeeded(
    _In_ DMFMODULE DmfModule,
    _In_ PUNICODE_STRING SymbolicLinkName
    )
/*++

Routine Description:

    Check if target device identified by the given device name is opened.
    If yes, unintitialize the target device.

Arguments:

    DmfModule - The given Parent Module.
    SymbolicLinkName - The given device name.

Return Value:

    None.

--*/
{
    WDFDEVICE device;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DeviceInterfaceMultipleTarget_EnumerationContext enumerationCallbackContext;
    DeviceInterfaceMultipleTarget_IoTarget* target;

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    device = DMF_ParentDeviceGet(DmfModule);
    moduleContext = DMF_CONTEXT_GET(DmfModule);

    enumerationCallbackContext.ContextData = (VOID*)SymbolicLinkName;
    enumerationCallbackContext.RemoveBuffer = TRUE;
    enumerationCallbackContext.BufferFound = FALSE;
    DMF_BufferQueue_Enumerate(moduleContext->DmfModuleBufferQueue,
                              DeviceInterfaceMultipleTarget_FindSymbolicLink,
                              &enumerationCallbackContext,
                              (VOID **)&target,
                              NULL);

    if (enumerationCallbackContext.BufferFound)
    {
        DmfAssert(target != NULL);
        DeviceInterfaceMultipleTarget_TargetDestroyAndCloseModule(DmfModule,
                                                                  target);
    }

    FuncExitVoid(DMF_TRACE);
}
#pragma code_seg()

#pragma code_seg("PAGE")
_IRQL_requires_max_(PASSIVE_LEVEL)
static
VOID
DeviceInterfaceMultipleTarget_NotificationUnregisterCleanup(
    _In_ DMFMODULE DmfModule
    )
/*++

Routine Description:

    Upon notification unregister, cleanup all the targets which were not removed and uninitialized.

Arguments:

    DmfModule - The given Parent Module.

Return Value:

    STATUS_SUCCESS or underlying failure code.

--*/
{
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DeviceInterfaceMultipleTarget_IoTarget* target;
    LONG targetCount;

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    // Already unregistered from PnP notification.
    // Clean the buffer queue here since the notifications callback will no longer be called.
    //
    while ((targetCount = DMF_BufferQueue_Count(moduleContext->DmfModuleBufferQueue)) != 0)
    {
        // NOTE: targetCount may not equal moduleContext->NumberOfTargetsOpened if WDFIOTARGET
        //       failed to reopen during RemoveCancel. Thus, the number of contexts may not 
        //       equal the number of targets opened.
        //
        DMF_BufferQueue_Dequeue(moduleContext->DmfModuleBufferQueue,
                                (VOID**)&target,
                                NULL);
        DeviceInterfaceMultipleTarget_TargetDestroyAndCloseModule(DmfModule,
                                                                  target);
    }
    // NOTE: This number can be less than zero if target failed to reopen during RemoveCancel.
    //       Reset to zero for case where PrepareHardware happens after ReleaseHardware.
    //
    DmfAssert(moduleContext->NumberOfTargetsOpened <= 0);
    moduleContext->NumberOfTargetsOpened = 0;

    FuncExitVoid(DMF_TRACE);
}
#pragma code_seg()

#if defined(DMF_USER_MODE)

#pragma code_seg("PAGE")
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
static
NTSTATUS
DeviceInterfaceMultipleTarget_InitializeTargets(
    _In_ DMFMODULE DmfModule
    )
/*++

Routine Description:

    Opens a handle to the Target device if available

Arguments:

    DmfModule - This Module's handle.

Return Value:

    None

--*/
{
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DMF_CONFIG_DeviceInterfaceMultipleTarget* moduleConfig;
    DWORD cmListSize;
    WCHAR *bufferPointer;
    UNICODE_STRING unitargetName;
    NTSTATUS ntStatus;
    CONFIGRET configRet;
    PWSTR currentInterface;
    ULONG currentInterfaceLength;

    PAGED_CODE();

    ntStatus = STATUS_SUCCESS;
    bufferPointer = NULL;

    moduleContext = DMF_CONTEXT_GET(DmfModule);
    moduleConfig = DMF_CONFIG_GET(DmfModule);

    configRet = CM_Get_Device_Interface_List_Size(&cmListSize,
                                                  &moduleConfig->DeviceInterfaceMultipleTargetGuid,
                                                  NULL,
                                                  CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (configRet != CR_SUCCESS)
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "CM_Get_Device_Interface_List_Size fails: configRet=0x%x", configRet);
        ntStatus = ERROR_NOT_FOUND;
        goto Exit;
    }

    bufferPointer = (WCHAR*)malloc(cmListSize * sizeof(WCHAR));
    if (bufferPointer != NULL)
    {
        configRet = CM_Get_Device_Interface_List(&moduleConfig->DeviceInterfaceMultipleTargetGuid,
                                                 NULL,
                                                 bufferPointer,
                                                 cmListSize,
                                                 CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (configRet != CR_SUCCESS)
        {
            TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "CM_Get_Device_Interface_List fails: configRet=0x%x", configRet);
            ntStatus = ERROR_NOT_FOUND;
            goto Exit;
        }

        // Enumerate devices of this interface class
        //
        currentInterface = bufferPointer;
        currentInterfaceLength = (ULONG)wcsnlen(currentInterface, cmListSize);
        while ((currentInterfaceLength < cmListSize) && (*currentInterface != UNICODE_NULL))
        {
            RtlInitUnicodeString(&unitargetName,
                                 bufferPointer);
            DeviceInterfaceMultipleTarget_InitializeIoTargetIfNeeded(DmfModule,
                                                                     &unitargetName);

            currentInterfaceLength = (ULONG)wcsnlen(currentInterface, cmListSize);
            currentInterface += currentInterfaceLength + 1;
        }
    }
    else
    {
        ntStatus = ERROR_NOT_ENOUGH_MEMORY;
    }

Exit:
    if (bufferPointer != NULL)
    {
        free(bufferPointer);
    }

    return ntStatus;
}
#pragma code_seg()

#pragma code_seg("PAGE")
DWORD
DeviceInterfaceMultipleTarget_UserNotificationCallback(
    _In_ HCMNOTIFICATION hNotify,
    _In_opt_ VOID* Context,
    _In_ CM_NOTIFY_ACTION Action,
    _In_reads_bytes_(EventDataSize) PCM_NOTIFY_EVENT_DATA EventData,
    _In_ DWORD EventDataSize
    )
/*++

Routine Description:

    Callback called when the notification that is registered detects an arrival or
    removal of an instance of a registered device. This function determines if the instance
    of the device is the proper device to open, and if so, opens it.

Arguments:

    Context - This Module's handle.

Return Value:

    None

--*/
{
    DMFMODULE dmfModule;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DMF_CONFIG_DeviceInterfaceMultipleTarget* moduleConfig;
    NTSTATUS ntStatus;
    UNICODE_STRING symbolickLink;

    UNREFERENCED_PARAMETER(hNotify);
    UNREFERENCED_PARAMETER(EventData);
    UNREFERENCED_PARAMETER(EventDataSize);

    ntStatus = STATUS_SUCCESS;

    dmfModule = DMFMODULEVOID_TO_MODULE(Context);
    moduleContext = DMF_CONTEXT_GET(dmfModule);

    moduleConfig = DMF_CONFIG_GET(dmfModule);

    if (Action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL)
    {
        if (EventData->FilterType == CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE)
        {
            RtlInitUnicodeString(&symbolickLink,
                                 EventData->u.DeviceInterface.SymbolicLink);
            DeviceInterfaceMultipleTarget_InitializeIoTargetIfNeeded(dmfModule,
                                                                     &symbolickLink);
        }
    }
    else if (Action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)
    {
        if (EventData->FilterType == CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE)
        {
            RtlInitUnicodeString(&symbolickLink,
                                 EventData->u.DeviceInterface.SymbolicLink);
            DeviceInterfaceMultipleTarget_UninitializeIoTargetIfNeeded(dmfModule,
                                                                       &symbolickLink);
        }
    }

    return (DWORD)ntStatus;
}
#pragma code_seg()

#endif // defined(DMF_USER_MODE)

#if !defined(DMF_USER_MODE)

#pragma code_seg("PAGE")
_Function_class_(DRIVER_NOTIFICATION_CALLBACK_ROUTINE)
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
static
NTSTATUS
DeviceInterfaceMultipleTarget_InterfaceArrivalCallback(
    _In_ VOID* NotificationStructure,
    _Inout_opt_ VOID* Context
    )
/*++

Routine Description:

    Callback called when the notification that is registered detects an arrival or
    removal of an instance of a registered device. This function determines if the instance
    of the device is the proper device to open, and if so, opens it.

Arguments:

    Context - This Module's handle.

Return Value:

    None

--*/
{
    DEVICE_INTERFACE_CHANGE_NOTIFICATION* deviceInterfaceChangeNotification;
    DMFMODULE dmfModule;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DMF_CONFIG_DeviceInterfaceMultipleTarget* moduleConfig;
    DeviceInterfaceMultipleTarget_IoTarget* target;

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    // warning C6387: 'Context' could be '0'.
    //
    #pragma warning(suppress:6387)
    dmfModule = DMFMODULEVOID_TO_MODULE(Context);
    DmfAssert(dmfModule != NULL);

    moduleContext = DMF_CONTEXT_GET(dmfModule);
    moduleConfig = DMF_CONFIG_GET(dmfModule);
    target = NULL;

    deviceInterfaceChangeNotification = (PDEVICE_INTERFACE_CHANGE_NOTIFICATION)NotificationStructure;

    TraceEvents(TRACE_LEVEL_VERBOSE, DMF_TRACE, "Found device: %S", deviceInterfaceChangeNotification->SymbolicLinkName->Buffer);

    if (DMF_Utility_IsEqualGUID((LPGUID)&(deviceInterfaceChangeNotification->Event),
                                (LPGUID)&GUID_DEVICE_INTERFACE_ARRIVAL))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "Arrival Interface Notification.");

        DeviceInterfaceMultipleTarget_InitializeIoTargetIfNeeded(dmfModule,
                                                                 deviceInterfaceChangeNotification->SymbolicLinkName);
    }
    else if (DMF_Utility_IsEqualGUID((LPGUID)&(deviceInterfaceChangeNotification->Event),
                                     (LPGUID)&GUID_DEVICE_INTERFACE_REMOVAL))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "Removal Interface Notification.");

        DeviceInterfaceMultipleTarget_UninitializeIoTargetIfNeeded(dmfModule,
                                                                   deviceInterfaceChangeNotification->SymbolicLinkName);
    }
    else
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "Invalid Notification. GUID=%!GUID!", &deviceInterfaceChangeNotification->Event);
        DmfAssert(FALSE);
    }

    FuncExit(DMF_TRACE, "ntStatus=%!STATUS!", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}
#pragma code_seg()

#endif // !defined(DMF_USER_MODE)

///////////////////////////////////////////////////////////////////////////////////////////////////////
// WDF Module Callbacks
///////////////////////////////////////////////////////////////////////////////////////////////////////
//

///////////////////////////////////////////////////////////////////////////////////////////////////////
// DMF Module Callbacks
///////////////////////////////////////////////////////////////////////////////////////////////////////
//

#if defined(DMF_USER_MODE)

#pragma code_seg("PAGE")
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
static
NTSTATUS
DMF_DeviceInterfaceMultipleTarget_NotificationRegisterUser(
    _In_ DMFMODULE DmfModule
    )
/*++

Routine Description:

    This callback is called when the Module Open Flags indicate that the this Module
    is opened after an asynchronous notification has happened.
    (DMF_MODULE_OPEN_OPTION_NOTIFY_PrepareHardware or DMF_MODULE_OPEN_OPTION_NOTIFY_D0Entry)
    This callback registers the notification.

Arguments:

    DmfModule - The given DMF Module.

Return Value:

    STATUS_SUCCESS

--*/
{
    NTSTATUS ntStatus;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DMF_CONFIG_DeviceInterfaceMultipleTarget* moduleConfig;
    CM_NOTIFY_FILTER cmNotifyFilter;
    CONFIGRET configRet;

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    ntStatus = STATUS_SUCCESS;
    moduleContext = DMF_CONTEXT_GET(DmfModule);
    moduleConfig = DMF_CONFIG_GET(DmfModule);

    // This function should not be not called twice.
    //
    DmfAssert(NULL == moduleContext->DeviceInterfaceNotification);

    cmNotifyFilter.cbSize = sizeof(CM_NOTIFY_FILTER);
    cmNotifyFilter.Flags = 0;
    cmNotifyFilter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    cmNotifyFilter.u.DeviceInterface.ClassGuid = moduleConfig->DeviceInterfaceMultipleTargetGuid;

    configRet = CM_Register_Notification(&cmNotifyFilter,
                                         (VOID*)DmfModule,
                                         (PCM_NOTIFY_CALLBACK)DeviceInterfaceMultipleTarget_UserNotificationCallback,
                                         &(moduleContext->DeviceInterfaceNotification));

    // Target device might already be there. Try now.
    // 
    if (configRet == CR_SUCCESS)
    {
        DeviceInterfaceMultipleTarget_InitializeTargets(DmfModule);

        // Should always return success here since notification might be called back later.
        //
        ntStatus = STATUS_SUCCESS;
    }
    else
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "CM_Register_Notification fails: configRet=0x%x", configRet);

        // Just a catchall error. Trace event configret should point to what went wrong.
        //
        ntStatus = STATUS_NOT_FOUND;
    }

    FuncExit(DMF_TRACE, "ntStatus=%!STATUS!", ntStatus);

    return ntStatus;
}
#pragma code_seg()

_IRQL_requires_max_(PASSIVE_LEVEL)
static
VOID
DMF_DeviceInterfaceMultipleTarget_NotificationUnregisterUser(
    _In_ DMFMODULE DmfModule
    )
/*++

Routine Description:

    This function is called when the TargetDevice is removed. This closes the handle to the
    target device.

Arguments:

    DmfModule - The given DMF Module.

Return Value:

    NONE

--*/
{
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    CM_Unregister_Notification(moduleContext->DeviceInterfaceNotification);

    moduleContext->DeviceInterfaceNotification = NULL;

    DeviceInterfaceMultipleTarget_NotificationUnregisterCleanup(DmfModule);
}

#endif // defined(DMF_USER_MODE)

#if !defined(DMF_USER_MODE)

#pragma code_seg("PAGE")
_Function_class_(DMF_NotificationRegister)
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
static
NTSTATUS
DMF_DeviceInterfaceMultipleTarget_NotificationRegister(
    _In_ DMFMODULE DmfModule
    )
/*++

Routine Description:

    This callback is called when the Module Open Flags indicate that the this Module
    is opened after an asynchronous notification has happened.
    (DMF_MODULE_OPEN_OPTION_NOTIFY_PrepareHardware or DMF_MODULE_OPEN_OPTION_NOTIFY_D0Entry)
    This callback registers the notification.

Arguments:

    DmfModule - The given DMF Module.

Return Value:

    STATUS_SUCCESS

--*/
{
    NTSTATUS ntStatus;
    WDFDEVICE parentDevice;
    PDEVICE_OBJECT deviceObject;
    PDRIVER_OBJECT driverObject;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DMF_CONFIG_DeviceInterfaceMultipleTarget* moduleConfig;

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    moduleConfig = DMF_CONFIG_GET(DmfModule);

    // This function should not be not called twice.
    //
    DmfAssert(NULL == moduleContext->DeviceInterfaceNotification);

    parentDevice = DMF_ParentDeviceGet(DmfModule);
    DmfAssert(parentDevice != NULL);
    deviceObject = WdfDeviceWdmGetDeviceObject(parentDevice);
    DmfAssert(deviceObject != NULL);
    driverObject = deviceObject->DriverObject;

    ntStatus = IoRegisterPlugPlayNotification(EventCategoryDeviceInterfaceChange,
                                              PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
                                              (void*)&moduleConfig->DeviceInterfaceMultipleTargetGuid,
                                              driverObject,
                                              (PDRIVER_NOTIFICATION_CALLBACK_ROUTINE)DeviceInterfaceMultipleTarget_InterfaceArrivalCallback,
                                              (VOID*)DmfModule,
                                              &(moduleContext->DeviceInterfaceNotification));

    FuncExit(DMF_TRACE, "ntStatus=%!STATUS!", ntStatus);

    return ntStatus;
}
#pragma code_seg()
#endif // !defined(DMF_USER_MODE)

#if !defined(DMF_USER_MODE)

#pragma code_seg("PAGE")
_Function_class_(DMF_NotificationUnregister)
_IRQL_requires_max_(PASSIVE_LEVEL)
static
VOID
DMF_DeviceInterfaceMultipleTarget_NotificationUnregister(
    _In_ DMFMODULE DmfModule
    )
/*++

Routine Description:

    This callback is called when the Module Open Flags indicate that the this Module
    is opened after an asynchronous notification has happened.
    (DMF_MODULE_OPEN_OPTION_NOTIFY_PrepareHardware or DMF_MODULE_OPEN_OPTION_NOTIFY_D0Entry)
    This callback unregisters the notification that was previously registered.

Arguments:

    DmfModule - The given DMF Module.

Return Value:

    None

--*/
{
    NTSTATUS ntStatus;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;

    PAGED_CODE();

    FuncEntry(DMF_TRACE);


    ntStatus = STATUS_SUCCESS;

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    // The notification routine could be called after the IoUnregisterPlugPlayNotification method 
    // has returned which was undesirable. IoUnregisterPlugPlayNotificationEx prevents the 
    // notification routine from being called after IoUnregisterPlugPlayNotificationEx returns.
    //
    if (moduleContext->DeviceInterfaceNotification != NULL)
    {
        ntStatus = IoUnregisterPlugPlayNotificationEx(moduleContext->DeviceInterfaceNotification);
        if (! NT_SUCCESS(ntStatus))
        {
            DmfAssert(FALSE);
            TraceEvents(TRACE_LEVEL_VERBOSE,
                        DMF_TRACE,
                        "IoUnregisterPlugPlayNotificationEx fails: ntStatus=%!STATUS!",
                        ntStatus);
            goto Exit;
        }

        moduleContext->DeviceInterfaceNotification = NULL;

        DeviceInterfaceMultipleTarget_NotificationUnregisterCleanup(DmfModule);
    }
    else
    {
        // Allow caller to unregister notification even if it has not been registered.
        //
    }

Exit:

    FuncExit(DMF_TRACE, "ntStatus=%!STATUS!", ntStatus);
}
#pragma code_seg()

#endif // !defined(DMF_USER_MODE)


#pragma code_seg("PAGE")
_Function_class_(DMF_ChildModulesAdd)
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
DMF_DeviceInterfaceMultipleTarget_ChildModulesAdd(
    _In_ DMFMODULE DmfModule,
    _In_ DMF_MODULE_ATTRIBUTES* DmfParentModuleAttributes,
    _In_ PDMFMODULE_INIT DmfModuleInit
    )
/*++

Routine Description:

    Configure and add the required Child Modules to the given Parent Module.

Arguments:

    DmfModule - The given Parent Module.
    DmfParentModuleAttributes - Pointer to the parent DMF_MODULE_ATTRIBUTES structure.
    DmfModuleInit - Opaque structure to be passed to DMF_DmfModuleAdd.

Return Value:

    None

--*/
{
    DMF_MODULE_ATTRIBUTES moduleAttributes;
    DMF_CONFIG_BufferQueue moduleBufferQueueConfigList;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    // Save for Dynamic Module instantiation later.
    //
    moduleContext->PassiveLevel = DmfParentModuleAttributes->PassiveLevel;

    // BufferQueue
    // -----------
    //
    DMF_CONFIG_BufferQueue_AND_ATTRIBUTES_INIT(&moduleBufferQueueConfigList,
                                               &moduleAttributes);
    moduleBufferQueueConfigList.SourceSettings.EnableLookAside = TRUE;
    moduleBufferQueueConfigList.SourceSettings.BufferCount = 0;
    moduleBufferQueueConfigList.SourceSettings.BufferSize = sizeof(DeviceInterfaceMultipleTarget_IoTarget);
    moduleBufferQueueConfigList.SourceSettings.PoolType = NonPagedPoolNx;
    moduleAttributes.ClientModuleInstanceName = "DeviceInterfaceMultipleTargetBufferQueue";
    // BufferQueue is accessed in interface arrival callbacks, which needs to execute at PASSIVE_LEVEL 
    // because the symbolic link name buffer is allocated by another actor using PagedPool.
    //
    moduleAttributes.PassiveLevel = TRUE;
    DMF_DmfModuleAdd(DmfModuleInit,
                     &moduleAttributes,
                     WDF_NO_OBJECT_ATTRIBUTES,
                     &moduleContext->DmfModuleBufferQueue);

    FuncExitVoid(DMF_TRACE);
}
#pragma code_seg()

///////////////////////////////////////////////////////////////////////////////////////////////////////
// Public Calls by Client
///////////////////////////////////////////////////////////////////////////////////////////////////////
//

#pragma code_seg("PAGE")
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS
DMF_DeviceInterfaceMultipleTarget_Create(
    _In_ WDFDEVICE Device,
    _In_ DMF_MODULE_ATTRIBUTES* DmfModuleAttributes,
    _In_ WDF_OBJECT_ATTRIBUTES* ObjectAttributes,
    _Out_ DMFMODULE* DmfModule
    )
/*++

Routine Description:

    Create an instance of a DMF Module of type DeviceInterfaceMultipleTarget.

Arguments:

    Device - Client driver's WDFDEVICE object.
    DmfModuleAttributes - Opaque structure that contains parameters DMF needs to initialize the Module.
    ObjectAttributes - WDF object attributes for DMFMODULE.
    DmfModule - Address of the location where the created DMFMODULE handle is returned.

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS ntStatus;
    DMF_MODULE_DESCRIPTOR dmfModuleDescriptor_DeviceInterfaceMultipleTarget;
    DMF_CALLBACKS_DMF dmfCallbacksDmf_DeviceInterfaceMultipleTarget;
    DMF_CONFIG_DeviceInterfaceMultipleTarget* moduleConfig;
    DmfModuleOpenOption openOption;

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    moduleConfig = (DMF_CONFIG_DeviceInterfaceMultipleTarget*)DmfModuleAttributes->ModuleConfigPointer;

    DMF_CALLBACKS_DMF_INIT(&dmfCallbacksDmf_DeviceInterfaceMultipleTarget);
    dmfCallbacksDmf_DeviceInterfaceMultipleTarget.ChildModulesAdd = DMF_DeviceInterfaceMultipleTarget_ChildModulesAdd;
#if defined(DMF_USER_MODE)
    dmfCallbacksDmf_DeviceInterfaceMultipleTarget.DeviceNotificationRegister = DMF_DeviceInterfaceMultipleTarget_NotificationRegisterUser;
    dmfCallbacksDmf_DeviceInterfaceMultipleTarget.DeviceNotificationUnregister = DMF_DeviceInterfaceMultipleTarget_NotificationUnregisterUser;
#else
    dmfCallbacksDmf_DeviceInterfaceMultipleTarget.DeviceNotificationRegister = DMF_DeviceInterfaceMultipleTarget_NotificationRegister;
    dmfCallbacksDmf_DeviceInterfaceMultipleTarget.DeviceNotificationUnregister = DMF_DeviceInterfaceMultipleTarget_NotificationUnregister;
#endif // defined(DMF_USER_MODE)

    // DeviceInterfaceMultipleTarget supports multiple open option configurations. 
    // Choose the open option based on Module configuration. 
    //
    switch (moduleConfig->ModuleOpenOption)
    {
    case DeviceInterfaceMultipleTarget_PnpRegisterWhen_PrepareHardware:
        openOption = DMF_MODULE_OPEN_OPTION_NOTIFY_PrepareHardware;
        break;
    case DeviceInterfaceMultipleTarget_PnpRegisterWhen_D0Entry:
        openOption = DMF_MODULE_OPEN_OPTION_NOTIFY_D0Entry;
        break;
    case DeviceInterfaceMultipleTarget_PnpRegisterWhen_Create:
        openOption = DMF_MODULE_OPEN_OPTION_NOTIFY_Create;
        break;
    default:
        DmfAssert(FALSE);
        openOption = DMF_MODULE_OPEN_OPTION_Invalid;
        break;
    }

    DMF_MODULE_DESCRIPTOR_INIT_CONTEXT_TYPE(dmfModuleDescriptor_DeviceInterfaceMultipleTarget,
                                            DeviceInterfaceMultipleTarget,
                                            DMF_CONTEXT_DeviceInterfaceMultipleTarget,
                                            DMF_MODULE_OPTIONS_DISPATCH_MAXIMUM,
                                            openOption);

    dmfModuleDescriptor_DeviceInterfaceMultipleTarget.CallbacksDmf = &dmfCallbacksDmf_DeviceInterfaceMultipleTarget;

    ntStatus = DMF_ModuleCreate(Device,
                                DmfModuleAttributes,
                                ObjectAttributes,
                                &dmfModuleDescriptor_DeviceInterfaceMultipleTarget,
                                DmfModule);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_ModuleCreate fails: ntStatus=%!STATUS!", ntStatus);
    }

    FuncExit(DMF_TRACE, "ntStatus=%!STATUS!", ntStatus);

    return ntStatus;
}
#pragma code_seg()

// Module Methods
//

_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
NTSTATUS
DMF_DeviceInterfaceMultipleTarget_BufferPut(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_Target Target,
    _In_ VOID* ClientBuffer
    )
/*++

Routine Description:

    Add the output buffer back to OutputBufferPool.

Arguments:

    DmfModule - This Module's handle.
    ClientBuffer - The buffer to add to the list.
    NOTE: This must be a properly formed buffer that was created by this Module.

Return Value:

    STATUS_SUCCESS if a buffer is added to the list.
    Other NTSTATUS if there is an error.

--*/
{
    NTSTATUS ntStatus;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DeviceInterfaceMultipleTarget_IoTarget* target;

    FuncEntry(DMF_TRACE);

    DMFMODULE_VALIDATE_IN_METHOD(DmfModule,
                                 DeviceInterfaceMultipleTarget);

    ntStatus = DMF_ModuleReference(DmfModule);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_ModuleReference");
        goto Exit;
    }

    target = DeviceInterfaceMultipleTarget_BufferGet(Target);
    moduleContext = DMF_CONTEXT_GET(DmfModule);

    // Ensure Target structure is valid during the duration of this Method.
    //
    ntStatus = DMF_Rundown_Reference(target->DmfModuleRundown);
    if (! NT_SUCCESS(ntStatus))
    {
        DMF_ModuleDereference(DmfModule);
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_Rundown_Reference");
        goto Exit;
    }

    DmfAssert(moduleContext->ContinuousReaderMode);
    DMF_ContinuousRequestTarget_BufferPut(target->DmfModuleRequestTarget,
                                          ClientBuffer);

    DMF_Rundown_Dereference(target->DmfModuleRundown);
    DMF_ModuleDereference(DmfModule);

Exit:

    FuncExit(DMF_TRACE, "ntStatus=%!STATUS!", ntStatus);

    return ntStatus;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
BOOLEAN
DMF_DeviceInterfaceMultipleTarget_Cancel(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_Target Target,
    _In_ RequestTarget_DmfRequest DmfRequestId
    )
/*++

Routine Description:

    Cancels a given WDFREQUEST associated with DmfRequestId that has been sent to a given Target.

Arguments:

    DmfModule - This Module's handle.
    Target - The given Target.
    DmfRequestId - The given DmfRequestId.

Return Value:

    TRUE if the given WDFREQUEST was has been canceled.
    FALSE if the given WDFREQUEST is not canceled because it has already been completed or deleted.

--*/
{
    BOOLEAN returnValue;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    NTSTATUS ntStatus;
    DeviceInterfaceMultipleTarget_IoTarget* target;

    FuncEntry(DMF_TRACE);

    DMFMODULE_VALIDATE_IN_METHOD(DmfModule,
                                 DeviceInterfaceMultipleTarget);

    ntStatus = DMF_ModuleReference(DmfModule);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_ModuleReference");
        returnValue = FALSE;
        goto Exit;
    }

    target = DeviceInterfaceMultipleTarget_BufferGet(Target);
    moduleContext = DMF_CONTEXT_GET(DmfModule);

    // Ensure Target structure is valid during the duration of this Method.
    //
    ntStatus = DMF_Rundown_Reference(target->DmfModuleRundown);
    if (! NT_SUCCESS(ntStatus))
    {
        DMF_ModuleDereference(DmfModule);
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_Rundown_Reference");
        returnValue = FALSE;
        goto Exit;
    }

    // Needs to be checked after rundown check.
    //
    DmfAssert(target->IoTarget != NULL);
    returnValue = moduleContext->RequestSink_Cancel(DmfModule,
                                                    target,
                                                    DmfRequestId);

    DMF_Rundown_Dereference(target->DmfModuleRundown);
    DMF_ModuleDereference(DmfModule);

Exit:

    return returnValue;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
NTSTATUS
DMF_DeviceInterfaceMultipleTarget_Get(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_Target Target,
    _Out_ WDFIOTARGET* IoTarget
    )
/*++

Routine Description:

    Get the IoTarget to Send Requests to.

Arguments:

    DmfModule - This Module's handle.
    IoTarget - IO Target.

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS ntStatus;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DeviceInterfaceMultipleTarget_IoTarget* target;

    FuncEntry(DMF_TRACE);

    DmfAssert(IoTarget != NULL);
    *IoTarget = NULL;

    DMFMODULE_VALIDATE_IN_METHOD(DmfModule,
                                 DeviceInterfaceMultipleTarget);

    ntStatus = DMF_ModuleReference(DmfModule);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_ModuleReference");
        goto Exit;
    }

    target = DeviceInterfaceMultipleTarget_BufferGet(Target);
    moduleContext = DMF_CONTEXT_GET(DmfModule);

    // Ensure Target structure is valid during the duration of this Method.
    // This is here for consistency's sake. It also ensures Client never receives
    // a NULL target.
    //
    ntStatus = DMF_Rundown_Reference(target->DmfModuleRundown);
    if (! NT_SUCCESS(ntStatus))
    {
        DMF_ModuleDereference(DmfModule);
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_Rundown_Reference");
        goto Exit;
    }

    // It will only be NULL if Module is closed or closing due to rundown protection.
    //
    DmfAssert(target->IoTarget != NULL);
    *IoTarget = target->IoTarget;

    DMF_Rundown_Dereference(target->DmfModuleRundown);
    DMF_ModuleDereference(DmfModule);

Exit:

    FuncExitVoid(DMF_TRACE);

    return ntStatus;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
NTSTATUS
DMF_DeviceInterfaceMultipleTarget_GuidGet(
    _In_ DMFMODULE DmfModule,
    _Out_ GUID* Guid
    )
/*++

Routine Description:

    The device interface GUID associated with this Module's WDFIOTARGET.

Arguments:

    DmfModule - This Module's handle.
    Guid - The device interface GUID associated with this Module's WDFIOTARGET.

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS ntStatus;
    DMF_CONFIG_DeviceInterfaceMultipleTarget* moduleConfig;

    FuncEntry(DMF_TRACE);

    DMFMODULE_VALIDATE_IN_METHOD(DmfModule,
                                 DeviceInterfaceMultipleTarget);

    DmfAssert(Guid != NULL);

    ntStatus = STATUS_SUCCESS;
    moduleConfig = DMF_CONFIG_GET(DmfModule);

    *Guid = moduleConfig->DeviceInterfaceMultipleTargetGuid;

    FuncExit(DMF_TRACE, "ntStatus=%!STATUS!", ntStatus);

    return ntStatus;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
NTSTATUS
DMF_DeviceInterfaceMultipleTarget_Send(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_Target Target,
    _In_reads_bytes_opt_(RequestLength) VOID* RequestBuffer,
    _In_ size_t RequestLength,
    _Out_writes_bytes_opt_(ResponseLength) VOID* ResponseBuffer,
    _In_ size_t ResponseLength,
    _In_ ContinuousRequestTarget_RequestType RequestType,
    _In_ ULONG RequestIoctl,
    _In_ ULONG RequestTimeoutMilliseconds,
    _In_opt_ EVT_DMF_ContinuousRequestTarget_SendCompletion* EvtContinuousRequestTargetSingleAsynchronousRequest,
    _In_opt_ VOID* SingleAsynchronousRequestClientContext
    )
/*++

Routine Description:

    Creates and sends an Asynchronous request to the IoTarget given a buffer, IOCTL and other information.

Arguments:

    DmfModule - This Module's handle.
    RequestBuffer - Buffer of data to attach to request to be sent.
    RequestLength - Number of bytes to in RequestBuffer to send.
    ResponseBuffer - Buffer of data that is returned by the request.
    ResponseLength - Size of Response Buffer in bytes.
    RequestType - Read or Write or Ioctl
    RequestIoctl - The given IOCTL.
    RequestTimeoutMilliseconds - Timeout value in milliseconds of the transfer or zero for no timeout.
    EvtContinuousRequestTargetSingleAsynchronousRequest - Callback to be called in completion routine.
    SingleAsynchronousRequestClientContext - Client context sent in callback

Return Value:

    STATUS_SUCCESS if a buffer is added to the list.
    Other NTSTATUS if there is an error.

--*/
{
    NTSTATUS ntStatus;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DeviceInterfaceMultipleTarget_IoTarget* target;

    FuncEntry(DMF_TRACE);

    // This Module Method can be called when SSH is removed or being removed. The code in this function is 
    // protected due to call to ModuleAcquire.
    //
    DMFMODULE_VALIDATE_IN_METHOD(DmfModule,
                                 DeviceInterfaceMultipleTarget);

    ntStatus = DMF_ModuleReference(DmfModule);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_ModuleReference");
        goto Exit;
    }

    target = DeviceInterfaceMultipleTarget_BufferGet(Target);
    moduleContext = DMF_CONTEXT_GET(DmfModule);

    ntStatus = DMF_Rundown_Reference(target->DmfModuleRundown);
    if (! NT_SUCCESS(ntStatus))
    {
        DMF_ModuleDereference(DmfModule);
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_Rundown_Reference");
        goto Exit;
    }

    DmfAssert(target->IoTarget != NULL);
    // This assert will fail if the Target is valid the it is sent to a wrong Module instance
    // that is not properly initialized.
    //
    DmfAssert(moduleContext->RequestSink_Send != NULL);
    ntStatus = moduleContext->RequestSink_Send(DmfModule,
                                               target,
                                               RequestBuffer,
                                               RequestLength,
                                               ResponseBuffer,
                                               ResponseLength,
                                               RequestType,
                                               RequestIoctl,
                                               RequestTimeoutMilliseconds,
                                               EvtContinuousRequestTargetSingleAsynchronousRequest,
                                               SingleAsynchronousRequestClientContext);

    DMF_Rundown_Dereference(target->DmfModuleRundown);
    DMF_ModuleDereference(DmfModule);

Exit:

    FuncExit(DMF_TRACE, "ntStatus=%!STATUS!", ntStatus);

    return ntStatus;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
NTSTATUS
DMF_DeviceInterfaceMultipleTarget_SendEx(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_Target Target,
    _In_reads_bytes_opt_(RequestLength) VOID* RequestBuffer,
    _In_ size_t RequestLength,
    _Out_writes_bytes_opt_(ResponseLength) VOID* ResponseBuffer,
    _In_ size_t ResponseLength,
    _In_ ContinuousRequestTarget_RequestType RequestType,
    _In_ ULONG RequestIoctl,
    _In_ ULONG RequestTimeoutMilliseconds,
    _In_opt_ EVT_DMF_ContinuousRequestTarget_SendCompletion* EvtContinuousRequestTargetSingleAsynchronousRequest,
    _In_opt_ VOID* SingleAsynchronousRequestClientContext,
    _Out_opt_ RequestTarget_DmfRequest* DmfRequestId
    )
/*++

Routine Description:

    Creates and sends an Asynchronous request to the IoTarget given a buffer, IOCTL and other information.

Arguments:

    DmfModule - This Module's handle.
    RequestBuffer - Buffer of data to attach to request to be sent.
    RequestLength - Number of bytes to in RequestBuffer to send.
    ResponseBuffer - Buffer of data that is returned by the request.
    ResponseLength - Size of Response Buffer in bytes.
    RequestType - Read or Write or Ioctl
    RequestIoctl - The given IOCTL.
    RequestTimeoutMilliseconds - Timeout value in milliseconds of the transfer or zero for no timeout.
    EvtContinuousRequestTargetSingleAsynchronousRequest - Callback to be called in completion routine.
    SingleAsynchronousRequestClientContext - Client context sent in callback

Return Value:

    STATUS_SUCCESS if a buffer is added to the list.
    Other NTSTATUS if there is an error.

--*/
{
    NTSTATUS ntStatus;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DeviceInterfaceMultipleTarget_IoTarget* target;

    FuncEntry(DMF_TRACE);

    // This Module Method can be called when SSH is removed or being removed. The code in this function is 
    // protected due to call to ModuleAcquire.
    //
    DMFMODULE_VALIDATE_IN_METHOD(DmfModule,
                                 DeviceInterfaceMultipleTarget);

    ntStatus = DMF_ModuleReference(DmfModule);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_ModuleReference");
        goto Exit;
    }

    target = DeviceInterfaceMultipleTarget_BufferGet(Target);
    moduleContext = DMF_CONTEXT_GET(DmfModule);

    ntStatus = DMF_Rundown_Reference(target->DmfModuleRundown);
    if (! NT_SUCCESS(ntStatus))
    {
        DMF_ModuleDereference(DmfModule);
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_Rundown_Reference");
        goto Exit;
    }

    DmfAssert(target->IoTarget != NULL);
    // This assert will fail if the Target is valid the it is sent to a wrong Module instance
    // that is not properly initialized.
    //
    DmfAssert(moduleContext->RequestSink_SendEx != NULL);
    ntStatus = moduleContext->RequestSink_SendEx(DmfModule,
                                                 target,
                                                 RequestBuffer,
                                                 RequestLength,
                                                 ResponseBuffer,
                                                 ResponseLength,
                                                 RequestType,
                                                 RequestIoctl,
                                                 RequestTimeoutMilliseconds,
                                                 EvtContinuousRequestTargetSingleAsynchronousRequest,
                                                 SingleAsynchronousRequestClientContext,
                                                 DmfRequestId);

    DMF_Rundown_Dereference(target->DmfModuleRundown);
    DMF_ModuleDereference(DmfModule);

Exit:

    FuncExit(DMF_TRACE, "ntStatus=%!STATUS!", ntStatus);

    return ntStatus;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
NTSTATUS
DMF_DeviceInterfaceMultipleTarget_SendSynchronously(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_Target Target,
    _In_reads_bytes_opt_(RequestLength) VOID* RequestBuffer,
    _In_ size_t RequestLength,
    _Out_writes_bytes_opt_(ResponseLength) VOID* ResponseBuffer,
    _In_ size_t ResponseLength,
    _In_ ContinuousRequestTarget_RequestType RequestType,
    _In_ ULONG RequestIoctl,
    _In_ ULONG RequestTimeoutMilliseconds,
    _Out_opt_ size_t* BytesWritten
    )
/*++

Routine Description:

    Creates and sends a synchronous request to the IoTarget given a buffer, IOCTL and other information.

Arguments:

    DmfModule - This Module's handle.
    RequestBuffer - Buffer of data to attach to request to be sent.
    RequestLength - Number of bytes to in RequestBuffer to send.
    ResponseBuffer - Buffer of data that is returned by the request.
    ResponseLength - Size of Response Buffer in bytes.
    RequestType - Read or Write or Ioctl
    RequestIoctl - The given IOCTL.
    RequestTimeoutMilliseconds - Timeout value in milliseconds of the transfer or zero for no timeout.
    BytesWritten - Bytes returned by the transaction.

Return Value:

    STATUS_SUCCESS if a buffer is added to the list.
    Other NTSTATUS if there is an error.

--*/
{
    NTSTATUS ntStatus;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DeviceInterfaceMultipleTarget_IoTarget* target;

    FuncEntry(DMF_TRACE);

    // This Module Method can be called when SSH is removed or being removed. The code in this function is 
    // protected due to call to ModuleAcquire.
    //
    DMFMODULE_VALIDATE_IN_METHOD(DmfModule,
                                 DeviceInterfaceMultipleTarget);

    ntStatus = DMF_ModuleReference(DmfModule);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_ModuleReference");
        goto Exit;
    }

    target = DeviceInterfaceMultipleTarget_BufferGet(Target);
    moduleContext = DMF_CONTEXT_GET(DmfModule);

    ntStatus = DMF_Rundown_Reference(target->DmfModuleRundown);
    if (! NT_SUCCESS(ntStatus))
    {
        DMF_ModuleDereference(DmfModule);
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_Rundown_Reference");
        goto Exit;
    }

    DmfAssert(target->IoTarget != NULL);

    ntStatus = moduleContext->RequestSink_SendSynchronously(DmfModule,
                                                            target,
                                                            RequestBuffer,
                                                            RequestLength,
                                                            ResponseBuffer,
                                                            ResponseLength,
                                                            RequestType,
                                                            RequestIoctl,
                                                            RequestTimeoutMilliseconds,
                                                            BytesWritten);

    DMF_Rundown_Dereference(target->DmfModuleRundown);
    DMF_ModuleDereference(DmfModule);

Exit:

    FuncExit(DMF_TRACE, "ntStatus=%!STATUS!", ntStatus);

    return ntStatus;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
NTSTATUS
DMF_DeviceInterfaceMultipleTarget_StreamStart(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_Target Target
    )
/*++

Routine Description:

    Starts streaming Asynchronous requests to the IoTarget.

Arguments:

    DmfModule - This Module's handle.

Return Value:

    STATUS_SUCCESS if a buffer is added to the list.
    Other NTSTATUS if there is an error.

--*/
{
    NTSTATUS ntStatus;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DeviceInterfaceMultipleTarget_IoTarget* target;

    FuncEntry(DMF_TRACE);

    DMFMODULE_VALIDATE_IN_METHOD(DmfModule,
                                 DeviceInterfaceMultipleTarget);

    ntStatus = DMF_ModuleReference(DmfModule);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_ModuleReference");
        goto Exit;
    }

    target = DeviceInterfaceMultipleTarget_BufferGet(Target);
    moduleContext = DMF_CONTEXT_GET(DmfModule);

    ntStatus = DMF_Rundown_Reference(target->DmfModuleRundown);
    if (! NT_SUCCESS(ntStatus))
    {
        DMF_ModuleDereference(DmfModule);
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_Rundown_Reference");
        goto Exit;
    }

    DmfAssert(target->IoTarget != NULL);

    DmfAssert(moduleContext->ContinuousReaderMode);
    ntStatus = DMF_ContinuousRequestTarget_Start(target->DmfModuleRequestTarget);

    DMF_Rundown_Dereference(target->DmfModuleRundown);
    DMF_ModuleDereference(DmfModule);

Exit:

    FuncExit(DMF_TRACE, "ntStatus=%!STATUS!", ntStatus);

    return ntStatus;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
DMF_DeviceInterfaceMultipleTarget_StreamStop(
    _In_ DMFMODULE DmfModule,
    _In_ DeviceInterfaceMultipleTarget_Target Target
    )
/*++

Routine Description:

    Stops streaming Asynchronous requests to the IoTarget and Cancels all the existing requests.

Arguments:

    DmfModule - This Module's handle.

Return Value:

    VOID.

--*/
{
    NTSTATUS ntStatus;
    DMF_CONTEXT_DeviceInterfaceMultipleTarget* moduleContext;
    DeviceInterfaceMultipleTarget_IoTarget* target;

    FuncEntry(DMF_TRACE);

    DMFMODULE_VALIDATE_IN_METHOD(DmfModule,
                                 DeviceInterfaceMultipleTarget);

    ntStatus = DMF_ModuleReference(DmfModule);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_ModuleReference");
        goto Exit;
    }

    target = DeviceInterfaceMultipleTarget_BufferGet(Target);
    moduleContext = DMF_CONTEXT_GET(DmfModule);

    ntStatus = DMF_Rundown_Reference(target->DmfModuleRundown);
    if (! NT_SUCCESS(ntStatus))
    {
        DMF_ModuleDereference(DmfModule);
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_Rundown_Reference");
        goto Exit;
    }

    DmfAssert(target->IoTarget != NULL);

    DmfAssert(moduleContext->ContinuousReaderMode);
    DMF_ContinuousRequestTarget_StopAndWait(target->DmfModuleRequestTarget);

    DMF_Rundown_Dereference(target->DmfModuleRundown);
    DMF_ModuleDereference(DmfModule);

Exit:

    FuncExitVoid(DMF_TRACE);
}

// eof: Dmf_DeviceInterfaceMultipleTarget.c
//
