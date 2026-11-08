// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 AVS OpenAudio Driver Project contributors
//
// mintopo.cpp -- topology miniport (one per endpoint).
//
// Two bridge pins and no nodes: pin 0 comes from the wave filter's DAC,
// pin 1 is the physical output whose category tells Windows what the
// endpoint is (KSNODETYPE_SPEAKER -> "Speakers", KSNODETYPE_HEADPHONES ->
// "Headphones"). Without volume/mute nodes Windows uses software volume.
//
// Jack: KSPROPERTY_JACK_DESCRIPTION(2) on the output pin, IsConnected from
// avs_jack.c (da7219.sys HID reports), KSEVENT_PINCAPS_JACKINFOCHANGE when
// it changes. Windows then hides an unplugged "Headphones" endpoint and
// switches to it when the plug goes in (same layout as Microsoft's sysvad).

#include "avs_pcls.h"

class CMiniportTopology : public IMiniportTopology, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportTopology);
    ~CMiniportTopology();

    IMP_IMiniport;
    STDMETHODIMP_(NTSTATUS) Init(
        _In_ PUNKNOWN UnknownAdapter,
        _In_ PRESOURCELIST ResourceList,
        _In_ PPORTTOPOLOGY Port);

    VOID Setup(_In_ PAVS_DEVICE Dev, _In_ ULONG Endpoint);

    static NTSTATUS PropertyJack(_In_ PPCPROPERTY_REQUEST Request);
    static NTSTATUS EventJack(_In_ PPCEVENT_REQUEST Request);
    static VOID JackChanged(_In_ PVOID Context);

    PAVS_DEVICE             m_Dev;
    ULONG                   m_Endpoint;
    AVS_EP_INFO             m_Info;
    PPORTEVENTS             m_PortEvents;
    BOOLEAN                 m_JackRegistered;

private:
    PPORTTOPOLOGY           m_Port;
    KSDATARANGE             m_BridgeRange;
    PKSDATARANGE            m_BridgeRanges[1];
    PCPIN_DESCRIPTOR        m_Pins[2];
    PCCONNECTION_DESCRIPTOR m_Connections[1];
    GUID                    m_InCategory;
    GUID                    m_OutCategory;
    GUID                    m_Categories[2];
    PCFILTER_DESCRIPTOR     m_Filter;
};

// ------------------------------------------------- jack automation table

// Filter-level, the pin id comes in the KSP_PIN instance (sysvad layout).
static PCPROPERTY_ITEM AvsTopoFilterProperties[] =
{
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        CMiniportTopology::PropertyJack
    },
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION2,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        CMiniportTopology::PropertyJack
    },
};

static PCEVENT_ITEM AvsTopoFilterEvents[] =
{
    {
        &KSEVENTSETID_PinCapsChange,
        KSEVENT_PINCAPS_JACKINFOCHANGE,
        KSEVENT_TYPE_ENABLE | KSEVENT_TYPE_BASICSUPPORT,
        CMiniportTopology::EventJack
    },
};

static PCAUTOMATION_TABLE AvsTopoFilterAutomation =
{
    sizeof(PCPROPERTY_ITEM), ARRAYSIZE(AvsTopoFilterProperties), AvsTopoFilterProperties,
    sizeof(PCMETHOD_ITEM),   0,                                  NULL,
    sizeof(PCEVENT_ITEM),    ARRAYSIZE(AvsTopoFilterEvents),     AvsTopoFilterEvents,
    0
};

NTSTATUS
AvsCreateMiniportTopology(
    _Out_ PUNKNOWN* Unknown,
    _In_ PAVS_DEVICE Dev,
    _In_ ULONG Endpoint
    )
{
    CMiniportTopology* obj = new(POOL_FLAG_NON_PAGED, AVS_PCLS_POOL_TAG) CMiniportTopology(NULL);

    *Unknown = NULL;
    if (obj == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    obj->Setup(Dev, Endpoint);
    *Unknown = PUNKNOWN(PMINIPORTTOPOLOGY(obj));
    (*Unknown)->AddRef();
    return STATUS_SUCCESS;
}

CMiniportTopology::~CMiniportTopology()
{
    if (m_JackRegistered)
    {
        // waits for a notification already in flight
        AvsDeviceRegisterJackNotify(m_Dev, m_Endpoint, NULL, NULL);
        m_JackRegistered = FALSE;
    }
    if (m_PortEvents != NULL)
    {
        m_PortEvents->Release();
        m_PortEvents = NULL;
    }
    if (m_Port != NULL)
    {
        m_Port->Release();
        m_Port = NULL;
    }
}

VOID
CMiniportTopology::Setup(
    _In_ PAVS_DEVICE Dev,
    _In_ ULONG Endpoint
    )
{
    m_Dev = Dev;
    m_Endpoint = Endpoint;
    AvsDeviceGetEndpoint(Dev, Endpoint, &m_Info);

    RtlZeroMemory(&m_BridgeRange, sizeof(m_BridgeRange));
    m_BridgeRange.FormatSize = sizeof(KSDATARANGE);
    m_BridgeRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    m_BridgeRange.SubFormat = KSDATAFORMAT_SUBTYPE_ANALOG;
    m_BridgeRange.Specifier = KSDATAFORMAT_SPECIFIER_NONE;
    m_BridgeRanges[0] = &m_BridgeRange;

    // Wave-side bridge pin: KSCATEGORY_AUDIO. Physical pin: its category
    // names the endpoint form factor for Windows.
    m_InCategory = KSCATEGORY_AUDIO;
    switch (m_Info.Type)
    {
    case AVS_EP_TYPE_HEADPHONE:   m_OutCategory = KSNODETYPE_HEADPHONES; break;
    case AVS_EP_TYPE_MIC:         m_OutCategory = KSNODETYPE_MICROPHONE; break;
    case AVS_EP_TYPE_HEADSET_MIC: m_OutCategory = KSNODETYPE_MICROPHONE; break;   // the 3.5 mm jack description tells it apart
    default:                      m_OutCategory = KSNODETYPE_SPEAKER; break;
    }

    RtlZeroMemory(m_Pins, sizeof(m_Pins));
    m_Pins[AVS_TOPO_PIN_WAVE].KsPinDescriptor.DataRangesCount = 1;
    m_Pins[AVS_TOPO_PIN_WAVE].KsPinDescriptor.DataRanges = m_BridgeRanges;
    m_Pins[AVS_TOPO_PIN_WAVE].KsPinDescriptor.DataFlow =
        m_Info.Capture ? KSPIN_DATAFLOW_OUT : KSPIN_DATAFLOW_IN;
    m_Pins[AVS_TOPO_PIN_WAVE].KsPinDescriptor.Communication = KSPIN_COMMUNICATION_NONE;
    m_Pins[AVS_TOPO_PIN_WAVE].KsPinDescriptor.Category = &m_InCategory;

    m_Pins[AVS_TOPO_PIN_JACK].KsPinDescriptor.DataRangesCount = 1;
    m_Pins[AVS_TOPO_PIN_JACK].KsPinDescriptor.DataRanges = m_BridgeRanges;
    m_Pins[AVS_TOPO_PIN_JACK].KsPinDescriptor.DataFlow =
        m_Info.Capture ? KSPIN_DATAFLOW_IN : KSPIN_DATAFLOW_OUT;
    m_Pins[AVS_TOPO_PIN_JACK].KsPinDescriptor.Communication = KSPIN_COMMUNICATION_NONE;
    m_Pins[AVS_TOPO_PIN_JACK].KsPinDescriptor.Category = &m_OutCategory;

    // render: wave bridge -> jack; capture: mic -> wave bridge
    m_Connections[0].FromNode = PCFILTER_NODE;
    m_Connections[0].FromNodePin = m_Info.Capture ? AVS_TOPO_PIN_JACK : AVS_TOPO_PIN_WAVE;
    m_Connections[0].ToNode = PCFILTER_NODE;
    m_Connections[0].ToNodePin = m_Info.Capture ? AVS_TOPO_PIN_WAVE : AVS_TOPO_PIN_JACK;

    m_Categories[0] = KSCATEGORY_AUDIO;
    m_Categories[1] = KSCATEGORY_TOPOLOGY;

    RtlZeroMemory(&m_Filter, sizeof(m_Filter));
    m_Filter.Version = 0;
    m_Filter.AutomationTable = &AvsTopoFilterAutomation;
    m_Filter.PinSize = sizeof(PCPIN_DESCRIPTOR);
    m_Filter.PinCount = ARRAYSIZE(m_Pins);
    m_Filter.Pins = m_Pins;
    m_Filter.NodeSize = sizeof(PCNODE_DESCRIPTOR);
    m_Filter.NodeCount = 0;
    m_Filter.Nodes = NULL;
    m_Filter.ConnectionCount = ARRAYSIZE(m_Connections);
    m_Filter.Connections = m_Connections;
    m_Filter.CategoryCount = ARRAYSIZE(m_Categories);
    m_Filter.Categories = m_Categories;
}

STDMETHODIMP_(NTSTATUS)
CMiniportTopology::NonDelegatingQueryInterface(
    _In_ REFIID Interface,
    _COM_Outptr_ PVOID* Object
    )
{
    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PMINIPORTTOPOLOGY(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport))
    {
        *Object = PVOID(PMINIPORT(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportTopology))
    {
        *Object = PVOID(PMINIPORTTOPOLOGY(this));
    }
    else
    {
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }
    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportTopology::Init(
    _In_ PUNKNOWN UnknownAdapter,
    _In_ PRESOURCELIST ResourceList,
    _In_ PPORTTOPOLOGY Port
    )
{
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);

    m_Port = Port;
    m_Port->AddRef();

    // IPortEvents of our own port, for KSEVENT_PINCAPS_JACKINFOCHANGE.
    if (!NT_SUCCESS(m_Port->QueryInterface(IID_IPortEvents, (PVOID*)&m_PortEvents)))
    {
        m_PortEvents = NULL;
    }
    if (m_PortEvents != NULL && AvsDeviceHasJackDetect(m_Dev, m_Endpoint))
    {
        AvsDeviceRegisterJackNotify(m_Dev, m_Endpoint, JackChanged, this);
        m_JackRegistered = TRUE;
    }

    AVS_LOG("AVS: Topo%lu Init (%s), jack detect %s, now %s\n", m_Endpoint,
        m_Info.Type == AVS_EP_TYPE_HEADPHONE ? "headphones" :
        m_Info.Type == AVS_EP_TYPE_MIC ? "microphones" :
        m_Info.Type == AVS_EP_TYPE_HEADSET_MIC ? "headset mic" : "speakers",
        m_JackRegistered ? "on" : "off",
        AvsDeviceIsJackConnected(m_Dev, m_Endpoint) ? "connected" : "unplugged");
    return STATUS_SUCCESS;
}

// ------------------------------------------------------------------ jack

// Called by avs_jack.c on every plug/unplug (PASSIVE/APC level).
VOID
CMiniportTopology::JackChanged(
    _In_ PVOID Context
    )
{
    CMiniportTopology* self = (CMiniportTopology*)Context;
    BOOLEAN connected = AvsDeviceIsJackConnected(self->m_Dev, self->m_Endpoint);

    AVS_LOG("AVS: Topo%lu jack -> %s, notifying Windows\n", self->m_Endpoint,
        connected ? "connected" : "unplugged");

    if (self->m_PortEvents != NULL)
    {
        self->m_PortEvents->GenerateEventList((GUID*)&KSEVENTSETID_PinCapsChange,
            KSEVENT_PINCAPS_JACKINFOCHANGE, FALSE, ULONG(-1), FALSE, ULONG(-1));
    }
}

NTSTATUS
CMiniportTopology::EventJack(
    _In_ PPCEVENT_REQUEST Request
    )
{
    CMiniportTopology* self = static_cast<CMiniportTopology*>(PMINIPORTTOPOLOGY(Request->MajorTarget));

    if (Request->Verb == PCEVENT_VERB_ADD)
    {
        if (self->m_PortEvents == NULL)
        {
            return STATUS_UNSUCCESSFUL;
        }
        self->m_PortEvents->AddEventToEventList(Request->EventEntry);
    }
    return STATUS_SUCCESS;      // REMOVE / SUPPORT: nothing to do
}

NTSTATUS
CMiniportTopology::PropertyJack(
    _In_ PPCPROPERTY_REQUEST Request
    )
{
    CMiniportTopology* self = static_cast<CMiniportTopology*>(PMINIPORTTOPOLOGY(Request->MajorTarget));
    BOOLEAN desc2 = (Request->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION2);
    BOOLEAN headphone = AVS_EP_HAS_JACK(self->m_Info.Type) ? TRUE : FALSE;  // 3.5 mm jack
    ULONG pin = (Request->InstanceSize >= sizeof(ULONG)) ? *(PULONG)Request->Instance : ULONG(-1);
    ULONG cb = sizeof(KSMULTIPLE_ITEM) + (desc2 ? sizeof(KSJACK_DESCRIPTION2) : sizeof(KSJACK_DESCRIPTION));
    PKSMULTIPLE_ITEM item;

    if (pin != AVS_TOPO_PIN_JACK)
    {
        return STATUS_INVALID_PARAMETER;   // only the physical output has a jack
    }

    if (Request->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        const ULONG access = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET;

        if (Request->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION))
        {
            PKSPROPERTY_DESCRIPTION d = (PKSPROPERTY_DESCRIPTION)Request->Value;

            d->AccessFlags = access;
            d->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION);
            d->PropTypeSet.Set = KSPROPTYPESETID_General;
            d->PropTypeSet.Id = 0xFFFF;     // VT_ILLEGAL
            d->PropTypeSet.Flags = 0;
            d->MembersListCount = 0;
            d->Reserved = 0;
            Request->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        }
        else if (Request->ValueSize >= sizeof(ULONG))
        {
            *(PULONG)Request->Value = access;
            Request->ValueSize = sizeof(ULONG);
        }
        else
        {
            return STATUS_BUFFER_TOO_SMALL;
        }
        return STATUS_SUCCESS;
    }

    if (!(Request->Verb & KSPROPERTY_TYPE_GET))
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (Request->ValueSize == 0)
    {
        Request->ValueSize = cb;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (Request->ValueSize < cb)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    item = (PKSMULTIPLE_ITEM)Request->Value;
    item->Size = cb;
    item->Count = 1;

    if (desc2)
    {
        PKSJACK_DESCRIPTION2 d = (PKSJACK_DESCRIPTION2)(item + 1);

        RtlZeroMemory(d, sizeof(*d));
        d->DeviceStateInfo = 0;
        d->JackCapabilities = (headphone && self->m_JackRegistered) ? JACKDESC2_PRESENCE_DETECT_CAPABILITY : 0;
    }
    else
    {
        PKSJACK_DESCRIPTION d = (PKSJACK_DESCRIPTION)(item + 1);

        RtlZeroMemory(d, sizeof(*d));
        d->ChannelMapping = (self->m_Info.Channels == 1) ? KSAUDIO_SPEAKER_MONO : KSAUDIO_SPEAKER_STEREO;
        d->Color = 0;                                   // black
        d->ConnectionType = headphone ? eConnType3Point5mm : eConnTypeUnknown;
        d->GeoLocation = headphone ? eGeoLocLeft : eGeoLocFront;   // cosmetic only
        d->GenLocation = eGenLocPrimaryBox;
        d->PortConnection = headphone ? ePortConnJack : ePortConnIntegratedDevice;
        d->IsConnected = AvsDeviceIsJackConnected(self->m_Dev, self->m_Endpoint);
    }

    Request->ValueSize = cb;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportTopology::GetDescription(
    _Out_ PPCFILTER_DESCRIPTOR* Description
    )
{
    AVS_LOG("AVS: Topo%lu GetDescription\n", m_Endpoint);
    *Description = &m_Filter;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportTopology::DataRangeIntersection(
    _In_ ULONG PinId,
    _In_ PKSDATARANGE DataRange,
    _In_ PKSDATARANGE MatchingDataRange,
    _In_ ULONG OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength) PVOID ResultantFormat,
    _Out_ PULONG ResultantFormatLength
    )
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);
    return STATUS_NOT_IMPLEMENTED;
}
