// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// adapter.cpp -- PortCls adapter: DriverEntry, AddDevice, StartDevice,
// PnP stop/remove, adapter power management.
//
// The hardware work (bus interface, DSP boot, ported Linux core) is all
// C, behind avs_api.h; this file only wires it into PortCls.

#include <initguid.h>       // instantiate the KS/PortCls GUIDs once, here
#include "avs_pcls.h"

#define AVS_MAX_SUBDEVICES  (2 * AVS_MAX_ENDPOINTS)

// Our part of the FDO extension, right after PortCls's own part.
typedef struct _AVS_FDO_EXT
{
    PAVS_DEVICE     Dev;
    PDEVICE_OBJECT  Pdo;
} AVS_FDO_EXT, *PAVS_FDO_EXT;

static PAVS_FDO_EXT
AvsFdoExt(
    _In_ PDEVICE_OBJECT Fdo
    )
{
    return (PAVS_FDO_EXT)((PUCHAR)Fdo->DeviceExtension + PORT_CLASS_DEVICE_EXTENSION_SIZE);
}

// ------------------------------------------------------------- new/delete

PVOID __cdecl operator new(size_t Size, POOL_FLAGS Flags, ULONG Tag)
{
    return ExAllocatePool2(Flags, Size, Tag);     // zero-filled
}

// Plain operator delete(void*) comes from stdunk.lib (LNK2005 otherwise);
// it frees with ExFreePool, which matches ExAllocatePool2 allocations.

void __cdecl operator delete(PVOID Ptr, size_t Size)
{
    UNREFERENCED_PARAMETER(Size);
    if (Ptr != NULL)
    {
        ExFreePool(Ptr);
    }
}

void __cdecl operator delete[](PVOID Ptr)
{
    if (Ptr != NULL)
    {
        ExFreePool(Ptr);
    }
}

void __cdecl operator delete[](PVOID Ptr, size_t Size)
{
    UNREFERENCED_PARAMETER(Size);
    if (Ptr != NULL)
    {
        ExFreePool(Ptr);
    }
}

// ---------------------------------------------------- power management

class CAvsAdapterPower : public IAdapterPowerManagement, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CAvsAdapterPower);
    ~CAvsAdapterPower() {}

    STDMETHODIMP_(void) PowerChangeState(_In_ POWER_STATE NewState);
    STDMETHODIMP_(NTSTATUS) QueryPowerChangeState(_In_ POWER_STATE NewStateQuery);
    STDMETHODIMP_(NTSTATUS) QueryDeviceCapabilities(_Inout_ PDEVICE_CAPABILITIES PowerDeviceCaps);

    PAVS_DEVICE m_Dev;
};

STDMETHODIMP_(NTSTATUS)
CAvsAdapterPower::NonDelegatingQueryInterface(
    _In_ REFIID Interface,
    _COM_Outptr_ PVOID* Object
    )
{
    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PADAPTERPOWERMANAGEMENT(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IAdapterPowerManagement))
    {
        *Object = PVOID(PADAPTERPOWERMANAGEMENT(this));
    }
    else
    {
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }
    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(void)
CAvsAdapterPower::PowerChangeState(
    _In_ POWER_STATE NewState
    )
{
    AvsDeviceSetPower(m_Dev, NewState.DeviceState);
}

STDMETHODIMP_(NTSTATUS)
CAvsAdapterPower::QueryPowerChangeState(
    _In_ POWER_STATE NewStateQuery
    )
{
    UNREFERENCED_PARAMETER(NewStateQuery);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CAvsAdapterPower::QueryDeviceCapabilities(
    _Inout_ PDEVICE_CAPABILITIES PowerDeviceCaps
    )
{
    UNREFERENCED_PARAMETER(PowerDeviceCaps);
    return STATUS_SUCCESS;
}

// ------------------------------------------------------------ subdevices

// PcNewPort + Port::Init + PcRegisterSubdevice. Returns the port (one
// reference) for the physical connection.
static NTSTATUS
AvsInstallSubdevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PCWSTR Name,
    _In_ REFGUID PortClassId,
    _In_ PUNKNOWN Miniport,
    _In_opt_ PUNKNOWN UnknownAdapter,
    _In_ PRESOURCELIST ResourceList,
    _Out_ PUNKNOWN* OutPort
    )
{
    PPORT port = NULL;
    NTSTATUS status;

    *OutPort = NULL;

    status = PcNewPort(&port, PortClassId);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = port->Init(DeviceObject, Irp, Miniport, UnknownAdapter, ResourceList);
    if (NT_SUCCESS(status))
    {
        status = PcRegisterSubdevice(DeviceObject, (PWSTR)Name, port);
    }

    if (!NT_SUCCESS(status))
    {
        port->Release();
        return status;
    }

    *OutPort = port;
    return STATUS_SUCCESS;
}

// Subdevice names MUST be static: PcRegisterSubdevice hands the string to
// KS as the create-item ObjectClass, which KS keeps BY POINTER and compares
// on every filter open (KsAddObjectCreateItemToDeviceHeader: "must remain
// valid while the object is active"). A stack buffer here = KS reading a
// dead stack on the first open -> no endpoints, then a 0x50 bugcheck.
static const PCWSTR AvsWaveNames[AVS_MAX_ENDPOINTS] = { L"Wave0", L"Wave1", L"Wave2", L"Wave3" };
static const PCWSTR AvsTopoNames[AVS_MAX_ENDPOINTS] = { L"Topo0", L"Topo1", L"Topo2", L"Topo3" };

static NTSTATUS
AvsInstallEndpoint(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList,
    _In_ PUNKNOWN UnknownAdapter,
    _In_ PAVS_DEVICE Dev,
    _In_ ULONG Index
    )
{
    PUNKNOWN miniTopo = NULL, miniWave = NULL;
    PUNKNOWN portTopo = NULL, portWave = NULL;
    PCWSTR nameTopo = AvsTopoNames[Index];
    PCWSTR nameWave = AvsWaveNames[Index];
    NTSTATUS status;

    status = AvsCreateMiniportTopology(&miniTopo, Dev, Index);
    if (NT_SUCCESS(status))
    {
        status = AvsInstallSubdevice(DeviceObject, Irp, nameTopo, CLSID_PortTopology,
            miniTopo, UnknownAdapter, ResourceList, &portTopo);
    }

    if (NT_SUCCESS(status))
    {
        status = AvsCreateMiniportWaveRT(&miniWave, Dev, Index);
    }
    if (NT_SUCCESS(status))
    {
        status = AvsInstallSubdevice(DeviceObject, Irp, nameWave, CLSID_PortWaveRT,
            miniWave, UnknownAdapter, ResourceList, &portWave);
    }

    // Render: wave bridge (DAC out) -> topology. Capture: topology -> wave
    // bridge (ADC in).
    if (NT_SUCCESS(status))
    {
        AVS_EP_INFO info;

        AvsDeviceGetEndpoint(Dev, Index, &info);
        status = info.Capture
            ? PcRegisterPhysicalConnection(DeviceObject, portTopo, AVS_TOPO_PIN_WAVE, portWave, AVS_WAVE_PIN_BRIDGE)
            : PcRegisterPhysicalConnection(DeviceObject, portWave, AVS_WAVE_PIN_BRIDGE, portTopo, AVS_TOPO_PIN_WAVE);
    }

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, NT_SUCCESS(status) ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
        "AVS: endpoint %lu -> %ws + %ws: 0x%X\n", Index, nameWave, nameTopo, status);

    if (portWave != NULL) portWave->Release();
    if (portTopo != NULL) portTopo->Release();
    if (miniWave != NULL) miniWave->Release();
    if (miniTopo != NULL) miniTopo->Release();
    return status;
}

// ---------------------------------------------------------- start device

static NTSTATUS
AvsStartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList
    )
{
    PAVS_FDO_EXT ext = AvsFdoExt(DeviceObject);
    CAvsAdapterPower* power;
    PUNKNOWN unknownAdapter;
    ULONG installed = 0;
    NTSTATUS status;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "AVS: StartDevice (PortCls/WaveRT)\n");

    if (ext->Dev == NULL)
    {
        status = AvsDeviceCreate(DeviceObject, ext->Pdo, &ext->Dev);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
    }

    status = AvsDeviceStart(ext->Dev);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    power = new(POOL_FLAG_NON_PAGED, AVS_PCLS_POOL_TAG) CAvsAdapterPower(NULL);
    if (power == NULL)
    {
        AvsDeviceStop(ext->Dev);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    power->m_Dev = ext->Dev;
    unknownAdapter = PUNKNOWN(PADAPTERPOWERMANAGEMENT(power));
    unknownAdapter->AddRef();

    status = PcRegisterAdapterPowerManagement(unknownAdapter, DeviceObject);
    if (!NT_SUCCESS(status))
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: PcRegisterAdapterPowerManagement failed: 0x%X\n", status);
    }

    for (ULONG i = 0; i < AVS_MAX_ENDPOINTS; i++)
    {
        AVS_EP_INFO info;

        if (!AvsDeviceGetEndpoint(ext->Dev, i, &info))
        {
            continue;
        }
        if (NT_SUCCESS(AvsInstallEndpoint(DeviceObject, Irp, ResourceList, unknownAdapter,
                ext->Dev, i)))
        {
            installed++;
        }
    }

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "AVS: %lu endpoint(s) registered with PortCls\n", installed);

    unknownAdapter->Release();      // PortCls holds its own references
    return STATUS_SUCCESS;
}

// ------------------------------------------------------------------ PnP

static DRIVER_DISPATCH AvsDispatchPnp;

static NTSTATUS
AvsDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PAVS_FDO_EXT ext = AvsFdoExt(DeviceObject);
    NTSTATUS status;

    switch (stack->MinorFunction)
    {
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        // DSP paths, IPC interrupt and bus interface go before the
        // lower drivers see the IRP.
        if (ext->Dev != NULL)
        {
            AvsDeviceStop(ext->Dev);
        }
        break;

    case IRP_MN_REMOVE_DEVICE:
    {
        PAVS_DEVICE dev = ext->Dev;

        ext->Dev = NULL;
        if (dev != NULL)
        {
            AvsDeviceStop(dev);
        }
        // PortCls tears down subdevices and deletes the FDO (with ext).
        status = PcDispatchIrp(DeviceObject, Irp);
        if (dev != NULL)
        {
            AvsDeviceDestroy(dev);
        }
        return status;
    }

    default:
        break;
    }

    return PcDispatchIrp(DeviceObject, Irp);
}

// -------------------------------------------------------- driver entry

static NTSTATUS
AvsAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject
    )
{
    PDEVICE_OBJECT fdo;
    NTSTATUS status;

    status = PcAddAdapterDevice(DriverObject, PhysicalDeviceObject, AvsStartDevice,
        AVS_MAX_SUBDEVICES, PORT_CLASS_DEVICE_EXTENSION_SIZE + sizeof(AVS_FDO_EXT));
    if (!NT_SUCCESS(status))
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: PcAddAdapterDevice failed: 0x%X\n", status);
        return status;
    }

    // PcAddAdapterDevice doesn't hand the FDO back. It has just attached
    // it on top of the PDO's stack (upper filters attach later), so the
    // top of the stack is our FDO right now.
    fdo = IoGetAttachedDeviceReference(PhysicalDeviceObject);
    if (fdo->DriverObject == DriverObject)
    {
        AvsFdoExt(fdo)->Pdo = PhysicalDeviceObject;
        AvsFdoExt(fdo)->Dev = NULL;
    }
    else
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AVS: AddDevice -- top of stack is not our FDO\n");
        status = STATUS_UNSUCCESSFUL;
    }
    ObDereferenceObject(fdo);
    return status;
}

extern "C" DRIVER_INITIALIZE DriverEntry;

extern "C" NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "AVS: DriverEntry (PortCls/WaveRT build)\n");

    status = PcInitializeAdapterDriver(DriverObject, RegistryPath, (PDRIVER_ADD_DEVICE)AvsAddDevice);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    // Our PnP hook in front of PortCls (stop/remove cleanup).
    DriverObject->MajorFunction[IRP_MJ_PNP] = AvsDispatchPnp;
    return STATUS_SUCCESS;
}
