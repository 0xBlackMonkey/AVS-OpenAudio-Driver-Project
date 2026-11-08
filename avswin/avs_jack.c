// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// avs_jack.c -- headphone jack presence from CoolStar's da7219.sys.
//
// da7219.sys (github.com/coolstar/da7219) does the jack detection itself
// (DA7219 AAD interrupt) and publishes the result on its own HID device,
// VID 0x2DCF / PID 0x7219, vendor collection (usage page 0xFF00, usage 4):
//   input  report 2: { 0x02, CONTROL_CODE_JACK_TYPE=0x01, JackType }
//                    JackType = SND_JACK_* bits: 0 unplugged,
//                    1 headphone, 3 headset (headphone + mic)
//   output report 2: { 0x02, any } -> it answers with the current JackType
// (report descriptor in da7219.h, Da7219WriteReport() in da7219.c).
//
// The HID collections of a minidriver on an ACPI device are named after the
// ACPI ID, not VID/PID: "\??\HID#DLGS7219&Col02#..." (DLGS7219 = the
// DA7219's ACPI _HID, same ID Linux's da7219 driver matches). So we match
// on the name (JackHidMatch, '|'-separated alternatives + JackHidCollection)
// and then confirm VID/PID with IOCTL_HID_GET_COLLECTION_INFORMATION.
//
// We watch for that HID interface, open it from kernel mode, ask for the
// current state once and then keep one read pending on our own IRP. Each
// change goes to the topology miniport (mintopo.cpp), which reports it to
// Windows as KSJACK_DESCRIPTION.IsConnected + KSEVENT_PINCAPS_JACKINFOCHANGE.

#include "driver.h"
#include <ntstrsafe.h>

#define AVS_JACK_POOL_TAG        'kJvA'

#define AVS_DA7219_REPORTID      0x02   // REPORTID_SPECKEYS
#define AVS_DA7219_JACK_TYPE     0x01   // CONTROL_CODE_JACK_TYPE
#define AVS_DA7219_IN_LEN        3      // report id + 2 data bytes
#define AVS_DA7219_OUT_LEN       2      // report id + 1 data byte
#define AVS_DA7219_VID           0x2DCF // DA7219_VID / DA7219_PID in hidcommon.h
#define AVS_DA7219_PID           0x7219

// hidclass.h: HID_BUFFER_CTL_CODE(106) = CTL_CODE(FILE_DEVICE_KEYBOARD, 106,
// METHOD_BUFFERED, FILE_ANY_ACCESS)
#define AVS_IOCTL_HID_GET_COLLECTION_INFORMATION  0x000B01A8

typedef struct _AVS_HID_COLLECTION_INFORMATION     // HID_COLLECTION_INFORMATION
{
    ULONG   DescriptorSize;
    BOOLEAN Polled;
    UCHAR   Reserved1[1];
    USHORT  VendorID;
    USHORT  ProductID;
    USHORT  VersionNumber;
} AVS_HID_COLLECTION_INFORMATION;

// GUID_DEVINTERFACE_HID {4D1E55B2-F16F-11CF-88CB-001111000030}
DEFINE_GUID(AVS_GUID_DEVINTERFACE_HID,
    0x4D1E55B2, 0xF16F, 0x11CF, 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30);
// GUID_DEVICE_INTERFACE_ARRIVAL {CB3A4004-46F0-11D0-B08F-00609713053F}
DEFINE_GUID(AVS_GUID_DEVICE_INTERFACE_ARRIVAL,
    0xCB3A4004, 0x46F0, 0x11D0, 0xB0, 0x8F, 0x00, 0x60, 0x97, 0x13, 0x05, 0x3F);

typedef struct _AVS_JACK_START
{
    PAVS_DEVICE_CONTEXT Ctx;
    UNICODE_STRING      Link;
    WCHAR               Buffer[1];
} AVS_JACK_START;

// ------------------------------------------------------------------ helpers

static VOID
AvsAsciiToUpperW(
    _In_ PCSTR In,
    _Out_writes_(Cch) PWCHAR Out,
    _In_ ULONG Cch
    )
{
    ULONG i = 0;

    for (; In[i] != '\0' && i + 1 < Cch; i++)
    {
        CHAR c = In[i];
        Out[i] = (WCHAR)((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c);
    }
    Out[i] = L'\0';
}

// Case-insensitive search of an upper-case NUL-terminated pattern.
static BOOLEAN
AvsLinkContains(
    _In_ PCUNICODE_STRING Link,
    _In_ PCWSTR Pattern
    )
{
    USHORT n = Link->Length / sizeof(WCHAR);
    USHORT m = 0;

    while (Pattern[m] != L'\0')
    {
        m++;
    }
    if (m == 0)
    {
        return TRUE;
    }
    for (USHORT i = 0; i + m <= n; i++)
    {
        USHORT j = 0;

        while (j < m && RtlUpcaseUnicodeChar(Link->Buffer[i + j]) == Pattern[j])
        {
            j++;
        }
        if (j == m)
        {
            return TRUE;
        }
    }
    return FALSE;
}

// Pattern list "A|B|C" (upper case): TRUE if the link contains any of them.
static BOOLEAN
AvsLinkContainsAny(
    _In_ PCUNICODE_STRING Link,
    _In_ PCWSTR Patterns
    )
{
    WCHAR one[48];
    ULONG n = 0;

    for (PCWSTR p = Patterns; ; p++)
    {
        if (*p == L'|' || *p == L'\0')
        {
            one[n] = L'\0';
            if (n > 0 && AvsLinkContains(Link, one))
            {
                return TRUE;
            }
            n = 0;
            if (*p == L'\0')
            {
                return FALSE;
            }
        }
        else if (n + 1 < ARRAYSIZE(one))
        {
            one[n++] = *p;
        }
    }
}

static VOID
AvsJackSetState(
    _In_ PAVS_DEVICE_CONTEXT ctx,
    _In_ LONG Value
    )
{
    LONG old = InterlockedExchange(&ctx->JackState, Value);

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: jack -- %s%s (da7219 JackType 0x%X, was 0x%X)\n",
        Value != 0 ? "PLUGGED" : "unplugged",
        (Value & 2) ? ", headset with mic" : "", Value, old));

    if (old == Value)
    {
        return;
    }

    // Headphones and headset mic follow the same plug (SND_JACK_* bits).
    ExAcquireFastMutex(&ctx->JackLock);
    for (ULONG i = 0; i < AVS_MAX_ENDPOINTS; i++)
    {
        if (ctx->Endpoints[i].Valid && AVS_EP_HAS_JACK(ctx->Endpoints[i].Info.Type) &&
            ctx->JackNotify[i] != NULL)
        {
            ctx->JackNotify[i](ctx->JackNotifyCtx[i]);
        }
    }
    ExReleaseFastMutex(&ctx->JackLock);
}

// ----------------------------------------------------------- reader thread

static IO_COMPLETION_ROUTINE AvsJackIoDone;

static NTSTATUS
AvsJackIoDone(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_reads_opt_(_Inexpressible_("varies")) PVOID Context
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;     // the IRP is ours, we reuse it
}

// One read or write on our own (reusable, cancellable) IRP. The buffer
// is non-paged and handed over the way the HID device expects it.
static NTSTATUS
AvsJackIo(
    _In_ PDEVICE_OBJECT Dev,
    _In_ PFILE_OBJECT File,
    _In_ PIRP Irp,
    _In_ UCHAR Major,
    _In_ PUCHAR Data,
    _In_ ULONG Length,
    _In_opt_ PMDL Mdl,
    _In_ PKEVENT Done,
    _Out_ PULONG_PTR Information
    )
{
    PIO_STACK_LOCATION sl;

    IoReuseIrp(Irp, STATUS_NOT_SUPPORTED);
    KeClearEvent(Done);

    Irp->MdlAddress = NULL;
    Irp->AssociatedIrp.SystemBuffer = NULL;
    Irp->UserBuffer = NULL;
    if (Dev->Flags & DO_DIRECT_IO)
    {
        Irp->MdlAddress = Mdl;
    }
    else if (Dev->Flags & DO_BUFFERED_IO)
    {
        Irp->AssociatedIrp.SystemBuffer = Data;
    }
    else
    {
        Irp->UserBuffer = Data;
    }
    Irp->RequestorMode = KernelMode;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Irp->Tail.Overlay.OriginalFileObject = File;

    sl = IoGetNextIrpStackLocation(Irp);
    sl->MajorFunction = Major;
    sl->FileObject = File;
    if (Major == IRP_MJ_READ)
    {
        sl->Parameters.Read.Length = Length;
        sl->Parameters.Read.ByteOffset.QuadPart = 0;
    }
    else
    {
        sl->Parameters.Write.Length = Length;
        sl->Parameters.Write.ByteOffset.QuadPart = 0;
    }
    IoSetCompletionRoutine(Irp, AvsJackIoDone, Done, TRUE, TRUE, TRUE);

    (VOID)IoCallDriver(Dev, Irp);
    KeWaitForSingleObject(Done, Executive, KernelMode, FALSE, NULL);

    *Information = Irp->IoStatus.Information;
    return Irp->IoStatus.Status;
}

static KSTART_ROUTINE AvsJackReader;

static VOID
AvsJackReader(
    _In_ PVOID StartContext
    )
{
    AVS_JACK_START* start = (AVS_JACK_START*)StartContext;
    PAVS_DEVICE_CONTEXT ctx = start->Ctx;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE handle = NULL;
    PFILE_OBJECT file = NULL;
    PDEVICE_OBJECT dev;
    PIRP irp = NULL;
    PUCHAR buf = NULL;
    PMDL mdlIn = NULL, mdlOut = NULL;
    KEVENT done;
    ULONG_PTR len = 0;
    NTSTATUS status;
    KIRQL irql;

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: jack -- opening %wZ\n", &start->Link));

    // A kernel handle kept open for the reader's lifetime (closing it would
    // send IRP_MJ_CLEANUP, after which hidclass rejects reads on the file).
    InitializeObjectAttributes(&oa, &start->Link, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = ZwCreateFile(&handle, FILE_READ_DATA | FILE_WRITE_DATA, &oa, &iosb, NULL, 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0, NULL, 0);
    if (!NT_SUCCESS(status))
    {
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: jack -- open failed: 0x%X\n", status));
        goto out;
    }
    status = ObReferenceObjectByHandle(handle, FILE_READ_DATA, *IoFileObjectType, KernelMode,
        (PVOID*)&file, NULL);
    if (!NT_SUCCESS(status))
    {
        goto out;
    }
    dev = IoGetRelatedDeviceObject(file);

    // Confirm it really is da7219.sys's HID device.
    {
        AVS_HID_COLLECTION_INFORMATION info;
        KEVENT ev;
        PIRP q;

        RtlZeroMemory(&info, sizeof(info));
        KeInitializeEvent(&ev, NotificationEvent, FALSE);
        q = IoBuildDeviceIoControlRequest(AVS_IOCTL_HID_GET_COLLECTION_INFORMATION, dev,
            NULL, 0, &info, sizeof(info), FALSE, &ev, &iosb);
        if (q != NULL)
        {
            IoGetNextIrpStackLocation(q)->FileObject = file;
            status = IoCallDriver(dev, q);
            if (status == STATUS_PENDING)
            {
                KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
                status = iosb.Status;
            }
            AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                "AVS: jack -- collection info 0x%X: VID %04X PID %04X ver %04X\n",
                status, info.VendorID, info.ProductID, info.VersionNumber));
            if (NT_SUCCESS(status) &&
                (info.VendorID != AVS_DA7219_VID || info.ProductID != AVS_DA7219_PID))
            {
                status = STATUS_DEVICE_CONFIGURATION_ERROR;
                goto out;
            }
        }
    }

    buf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, 16, AVS_JACK_POOL_TAG);
    irp = IoAllocateIrp(dev->StackSize, FALSE);
    if (buf == NULL || irp == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }
    if (dev->Flags & DO_DIRECT_IO)
    {
        mdlIn = IoAllocateMdl(buf, AVS_DA7219_IN_LEN, FALSE, FALSE, NULL);
        mdlOut = IoAllocateMdl(buf + 8, AVS_DA7219_OUT_LEN, FALSE, FALSE, NULL);
        if (mdlIn == NULL || mdlOut == NULL)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto out;
        }
        MmBuildMdlForNonPagedPool(mdlIn);
        MmBuildMdlForNonPagedPool(mdlOut);
    }
    KeInitializeEvent(&done, NotificationEvent, FALSE);

    KeAcquireSpinLock(&ctx->JackIrpLock, &irql);
    ctx->JackIrp = irp;
    KeReleaseSpinLock(&ctx->JackIrpLock, irql);

    // Ask for the current state (headphones may be in since boot).
    buf[8] = AVS_DA7219_REPORTID;
    buf[9] = 0;
    status = AvsJackIo(dev, file, irp, IRP_MJ_WRITE, buf + 8, AVS_DA7219_OUT_LEN, mdlOut, &done, &len);
    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: jack -- HID open (dev flags 0x%X), state request: 0x%X\n", dev->Flags, status));

    while (!ctx->JackStopping)
    {
        RtlZeroMemory(buf, AVS_DA7219_IN_LEN);
        status = AvsJackIo(dev, file, irp, IRP_MJ_READ, buf, AVS_DA7219_IN_LEN, mdlIn, &done, &len);
        if (!NT_SUCCESS(status))
        {
            AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                "AVS: jack -- read ended: 0x%X\n", status));
            break;
        }
        if (len >= AVS_DA7219_IN_LEN && buf[0] == AVS_DA7219_REPORTID && buf[1] == AVS_DA7219_JACK_TYPE)
        {
            AvsJackSetState(ctx, buf[2]);
        }
        else
        {
            AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                "AVS: jack -- other report: len %Iu, %02X %02X %02X\n", len, buf[0], buf[1], buf[2]));
        }
    }

out:
    KeAcquireSpinLock(&ctx->JackIrpLock, &irql);
    ctx->JackIrp = NULL;
    KeReleaseSpinLock(&ctx->JackIrpLock, irql);

    if (irp != NULL)
    {
        irp->MdlAddress = NULL;
        IoFreeIrp(irp);
    }
    if (mdlIn != NULL) IoFreeMdl(mdlIn);
    if (mdlOut != NULL) IoFreeMdl(mdlOut);
    if (buf != NULL) ExFreePoolWithTag(buf, AVS_JACK_POOL_TAG);
    if (file != NULL) ObDereferenceObject(file);
    if (handle != NULL) ZwClose(handle);
    ExFreePoolWithTag(start, AVS_JACK_POOL_TAG);

    InterlockedExchange(&ctx->JackReaderRunning, 0);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

// --------------------------------------------------------- PnP notification

static DRIVER_NOTIFICATION_CALLBACK_ROUTINE AvsJackPnpCallback;

static NTSTATUS
AvsJackPnpCallback(
    _In_ PVOID NotificationStructure,
    _Inout_opt_ PVOID Context
    )
{
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION n = (PDEVICE_INTERFACE_CHANGE_NOTIFICATION)NotificationStructure;
    PAVS_DEVICE_CONTEXT ctx = (PAVS_DEVICE_CONTEXT)Context;
    AVS_JACK_START* start;
    HANDLE thread;
    NTSTATUS status;
    SIZE_T size;

    if (ctx == NULL || ctx->JackStopping ||
        !IsEqualGUID(&n->Event, &AVS_GUID_DEVICE_INTERFACE_ARRIVAL))
    {
        return STATUS_SUCCESS;
    }
    if (!AvsLinkContainsAny(n->SymbolicLinkName, ctx->JackMatchId) ||
        !AvsLinkContains(n->SymbolicLinkName, ctx->JackMatchCol))
    {
        // every HID interface seen, so a naming mismatch shows in the log
        AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "AVS: jack -- (not ours) %wZ\n", n->SymbolicLinkName));
        return STATUS_SUCCESS;
    }

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: jack -- da7219 HID interface: %wZ\n", n->SymbolicLinkName));

    if (InterlockedCompareExchange(&ctx->JackReaderRunning, 1, 0) != 0)
    {
        return STATUS_SUCCESS;      // a reader already owns it
    }

    // The previous reader (if any) has finished; reap its thread object.
    if (ctx->JackThread != NULL)
    {
        KeWaitForSingleObject(ctx->JackThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(ctx->JackThread);
        ctx->JackThread = NULL;
    }

    // Don't open the device inside the notification callback (PnP
    // deadlock rule) -- the reader thread does it.
    size = sizeof(AVS_JACK_START) + n->SymbolicLinkName->Length + sizeof(WCHAR);
    start = (AVS_JACK_START*)ExAllocatePool2(POOL_FLAG_NON_PAGED, size, AVS_JACK_POOL_TAG);
    if (start == NULL)
    {
        InterlockedExchange(&ctx->JackReaderRunning, 0);
        return STATUS_SUCCESS;
    }
    start->Ctx = ctx;
    RtlCopyMemory(start->Buffer, n->SymbolicLinkName->Buffer, n->SymbolicLinkName->Length);
    start->Link.Buffer = start->Buffer;
    start->Link.Length = n->SymbolicLinkName->Length;
    start->Link.MaximumLength = n->SymbolicLinkName->Length + sizeof(WCHAR);

    status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, AvsJackReader, start);
    if (!NT_SUCCESS(status))
    {
        ExFreePoolWithTag(start, AVS_JACK_POOL_TAG);
        InterlockedExchange(&ctx->JackReaderRunning, 0);
        return STATUS_SUCCESS;
    }
    status = ObReferenceObjectByHandle(thread, SYNCHRONIZE, *PsThreadType, KernelMode,
        (PVOID*)&ctx->JackThread, NULL);
    if (!NT_SUCCESS(status))
    {
        ctx->JackThread = NULL;
    }
    ZwClose(thread);
    return STATUS_SUCCESS;
}

// ------------------------------------------------------------------- API

VOID
AvsJackInit(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    ExInitializeFastMutex(&ctx->JackLock);
    KeInitializeSpinLock(&ctx->JackIrpLock);
    ctx->JackState = -1;
}

NTSTATUS
AvsJackStart(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    BOOLEAN anyHeadphone = FALSE;
    CHAR text[48];
    NTSTATUS status;

    for (ULONG i = 0; i < AVS_MAX_ENDPOINTS; i++)
    {
        if (ctx->Endpoints[i].Valid && AVS_EP_HAS_JACK(ctx->Endpoints[i].Info.Type))
        {
            anyHeadphone = TRUE;
        }
    }
    if (!anyHeadphone || ctx->JackEnabled || AvsConfigReadUlong("JackDetect", 1) == 0)
    {
        return STATUS_SUCCESS;
    }

    if (!AvsConfigReadString("JackHidMatch", text, sizeof(text)))
    {
        RtlStringCbCopyA(text, sizeof(text), "DLGS7219|VID_2DCF&PID_7219");
    }
    AvsAsciiToUpperW(text, ctx->JackMatchId, ARRAYSIZE(ctx->JackMatchId));
    if (!AvsConfigReadString("JackHidCollection", text, sizeof(text)))
    {
        RtlStringCbCopyA(text, sizeof(text), "COL02");
    }
    AvsAsciiToUpperW(text, ctx->JackMatchCol, ARRAYSIZE(ctx->JackMatchCol));

    ctx->JackStopping = 0;
    ctx->JackEnabled = TRUE;

    status = IoRegisterPlugPlayNotification(EventCategoryDeviceInterfaceChange,
        PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
        (PVOID)&AVS_GUID_DEVINTERFACE_HID, ctx->Fdo->DriverObject,
        AvsJackPnpCallback, ctx, &ctx->JackPnpEntry);

    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, NT_SUCCESS(status) ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
        "AVS: jack -- watching HID interfaces for %ws %ws: 0x%X\n",
        ctx->JackMatchId, ctx->JackMatchCol, status));

    if (!NT_SUCCESS(status))
    {
        ctx->JackPnpEntry = NULL;
        ctx->JackEnabled = FALSE;
    }
    return status;
}

VOID
AvsJackStop(
    _In_ PAVS_DEVICE_CONTEXT ctx
    )
{
    if (!ctx->JackEnabled)
    {
        return;
    }

    InterlockedExchange(&ctx->JackStopping, 1);

    // Waits for callbacks in progress, so JackThread is final after this.
    if (ctx->JackPnpEntry != NULL)
    {
        IoUnregisterPlugPlayNotificationEx(ctx->JackPnpEntry);
        ctx->JackPnpEntry = NULL;
    }

    if (ctx->JackThread != NULL)
    {
        for (;;)
        {
            LARGE_INTEGER timeout;
            KIRQL irql;

            KeAcquireSpinLock(&ctx->JackIrpLock, &irql);
            if (ctx->JackIrp != NULL)
            {
                IoCancelIrp(ctx->JackIrp);
            }
            KeReleaseSpinLock(&ctx->JackIrpLock, irql);

            timeout.QuadPart = -100 * 10000LL;      // 100 ms
            if (KeWaitForSingleObject(ctx->JackThread, Executive, KernelMode, FALSE, &timeout) == STATUS_SUCCESS)
            {
                break;
            }
        }
        ObDereferenceObject(ctx->JackThread);
        ctx->JackThread = NULL;
    }

    ctx->JackEnabled = FALSE;
    AVS_KDPRINT((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "AVS: jack -- stopped\n"));
}

BOOLEAN
AvsDeviceHasJackDetect(
    _In_ PAVS_DEVICE Dev,
    _In_ ULONG Endpoint
    )
{
    PAVS_DEVICE_CONTEXT ctx = Dev;

    return Endpoint < AVS_MAX_ENDPOINTS && ctx->Endpoints[Endpoint].Valid &&
           AVS_EP_HAS_JACK(ctx->Endpoints[Endpoint].Info.Type) &&
           ctx->JackEnabled;
}

BOOLEAN
AvsDeviceIsJackConnected(
    _In_ PAVS_DEVICE Dev,
    _In_ ULONG Endpoint
    )
{
    PAVS_DEVICE_CONTEXT ctx = Dev;

    LONG state;

    if (Endpoint >= AVS_MAX_ENDPOINTS || !AVS_EP_HAS_JACK(ctx->Endpoints[Endpoint].Info.Type))
    {
        return TRUE;
    }
    state = ctx->JackState;
    if (state == -1)
    {
        return TRUE;                    // unknown -> keep the endpoint visible
    }
    if (ctx->Endpoints[Endpoint].Info.Type == AVS_EP_TYPE_HEADSET_MIC)
    {
        return (state & 2) != 0;        // SND_JACK_MICROPHONE
    }
    return state != 0;
}

VOID
AvsDeviceRegisterJackNotify(
    _In_ PAVS_DEVICE Dev,
    _In_ ULONG Endpoint,
    _In_opt_ AVS_JACK_NOTIFY Fn,
    _In_opt_ PVOID Context
    )
{
    PAVS_DEVICE_CONTEXT ctx = Dev;

    if (Endpoint >= AVS_MAX_ENDPOINTS)
    {
        return;
    }
    ExAcquireFastMutex(&ctx->JackLock);
    ctx->JackNotify[Endpoint] = Fn;
    ctx->JackNotifyCtx[Endpoint] = Context;
    ExReleaseFastMutex(&ctx->JackLock);
}
