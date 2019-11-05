/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    The connection is the topmost structure that all connection-specific state
    and logic is derived from. Connections are only ever processed by one
    thread at a time. Other threads may queue operations on the connection, but
    the operations are only drained and processed serially, by a single thread;
    though the thread that does the draining may change over time. All
    events/triggers/API calls are processed via operations.

    The connection drains operations in the QuicConnDrainOperations function.
    The only requirement here is that this function is not called in parallel
    on multiple threads. The function will drain up to QUIC_SETTINGS's
    MaxOperationsPerDrain operations per call, so as to not starve any other
    work.

    While most of the connection specific work is managed by other interfaces,
    the following things are managed in this file:

    Connection Lifetime - Initialization, handshake and state changes, shutdown,
    closure and cleanup are located here.

    Receive Path - The per-connection packet receive path is here. This is the
    logic that happens after the global receive callback has processed the
    packet initially and done the necessary processing to pass the packet to
    the correct connection.

--*/

#include "precomp.h"

#ifdef QUIC_LOGS_WPP
#include "connection.tmh"
#endif

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
QuicConnInitializeCrypto(
    _In_ PQUIC_CONNECTION Connection
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
__drv_allocatesMem(Mem)
_Must_inspect_result_
_Success_(return != NULL)
PQUIC_CONNECTION
QuicConnAlloc(
    _In_opt_ const QUIC_RECV_DATAGRAM* const Datagram
    )
{
    BOOLEAN IsServer = Datagram != NULL;
    uint8_t AllocProcIndex =
        (uint8_t)(QuicProcCurrentNumber() % MsQuicLib.PartitionCount);

    PQUIC_CONNECTION Connection =
        QuicPoolAlloc(&MsQuicLib.PerProc[AllocProcIndex].ConnectionPool);
    if (Connection == NULL) {
        EventWriteQuicAllocFailure("connection", sizeof(QUIC_CONNECTION));
        goto Error;
    }
    QuicZeroMemory(Connection, sizeof(QUIC_CONNECTION));

#if QUIC_TEST_MODE
    InterlockedIncrement(&MsQuicLib.ConnectionCount);
#endif

    Connection->Stats.CorrelationId =
        InterlockedIncrement64((LONG64*)&MsQuicLib.ConnectionCorrelationId) - 1;
    EventWriteQuicConnCreated(Connection, IsServer, Connection->Stats.CorrelationId);

    Connection->RefCount = 1;
#if QUIC_TEST_MODE
    Connection->RefTypeCount[QUIC_CONN_REF_HANDLE_OWNER] = 1;
#endif
    Connection->AllocProcIndex = AllocProcIndex;
    Connection->State.Allocated = TRUE;
    Connection->State.UseSendBuffer = QUIC_DEFAULT_SEND_BUFFERING_ENABLE;
    Connection->State.EncryptionEnabled = !MsQuicLib.EncryptionDisabled;
    Connection->State.ShareBinding = IsServer;
    Connection->Stats.Timing.Start = QuicTimeUs64();
    Connection->MinRtt = UINT32_MAX;
    Connection->AckDelayExponent = QUIC_ACK_DELAY_EXPONENT;
    Connection->PeerTransportParams.AckDelayExponent = QUIC_DEFAULT_ACK_DELAY_EXPONENT;
    Connection->ReceiveQueueTail = &Connection->ReceiveQueue;
    QuicDispatchLockInitialize(&Connection->ReceiveQueueLock);
    QuicListInitializeHead(&Connection->DestCIDs);
    QuicStreamSetInitialize(&Connection->Streams);
    QuicSendBufferInitialize(&Connection->SendBuffer);
    QuicOperationQueueInitialize(&Connection->OperQ);
    QuicSendInitialize(&Connection->Send);
    QuicCongestionControlInitialize(&Connection->CongestionControl);
    QuicLossDetectionInitialize(&Connection->LossDetection);

    for (uint32_t i = 0; i < ARRAYSIZE(Connection->Timers); i++) {
        Connection->Timers[i].Type = (QUIC_CONN_TIMER_TYPE)i;
        Connection->Timers[i].ExpirationTime = UINT64_MAX;
    }

    if (IsServer) {
        const QUIC_RECV_PACKET* Packet =
            QuicDataPathRecvDatagramToRecvPacket(Datagram);

        Connection->Type = QUIC_HANDLE_TYPE_CHILD;
        Connection->ServerID = Packet->DestCID[QUIC_CID_SID_INDEX];
        Connection->PartitionID = AllocProcIndex; // Used in tuple RSS modes.

        Connection->Stats.QuicVersion = Packet->Invariant->LONG_HDR.Version;
        QuicConnOnQuicVersionSet(Connection);

        Connection->LocalAddress = Datagram->Tuple->LocalAddress;
        Connection->State.LocalAddressSet = TRUE;
        EventWriteQuicConnLocalAddrAdded(
            Connection,
            LOG_ADDR_LEN(Connection->LocalAddress),
            (const uint8_t*)&Connection->LocalAddress);

        Connection->RemoteAddress = Datagram->Tuple->RemoteAddress;
        Connection->State.RemoteAddressSet = TRUE;
        EventWriteQuicConnRemoteAddrAdded(
            Connection,
            LOG_ADDR_LEN(Connection->RemoteAddress),
            (const uint8_t*)&Connection->RemoteAddress);

        QUIC_CID_QUIC_LIST_ENTRY* DestCID =
            QuicCidNewDestination(Packet->SourceCIDLen, Packet->SourceCID);
        if (DestCID == NULL) {
            goto Error;
        }
        QuicListInsertTail(&Connection->DestCIDs, &DestCID->Link);
        EventWriteQuicConnDestCidAdded(
            Connection, DestCID->CID.Length, DestCID->CID.Data);

        QUIC_CID_HASH_ENTRY* SourceCID =
            QuicCidNewSource(Connection, Packet->DestCIDLen, Packet->DestCID);
        if (SourceCID == NULL) {
            goto Error;
        }
        SourceCID->CID.IsInitial = TRUE;
        SourceCID->CID.UsedByPeer = TRUE;
        QuicListPushEntry(&Connection->SourceCIDs, &SourceCID->Link);
        EventWriteQuicConnSourceCidAdded(
            Connection, SourceCID->CID.Length, SourceCID->CID.Data);

    } else {
        Connection->Type = QUIC_HANDLE_TYPE_CLIENT;
        Connection->State.ExternalOwner = TRUE;
        Connection->State.SourceAddressValidated = TRUE;
        Connection->Send.Allowance = UINT32_MAX;

        QUIC_CID_QUIC_LIST_ENTRY* DestCID = QuicCidNewRandomDestination();
        if (DestCID == NULL) {
            goto Error;
        }
        Connection->DestCIDCount++;
        QuicListInsertTail(&Connection->DestCIDs, &DestCID->Link);
        EventWriteQuicConnDestCidAdded(Connection, DestCID->CID.Length, DestCID->CID.Data);
    }

    return Connection;

Error:

    if (Connection != NULL) {
        QuicConnRelease(Connection, QUIC_CONN_REF_HANDLE_OWNER);
    }

    return NULL;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
QuicConnInitialize(
    _In_opt_ const QUIC_RECV_DATAGRAM* const Datagram, // NULL for client side
    _Outptr_ _At_(*NewConnection, __drv_allocatesMem(Mem))
        PQUIC_CONNECTION* NewConnection
    )
{
    QUIC_STATUS Status;
    uint32_t InitStep = 0;

    PQUIC_CONNECTION Connection = QuicConnAlloc(Datagram);
    if (Connection == NULL) {
        Status = QUIC_STATUS_OUT_OF_MEMORY;
        goto Error;
    }
    InitStep++; // Step 1

    for (uint32_t i = 0; i < ARRAYSIZE(Connection->Packets); i++) {
        Status =
            QuicPacketSpaceInitialize(
                Connection,
                (QUIC_ENCRYPT_LEVEL)i,
                &Connection->Packets[i]);
        if (QUIC_FAILED(Status)) {
            goto Error;
        }
    }

    //
    // N.B. Initializing packet space can fail part-way through, so it must be
    //      cleaned up even if it doesn't complete. Do not separate it from
    //      allocation.
    //
    Status =
        QuicRangeInitialize(
            QUIC_MAX_RANGE_DECODE_ACKS,
            &Connection->DecodedAckRanges);
    if (QUIC_FAILED(Status)) {
        goto Error;
    }
    InitStep++; // Step 2

    if (Datagram == NULL) {
        Connection->State.Initialized = TRUE;
        EventWriteQuicConnInitializeComplete(Connection);
    } else {
        //
        // Server lazily finishes initialzation in response to first operation.
        //
    }

    *NewConnection = Connection;

    return QUIC_STATUS_SUCCESS;

Error:

    switch (InitStep) {
    case 2:
        QuicRangeUninitialize(&Connection->DecodedAckRanges);
        __fallthrough;
    case 1:
        for (uint32_t i = 0; i < ARRAYSIZE(Connection->Packets); i++) {
            if (Connection->Packets[i] != NULL) {
                QuicPacketSpaceUninitialize(Connection->Packets[i]);
            }
        }

        Connection->State.HandleClosed = TRUE;
        Connection->State.Uninitialized = TRUE;
        if (Datagram != NULL) {
            QUIC_FREE(
                QUIC_CONTAINING_RECORD(
                    Connection->SourceCIDs.Next,
                    QUIC_CID_HASH_ENTRY,
                    Link));
            Connection->SourceCIDs.Next = NULL;
        }
        QuicConnRelease(Connection, QUIC_CONN_REF_HANDLE_OWNER);
        break;
    }

    return Status;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicConnFree(
    _In_ __drv_freesMem(Mem) PQUIC_CONNECTION Connection
    )
{
    QUIC_FRE_ASSERT(!Connection->State.Freed);
    QUIC_TEL_ASSERT(Connection->RefCount == 0);
    if (Connection->State.ExternalOwner) {
        QUIC_TEL_ASSERT(Connection->State.HandleClosed);
        QUIC_TEL_ASSERT(Connection->State.Uninitialized);
    }
    QUIC_TEL_ASSERT(Connection->SourceCIDs.Next == NULL);
    QUIC_TEL_ASSERT(QuicListIsEmpty(&Connection->Streams.ClosedStreams));
    QuicLossDetectionUninitialize(&Connection->LossDetection);
    QuicSendUninitialize(&Connection->Send);
    while (!QuicListIsEmpty(&Connection->DestCIDs)) {
        QUIC_CID_QUIC_LIST_ENTRY *CID =
            QUIC_CONTAINING_RECORD(
                QuicListRemoveHead(&Connection->DestCIDs),
                QUIC_CID_QUIC_LIST_ENTRY,
                Link);
        QUIC_FREE(CID);
    }
    if (Connection->Worker != NULL) {
        QuicOperationQueueClear(Connection->Worker, &Connection->OperQ);
    }
    if (Connection->ReceiveQueue != NULL) {
        QUIC_RECV_DATAGRAM* Datagram = Connection->ReceiveQueue;
        do {
            Datagram->QueuedOnConnection = FALSE;
        } while ((Datagram = Datagram->Next) != NULL);
        QuicDataPathBindingReturnRecvDatagrams(Connection->ReceiveQueue);
        Connection->ReceiveQueue = NULL;
    }
    if (Connection->Binding != NULL) {
        if (!Connection->State.Connected) {
            InterlockedDecrement(&Connection->Binding->HandshakeConnections);
            InterlockedExchangeAdd64(
                (LONG64*)&MsQuicLib.CurrentHandshakeMemoryUsage,
                -1 * (LONG64)QUIC_CONN_HANDSHAKE_MEMORY_USAGE);
        }
        QuicLibraryReleaseBinding(Connection->Binding);
        Connection->Binding = NULL;
    }
    QuicDispatchLockUninitialize(&Connection->ReceiveQueueLock);
    QuicOperationQueueUninitialize(&Connection->OperQ);
    QuicStreamSetUninitialize(&Connection->Streams);
    QuicSendBufferUninitialize(&Connection->SendBuffer);
    Connection->State.Freed = TRUE;
    if (Connection->Session != NULL) {
        QuicSessionUnregisterConnection(Connection->Session, Connection);
    }
    if (Connection->RemoteServerName != NULL) {
        QUIC_FREE(Connection->RemoteServerName);
    }
    if (Connection->OrigCID != NULL) {
        QUIC_FREE(Connection->OrigCID);
    }
    QUIC_DBG_ASSERT(Connection->AllocProcIndex < MsQuicLib.PartitionCount);
    QuicPoolFree(
        &MsQuicLib.PerProc[Connection->AllocProcIndex].ConnectionPool,
        Connection);
    EventWriteQuicConnDestroyed(Connection);

#if QUIC_TEST_MODE
    InterlockedDecrement(&MsQuicLib.ConnectionCount);
#endif
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnApplySettings(
    _In_ PQUIC_CONNECTION Connection,
    _In_ const QUIC_SETTINGS* Settings
    )
{
    Connection->State.UsePacing = Settings->PacingDefault;
    Connection->MaxAckDelayMs = Settings->MaxAckDelayMs;
    Connection->SmoothedRtt = MS_TO_US(Settings->InitialRttMs);
    Connection->DisconnectTimeoutUs = MS_TO_US(Settings->DisconnectTimeoutMs);
    Connection->IdleTimeoutMs = Settings->IdleTimeoutMs;
    Connection->KeepAliveIntervalMs = Settings->KeepAliveIntervalMs;

    uint8_t PeerStreamType =
        QuicConnIsServer(Connection) ?
            STREAM_ID_FLAG_IS_CLIENT : STREAM_ID_FLAG_IS_SERVER;
    if (Settings->BidiStreamCount != 0) {
        QuicStreamSetUpdateMaxCount(
            &Connection->Streams,
            PeerStreamType | STREAM_ID_FLAG_IS_BI_DIR,
            Settings->BidiStreamCount);
    }
    if (Settings->UnidiStreamCount != 0) {
        QuicStreamSetUpdateMaxCount(
            &Connection->Streams,
            PeerStreamType | STREAM_ID_FLAG_IS_UNI_DIR,
            Settings->UnidiStreamCount);
    }

    QuicSendApplySettings(&Connection->Send, Settings);
    QuicCongestionControlApplySettings(&Connection->CongestionControl, Settings);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnShutdown(
    _In_ PQUIC_CONNECTION Connection,
    _In_ uint32_t Flags,
    _In_ QUIC_VAR_INT ErrorCode
    )
{
    uint32_t CloseFlags = QUIC_CLOSE_APPLICATION;
    if (Flags & QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT ||
        (!Connection->State.Started && !QuicConnIsServer(Connection))) {
        CloseFlags |= QUIC_CLOSE_SILENT;
    }

    QuicConnCloseLocally(Connection, CloseFlags, ErrorCode, NULL);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnUninitialize(
    _In_ PQUIC_CONNECTION Connection
    )
{
    QUIC_TEL_ASSERT(Connection->State.HandleClosed);
    QUIC_TEL_ASSERT(!Connection->State.Uninitialized);

    Connection->State.Uninitialized = TRUE;

    //
    // Ensure we are shut down.
    //
    QuicConnShutdown(
        Connection,
        QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT,
        QUIC_ERROR_NO_ERROR);

    //
    // Remove all entries in the binding's lookup tables so we don't get any
    // more packets queued.
    //
    if (Connection->Binding != NULL) {
        QuicBindingRemoveConnection(Connection->Binding, Connection);
    }

    //
    // Clean up the packet space first, to return any deferred received
    // packets back to the binding.
    //
    for (uint32_t i = 0; i < ARRAYSIZE(Connection->Packets); i++) {
        if (Connection->Packets[i] != NULL) {
            QuicPacketSpaceUninitialize(Connection->Packets[i]);
            Connection->Packets[i] = NULL;
        }
    }

    //
    // Clean up the rest of the internal state.
    //
    QuicRangeUninitialize(&Connection->DecodedAckRanges);
    QuicCryptoUninitialize(&Connection->Crypto);
    QuicTimerWheelRemoveConnection(&Connection->Worker->TimerWheel, Connection);
    QuicOperationQueueClear(Connection->Worker, &Connection->OperQ);

    if (Connection->CloseReasonPhrase != NULL) {
        QUIC_FREE(Connection->CloseReasonPhrase);
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicConnCloseHandle(
    _In_ PQUIC_CONNECTION Connection
    )
{
    QUIC_TEL_ASSERT(!Connection->State.HandleClosed);
    Connection->State.HandleClosed = TRUE;
    Connection->ClientCallbackHandler = NULL;

    if (Connection->Session != NULL) {
        QuicSessionUnregisterConnection(Connection->Session, Connection);
    }

    EventWriteQuicConnHandleClosed(Connection);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicConnQueueTraceRundown(
    _In_ PQUIC_CONNECTION Connection
    )
{
    PQUIC_OPERATION Oper;
    if ((Oper = QuicOperationAlloc(Connection->Worker, QUIC_OPER_TYPE_TRACE_RUNDOWN)) != NULL) {
        QuicConnQueueOper(Connection, Oper);
    } else {
        EventWriteQuicAllocFailure("trace rundown operation", 0);
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnTraceRundownOper(
    _In_ PQUIC_CONNECTION Connection
    )
{
    EventWriteQuicConnRundown(
        Connection,
        QuicConnIsServer(Connection),
        Connection->Stats.CorrelationId);
    EventWriteQuicConnAssignWorker(Connection, Connection->Worker);
    if (Connection->Session != NULL) {
        EventWriteQuicConnRegisterSession(Connection, Connection->Session);
    }
    if (Connection->State.Started) {
        if (Connection->State.LocalAddressSet) {
            EventWriteQuicConnLocalAddrAdded(
                Connection,
                LOG_ADDR_LEN(Connection->LocalAddress),
                (const uint8_t*)&Connection->LocalAddress);
        }
        if (Connection->State.RemoteAddressSet) {
            EventWriteQuicConnRemoteAddrAdded(
                Connection,
                LOG_ADDR_LEN(Connection->RemoteAddress),
                (const uint8_t*)&Connection->RemoteAddress);
        }
        for (QUIC_SINGLE_LIST_ENTRY* Entry = Connection->SourceCIDs.Next;
                Entry != NULL;
                Entry = Entry->Next) {
            const QUIC_CID_HASH_ENTRY* SourceCID =
                QUIC_CONTAINING_RECORD(
                    Entry,
                    QUIC_CID_HASH_ENTRY,
                    Link);
            EventWriteQuicConnSourceCidAdded(
                Connection, SourceCID->CID.Length, SourceCID->CID.Data);
        }
        for (QUIC_LIST_ENTRY* Entry = Connection->DestCIDs.Flink;
                Entry != &Connection->DestCIDs;
                Entry = Entry->Flink) {
            const QUIC_CID_QUIC_LIST_ENTRY* DestCID =
                QUIC_CONTAINING_RECORD(
                    Entry,
                    QUIC_CID_QUIC_LIST_ENTRY,
                    Link);
            EventWriteQuicConnDestCidAdded(
                Connection, DestCID->CID.Length, DestCID->CID.Data);
        }
    }
    if (Connection->State.Connected) {
        QuicConnOnQuicVersionSet(Connection);
        EventWriteQuicConnHandshakeComplete(Connection);
    }
    if (Connection->State.HandleClosed) {
        EventWriteQuicConnHandleClosed(Connection);
    }
    if (Connection->State.Started) {
        QuicConnLogStatistics(Connection);
    }

    QuicStreamSetTraceRundown(&Connection->Streams);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
QuicConnIndicateEvent(
    _In_ PQUIC_CONNECTION Connection,
    _Inout_ QUIC_CONNECTION_EVENT* Event
    )
{
    QUIC_STATUS Status;
    if (!Connection->State.HandleClosed) {
        QUIC_CONN_VERIFY(Connection, Connection->ClientCallbackHandler != NULL);
        if (Connection->ClientCallbackHandler == NULL) {
            Status = QUIC_STATUS_INVALID_STATE;
            LogWarning("[conn][%p] Event silently discarded (no handler).", Connection);
        } else {
            uint64_t StartTime = QuicTimeUs64();
            Status =
                Connection->ClientCallbackHandler(
                    (HQUIC)Connection,
                    Connection->ClientContext,
                    Event);
            uint64_t EndTime = QuicTimeUs64();
            if (EndTime - StartTime > QUIC_MAX_CALLBACK_TIME_WARNING) {
                LogWarning("[conn][%p] App took excessive time (%llu us) in callback.",
                    Connection, (EndTime - StartTime));
                QUIC_TEL_ASSERTMSG_ARGS(
                    EndTime - StartTime < QUIC_MAX_CALLBACK_TIME_ERROR,
                    "App extremely long time in connection callback",
                    Connection->Registration == NULL ?
                        NULL : Connection->Registration->AppName,
                    Event->Type, 0);
            }
        }
    } else {
        Status = QUIC_STATUS_INVALID_STATE;
        LogWarning("[conn][%p] Event silently discarded.", Connection);
    }
    return Status;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicConnQueueOper(
    _In_ PQUIC_CONNECTION Connection,
    _In_ PQUIC_OPERATION Oper
    )
{
    if (QuicOperationEnqueue(&Connection->OperQ, Oper)) {
        //
        // The connection needs to be queued on the worker because this was the
        // first operation in our OperQ.
        //
        QuicWorkerQueueConnection(Connection->Worker, Connection);
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicConnQueueHighestPriorityOper(
    _In_ PQUIC_CONNECTION Connection,
    _In_ PQUIC_OPERATION Oper
    )
{
    if (QuicOperationEnqueueFront(&Connection->OperQ, Oper)) {
        //
        // The connection needs to be queued on the worker because this was the
        // first operation in our OperQ.
        //
        QuicWorkerQueueConnection(Connection->Worker, Connection);
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
BOOLEAN
QuicConnUpdateRtt(
    _In_ PQUIC_CONNECTION Connection,
    _In_ uint32_t LatestRtt
    )
{
    BOOLEAN RttUpdated;

    Connection->LatestRttSample = LatestRtt;
    if (LatestRtt < Connection->MinRtt) {
        Connection->MinRtt = LatestRtt;
    }
    if (LatestRtt > Connection->MaxRtt) {
        Connection->MaxRtt = LatestRtt;
    }

    if (!Connection->State.GotFirstRttSample) {
        Connection->State.GotFirstRttSample = TRUE;

        Connection->SmoothedRtt = LatestRtt;
        Connection->RttVariance = LatestRtt / 2;
        RttUpdated = TRUE;

    } else {
        uint32_t PrevRtt = Connection->SmoothedRtt;
        if (Connection->SmoothedRtt > LatestRtt) {
            Connection->RttVariance = (3 * Connection->RttVariance + Connection->SmoothedRtt - LatestRtt) / 4;
        } else {
            Connection->RttVariance = (3 * Connection->RttVariance + LatestRtt - Connection->SmoothedRtt) / 4;
        }
        Connection->SmoothedRtt = (7 * Connection->SmoothedRtt + LatestRtt) / 8;
        RttUpdated = PrevRtt != Connection->SmoothedRtt;
    }

    if (RttUpdated) {
        LogVerbose("[conn][%p] Updated Rtt=%u.%u ms, Var=%u.%u",
            Connection,
            Connection->SmoothedRtt / 1000, Connection->SmoothedRtt % 1000,
            Connection->RttVariance / 1000, Connection->RttVariance % 1000);
    }

    return RttUpdated;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_CID_HASH_ENTRY*
QuicConnGenerateNewSourceCid(
    _In_ PQUIC_CONNECTION Connection,
    _In_ BOOLEAN IsInitial
    )
{
    uint8_t TryCount = 0;
    QUIC_CID_HASH_ENTRY* SourceCID;

    if (!Connection->State.ShareBinding) {
        //
        // We aren't sharing the binding, therefore aren't actually using a CID.
        // No need to generate a new one.
        //
        return NULL;
    }

    //
    // Keep randomly generating new source CIDs until we find one that doesn't
    // collide with an existing one.
    //

    do {
        SourceCID =
            QuicCidNewRandomSource(
                Connection,
                Connection->ServerID,
                Connection->PartitionID,
                Connection->Registration->CidPrefixLength,
                Connection->Registration->CidPrefix,
                MSQUIC_CONNECTION_ID_LENGTH);
        if (SourceCID == NULL) {
            EventWriteQuicAllocFailure("new Src CID", sizeof(QUIC_CID_HASH_ENTRY) + MSQUIC_CONNECTION_ID_LENGTH);
            return NULL;
        }
        if (!QuicBindingAddSourceConnectionID(Connection->Binding, SourceCID)) {
            QUIC_FREE(SourceCID);
            SourceCID = NULL;
            if (++TryCount > QUIC_CID_MAX_COLLISION_RETRY) {
                EventWriteQuicConnError(Connection, "Too many CID collisions");
                return NULL;
            }
            LogVerbose("[conn][%p] CID collision, trying again.", Connection);
        }
    } while (SourceCID == NULL);

    EventWriteQuicConnSourceCidAdded(Connection, SourceCID->CID.Length, SourceCID->CID.Data);

    SourceCID->CID.SequenceNumber = Connection->NextSourceCidSequenceNumber++;
    if (SourceCID->CID.SequenceNumber > 0) {
        SourceCID->CID.NeedsToSend = TRUE;
        QuicSendSetSendFlag(&Connection->Send, QUIC_CONN_SEND_FLAG_NEW_CONNECTION_ID);
    }

    if (IsInitial) {
        SourceCID->CID.IsInitial = TRUE;
        QuicListPushEntry(&Connection->SourceCIDs, &SourceCID->Link);
    } else {
        QUIC_SINGLE_LIST_ENTRY** Tail = &Connection->SourceCIDs.Next;
        while (*Tail != NULL) {
            Tail = &(*Tail)->Next;
        }
        *Tail = &SourceCID->Link;
        SourceCID->Link.Next = NULL;
    }

    return SourceCID;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnRetireCurrentDestCid(
    _In_ PQUIC_CONNECTION Connection
    )
{
    for (QUIC_LIST_ENTRY* Entry = Connection->DestCIDs.Flink;
            Entry != &Connection->DestCIDs;
            Entry = Entry->Flink) {
        QUIC_CID_QUIC_LIST_ENTRY* DestCid =
            QUIC_CONTAINING_RECORD(
                Entry,
                QUIC_CID_QUIC_LIST_ENTRY,
                Link);
        if (DestCid->CID.Length == 0) {
            LogWarning("[conn][%p] Can't retire current CID because it's zero length", Connection);
            break;
        }
        if (DestCid->CID.Retired) {
            continue;
        }
        if (Entry->Flink == &Connection->DestCIDs) {
            LogWarning("[conn][%p] Can't retire current CID because we don't have a replacement", Connection);
            break;
        }
        EventWriteQuicConnDestCidRemoved(
            Connection, DestCid->CID.Length, DestCid->CID.Data);
        DestCid->CID.Retired = TRUE;
        DestCid->CID.NeedsToSend = TRUE;
        QuicSendSetSendFlag(
            &Connection->Send, QUIC_CONN_SEND_FLAG_RETIRE_CONNECTION_ID);
        break;
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnTimerSet(
    _Inout_ PQUIC_CONNECTION Connection,
    _In_ QUIC_CONN_TIMER_TYPE Type,
    _In_ uint64_t Delay
    )
{
    uint64_t NewExpirationTime = QuicTimeUs64() + MS_TO_US(Delay);

    //
    // Find the current and new index in the timer array for this timer.
    //

    uint32_t NewIndex = ARRAYSIZE(Connection->Timers);
    uint32_t CurIndex = 0;
    for (uint32_t i = 0; i < ARRAYSIZE(Connection->Timers); ++i) {
        if (Connection->Timers[i].Type == Type) {
            CurIndex = i;
        }
        if (i < NewIndex &&
            NewExpirationTime < Connection->Timers[i].ExpirationTime) {
            NewIndex = i;
        }
    }

    if (NewIndex < CurIndex) {
        //
        // Need to move the timer forward in the array.
        //
        QuicMoveMemory(
            Connection->Timers + NewIndex + 1,
            Connection->Timers + NewIndex,
            sizeof(QUIC_CONN_TIMER_ENTRY) * (CurIndex - NewIndex));
        Connection->Timers[NewIndex].Type = Type;
        Connection->Timers[NewIndex].ExpirationTime = NewExpirationTime;

    } else if (NewIndex > CurIndex + 1) {
        //
        // Need to move the timer back in the array. Ignore changes that
        // wouldn't actually move it at all.
        //
        QuicMoveMemory(
            Connection->Timers + CurIndex,
            Connection->Timers + CurIndex + 1,
            sizeof(QUIC_CONN_TIMER_ENTRY) * (NewIndex - CurIndex - 1));
        Connection->Timers[NewIndex - 1].Type = Type;
        Connection->Timers[NewIndex - 1].ExpirationTime = NewExpirationTime;
    } else {
        //
        // Didn't move, so just update the expiration time.
        //
        Connection->Timers[CurIndex].ExpirationTime = NewExpirationTime;
        NewIndex = CurIndex;
    }

    if (NewIndex == 0) {
        //
        // The first timer was updated, so make sure the timer wheel is updated.
        //
        QuicTimerWheelUpdateConnection(&Connection->Worker->TimerWheel, Connection);
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnTimerCancel(
    _Inout_ PQUIC_CONNECTION Connection,
    _In_ QUIC_CONN_TIMER_TYPE Type
    )
{
    for (uint32_t i = 0;
        i < ARRAYSIZE(Connection->Timers) &&
            Connection->Timers[i].ExpirationTime != UINT64_MAX;
        ++i) {

        //
        // Find the correct timer (by type), invalidate it, and move it past all
        // the other valid timers.
        //

        if (Connection->Timers[i].Type == Type) {

            if (Connection->Timers[i].ExpirationTime != UINT64_MAX) {

                //
                // Find the end of the valid timers (if any more).
                //

                uint32_t j = i + 1;
                while (j < ARRAYSIZE(Connection->Timers) &&
                    Connection->Timers[j].ExpirationTime != UINT64_MAX) {
                    ++j;
                }

                if (j == i + 1) {
                    //
                    // No more valid timers, just invalidate this one and leave it
                    // where it is.
                    //
                    Connection->Timers[i].ExpirationTime = UINT64_MAX;
                } else {

                    //
                    // Move the valid timers forward and then put this timer after
                    // them.
                    //
                    QuicMoveMemory(
                        Connection->Timers + i,
                        Connection->Timers + i + 1,
                        sizeof(QUIC_CONN_TIMER_ENTRY) * (j - i - 1));
                    Connection->Timers[j - 1].Type = Type;
                    Connection->Timers[j - 1].ExpirationTime = UINT64_MAX;
                }

                if (i == 0) {
                    //
                    // The first timer was removed, so make sure the timer wheel is updated.
                    //
                    QuicTimerWheelUpdateConnection(&Connection->Worker->TimerWheel, Connection);
                }
            }

            break;
        }
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnTimerExpired(
    _Inout_ PQUIC_CONNECTION Connection,
    _In_ uint64_t TimeNow
    )
{
    uint32_t i = 0;
    QUIC_CONN_TIMER_ENTRY Temp[QUIC_CONN_TIMER_COUNT];
    BOOLEAN FlushSendImmediate = FALSE;

    while (i < ARRAYSIZE(Connection->Timers) &&
           Connection->Timers[i].ExpirationTime <= TimeNow) {
        Connection->Timers[i].ExpirationTime = UINT64_MAX;
        ++i;
    }

    QUIC_DBG_ASSERT(i != 0);

    QuicCopyMemory(
        Temp,
        Connection->Timers,
        i * sizeof(QUIC_CONN_TIMER_ENTRY));
    if (i < ARRAYSIZE(Connection->Timers)) {
        QuicMoveMemory(
            Connection->Timers,
            Connection->Timers + i,
            (QUIC_CONN_TIMER_COUNT - i) * sizeof(QUIC_CONN_TIMER_ENTRY));
        QuicCopyMemory(
            Connection->Timers + (QUIC_CONN_TIMER_COUNT - i),
            Temp,
            i * sizeof(QUIC_CONN_TIMER_ENTRY));
    }

    for (uint32_t j = 0; j < i; ++j) {
        const char* TimerNames[] = {
            "PACING",
            "ACK_DELAY",
            "LOSS_DETECTION",
            "KEEP_ALIVE",
            "IDLE",
            "SHUTDOWN",
            "INVALID"
        };
        LogVerbose("[conn][%p] %s timer expired", Connection, TimerNames[Temp[j].Type]);
        if (Temp[j].Type == QUIC_CONN_TIMER_ACK_DELAY) {
            EventWriteQuicConnExecTimerOper(Connection, QUIC_CONN_TIMER_ACK_DELAY);
            QuicSendProcessDelayedAckTimer(&Connection->Send);
            FlushSendImmediate = TRUE;
        } else if (Temp[j].Type == QUIC_CONN_TIMER_PACING) {
            EventWriteQuicConnExecTimerOper(Connection, QUIC_CONN_TIMER_PACING);
            FlushSendImmediate = TRUE;
        } else {
            PQUIC_OPERATION Oper;
            if ((Oper = QuicOperationAlloc(Connection->Worker, QUIC_OPER_TYPE_TIMER_EXPIRED)) != NULL) {
                Oper->TIMER_EXPIRED.Type = Temp[j].Type;
                QuicConnQueueOper(Connection, Oper);
            } else {
                EventWriteQuicAllocFailure("expired timer operation", 0);
            }
        }
    }

    QuicTimerWheelUpdateConnection(&Connection->Worker->TimerWheel, Connection);

    if (FlushSendImmediate) {
        //
        // We don't want to actually call the flush immediate above as it can
        // cause a new timer to be inserted, messing up timer loop.
        //
        (void)QuicSendProcessFlushSendOperation(&Connection->Send, TRUE);
    }
}

//
// Sends a shutdown being notification to the app, which represents the first
// indication that we know the connection is closed (locally or remotely).
//
_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnIndicateShutdownBegin(
    _In_ PQUIC_CONNECTION Connection
    )
{
    QUIC_CONNECTION_EVENT Event;
    if (Connection->State.AppClosed) {
        Event.Type = QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER;
        Event.SHUTDOWN_INITIATED_BY_PEER.ErrorCode = Connection->CloseErrorCode;
        LogVerbose("[conn][%p] Indicating QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER [0x%llx]",
            Connection, Event.SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
    } else {
        Event.Type = QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT;
        Event.SHUTDOWN_INITIATED_BY_TRANSPORT.Status = Connection->CloseStatus;
        LogVerbose("[conn][%p] Indicating QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT [0x%x]",
            Connection, Event.SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
    }
    (void)QuicConnIndicateEvent(Connection, &Event);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnOnShutdownComplete(
    _In_ PQUIC_CONNECTION Connection
    )
{
    if (Connection->State.HandleShutdown) {
        return;
    }
    Connection->State.HandleShutdown = TRUE;

    EventWriteQuicConnShutdownComplete(
        Connection, Connection->State.ShutdownCompleteTimedOut);

    if (Connection->State.ExternalOwner == FALSE) {

        //
        // If the connection was never indicated to the application, then it
        // needs to be cleaned up now.
        //

        QuicConnCloseHandle(Connection);
        QuicConnRelease(Connection, QUIC_CONN_REF_HANDLE_OWNER);

    } else {

        QUIC_CONNECTION_EVENT Event;
        Event.Type = QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE;
        Event.SHUTDOWN_COMPLETE.PeerAcknowledgedShutdown =
            !Connection->State.ShutdownCompleteTimedOut;

        LogVerbose("[conn][%p] Indicating QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE",
            Connection);
        (void)QuicConnIndicateEvent(Connection, &Event);
    }

    if (Connection->Binding != NULL) {
        QuicBindingRemoveConnection(Connection->Binding, Connection);
    }
}

QUIC_STATUS
QuicErrorCodeToStatus(
    QUIC_VAR_INT ErrorCode
    )
{
    switch (ErrorCode) {
    case QUIC_ERROR_NO_ERROR:           return QUIC_STATUS_SUCCESS;
    case QUIC_ERROR_SERVER_BUSY:        return QUIC_STATUS_SERVER_BUSY;
    case QUIC_ERROR_PROTOCOL_VIOLATION: return QUIC_STATUS_PROTOCOL_ERROR;
    default:                            return QUIC_STATUS_INTERNAL_ERROR;
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnTryClose(
    _In_ PQUIC_CONNECTION Connection,
    _In_ uint32_t Flags,
    _In_ uint64_t ErrorCode,
    _In_reads_bytes_opt_(RemoteReasonPhraseLength)
         const char* RemoteReasonPhrase,
    _In_ uint16_t RemoteReasonPhraseLength
    )
{
    BOOLEAN ClosedRemotely = !!(Flags & QUIC_CLOSE_REMOTE);
    BOOLEAN SilentClose = !!(Flags & QUIC_CLOSE_SILENT);

    if ((ClosedRemotely && Connection->State.ClosedRemotely) ||
        (!ClosedRemotely && Connection->State.ClosedLocally)) {
        //
        // Already closed.
        //
        if (SilentClose &&
            Connection->State.ClosedLocally &&
            !Connection->State.ClosedRemotely) {
            //
            // Silent close forced after we already started the close process.
            //
            Connection->State.ShutdownCompleteTimedOut = FALSE;
            Connection->State.SendShutdownCompleteNotif = TRUE;
        }
        return;
    }

    BOOLEAN ResultQuicStatus = !!(Flags & QUIC_CLOSE_QUIC_STATUS);

    BOOLEAN IsFirstCloseForConnection = TRUE;

    if (ClosedRemotely && !Connection->State.ClosedLocally) {
        //
        // Peer closed first.
        //

        if (!Connection->State.Connected && !QuicConnIsServer(Connection)) {
            //
            // If the server terminates a connection attempt, close immediately
            // without going through the draining period.
            //
            SilentClose = TRUE;
        }

        if (!SilentClose) {
            //
            // Enter 'draining period' to flush out any leftover packets.
            //
            QuicConnTimerSet(
                Connection,
                QUIC_CONN_TIMER_SHUTDOWN,
                max(15, US_TO_MS(Connection->SmoothedRtt * 2)));

            QuicSendSetSendFlag(
                &Connection->Send,
                QUIC_CONN_SEND_FLAG_CONNECTION_CLOSE);
        }

    } else if (!ClosedRemotely && !Connection->State.ClosedRemotely) {

        //
        // Locally closed first.
        //

        if (!SilentClose) {
            //
            // Enter 'closing period' to wait for a (optional) connection close
            // response.
            //
            uint32_t Pto =
                US_TO_MS(QuicLossDetectionComputeProbeTimeout(
                    &Connection->LossDetection,
                    QUIC_CLOSE_PTO_COUNT));
            QuicConnTimerSet(
                Connection,
                QUIC_CONN_TIMER_SHUTDOWN,
                Pto);

            QuicSendSetSendFlag(
                &Connection->Send,
                (Flags & QUIC_CLOSE_APPLICATION) ?
                    QUIC_CONN_SEND_FLAG_APPLICATION_CLOSE :
                    QUIC_CONN_SEND_FLAG_CONNECTION_CLOSE);
        }

    } else {

        LogInfo("[conn][%p] Connection close complete.", Connection);

        //
        // Peer acknowledged our local close.
        //

        if (!QuicConnIsServer(Connection)) {
            //
            // Client side can immediately clean up once its close frame was
            // acknowledged because we will close the socket during clean up,
            // which will automatically handle any leftover packets that
            // get received afterward by dropping them.
            //

        } else if (!SilentClose) {
            //
            // Server side transitions from the 'closing period' to the
            // 'draining period' and waits an additional 2 RTT just to make
            // sure all leftover packets have been flushed out.
            //
            QuicConnTimerSet(
                Connection,
                QUIC_CONN_TIMER_SHUTDOWN,
                max(15, US_TO_MS(Connection->SmoothedRtt * 2)));
        }

        IsFirstCloseForConnection = FALSE;
    }

    if (ClosedRemotely) {
        Connection->State.ClosedRemotely = TRUE;
    } else {
        Connection->State.ClosedLocally = TRUE;
    }

    if (IsFirstCloseForConnection) {
        //
        // Default to the timed out state.
        //
        Connection->State.ShutdownCompleteTimedOut = TRUE;

        //
        // Cancel all non-shutdown related timers.
        //
        for (QUIC_CONN_TIMER_TYPE TimerType = QUIC_CONN_TIMER_IDLE;
            TimerType < QUIC_CONN_TIMER_SHUTDOWN;
            ++TimerType) {
            QuicConnTimerCancel(Connection, TimerType);
        }

        if (ResultQuicStatus) {
            Connection->CloseStatus = (QUIC_STATUS)ErrorCode;
            Connection->CloseErrorCode = QUIC_ERROR_INTERNAL_ERROR;
        } else {
            Connection->CloseStatus = QuicErrorCodeToStatus(ErrorCode);
            Connection->CloseErrorCode = ErrorCode;
        }

        if (Flags & QUIC_CLOSE_APPLICATION) {
            Connection->State.AppClosed = TRUE;
        }

        if (Flags & QUIC_CLOSE_SEND_NOTIFICATION &&
            Connection->State.ExternalOwner) {
            QuicConnIndicateShutdownBegin(Connection);
        }

        if (Connection->CloseReasonPhrase != NULL) {
            QUIC_FREE(Connection->CloseReasonPhrase);
            Connection->CloseReasonPhrase = NULL;
        }

        if (RemoteReasonPhraseLength != 0) {
            Connection->CloseReasonPhrase =
                QUIC_ALLOC_NONPAGED(RemoteReasonPhraseLength + 1);
            if (Connection->CloseReasonPhrase != NULL) {
                QuicCopyMemory(
                    Connection->CloseReasonPhrase,
                    RemoteReasonPhrase,
                    RemoteReasonPhraseLength);
                Connection->CloseReasonPhrase[RemoteReasonPhraseLength] = 0;
            } else {
                EventWriteQuicAllocFailure("close reason", RemoteReasonPhraseLength + 1);
            }
        }

        if (Connection->State.Started) {
            QuicConnLogStatistics(Connection);
        }

        if (Flags & QUIC_CLOSE_APPLICATION) {
            EventWriteQuicConnAppShutdown(
                Connection,
                ErrorCode,
                ClosedRemotely);
        } else {
            EventWriteQuicConnTransportShutdown(
                Connection,
                ErrorCode,
                ClosedRemotely,
                !!(Flags & QUIC_CLOSE_QUIC_STATUS));
        }

        //
        // On initial close, we must shut down all the current streams.
        //
        QuicStreamSetShutdown(&Connection->Streams);
    }

    if (SilentClose ||
        (Connection->State.ClosedRemotely && Connection->State.ClosedLocally)) {
        Connection->State.ShutdownCompleteTimedOut = FALSE;
        Connection->State.SendShutdownCompleteNotif = TRUE;
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnProcessShutdownTimerOperation(
    _In_ PQUIC_CONNECTION Connection
    )
{
    //
    // We now consider the peer closed, even if they didn't respond to our close
    // frame.
    //
    Connection->State.ClosedRemotely = TRUE;

    //
    // Now that we are closed in both directions, we can complete the shutdown
    // of the connection.
    //
    Connection->State.SendShutdownCompleteNotif = TRUE;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnCloseLocally(
    _In_ PQUIC_CONNECTION Connection,
    _In_ uint32_t Flags,
    _In_ uint64_t ErrorCode,
    _In_opt_z_ const char* ErrorMsg
    )
{
    QUIC_DBG_ASSERT(ErrorMsg == NULL || strlen(ErrorMsg) < UINT16_MAX);
    QuicConnTryClose(
        Connection,
        Flags,
        ErrorCode,
        ErrorMsg,
        ErrorMsg == NULL ? 0 : (uint16_t)strlen(ErrorMsg));
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnOnQuicVersionSet(
    _In_ PQUIC_CONNECTION Connection
    )
{
    EventWriteQuicConnVersionSet(Connection, Connection->Stats.QuicVersion);

    switch (Connection->Stats.QuicVersion) {
    case QUIC_VERSION_DRAFT_23:
    case QUIC_VERSION_MS_1:
    default:
        Connection->State.HeaderProtectionEnabled = TRUE;
        break;
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
QuicConnStart(
    _In_ PQUIC_CONNECTION Connection,
    _In_ QUIC_ADDRESS_FAMILY Family,
    _In_opt_z_ const char* ServerName,
    _In_ uint16_t ServerPort // Host byte order
    )
{
    QUIC_STATUS Status;

    QUIC_TEL_ASSERT(Connection->Binding == NULL);
    Connection->Stats.Timing.Start = QuicTimeUs64();
    EventWriteQuicConnHandshakeStart(Connection);

    if (!Connection->State.RemoteAddressSet) {

        QUIC_DBG_ASSERT(ServerName != NULL);
        QuicAddrSetFamily(&Connection->RemoteAddress, Family);
        
#ifdef QUIC_COMPARTMENT_ID
        BOOLEAN RevertCompartmentId = FALSE;
        QUIC_COMPARTMENT_ID PrevCompartmentId = QuicCompartmentIdGetCurrent();
        if (PrevCompartmentId != Connection->Session->CompartmentId) {
            Status = QuicCompartmentIdSetCurrent(Connection->Session->CompartmentId);
            if (QUIC_FAILED(Status)) {
                EventWriteQuicConnErrorStatus(Connection, Status, "Set current compartment Id");
                goto Exit;
            }
            RevertCompartmentId = TRUE;
        }
#endif

        //
        // Resolve the server name to IP address.
        //
        Status =
            QuicDataPathResolveAddress(
                MsQuicLib.Datapath,
                ServerName,
                &Connection->RemoteAddress);

#ifdef QUIC_COMPARTMENT_ID
        if (RevertCompartmentId) {
            (void)QuicCompartmentIdSetCurrent(PrevCompartmentId);
        }
#endif

        if (QUIC_FAILED(Status)) {
            goto Exit;
        }

        Connection->State.RemoteAddressSet = TRUE;
    }

    QuicAddrSetPort(&Connection->RemoteAddress, ServerPort);
    EventWriteQuicConnRemoteAddrAdded(
        Connection,
        LOG_ADDR_LEN(Connection->RemoteAddress),
        (const uint8_t*)&Connection->RemoteAddress);

    //
    // Get the binding for the current local & remote addresses.
    //
    Status =
        QuicLibraryGetBinding(
            Connection->Session,
            Connection->State.ShareBinding,
            Connection->State.LocalAddressSet ? &Connection->LocalAddress : NULL,
            &Connection->RemoteAddress,
            &Connection->Binding);
    if (QUIC_FAILED(Status)) {
        goto Exit;
    }

    InterlockedIncrement(&Connection->Binding->HandshakeConnections);
    InterlockedExchangeAdd64(
        (LONG64*)&MsQuicLib.CurrentHandshakeMemoryUsage,
        (LONG64)QUIC_CONN_HANDSHAKE_MEMORY_USAGE);

    //
    // Clients only need to generate a non-zero length source CID if it
    // intends to share the UDP binding.
    //
    QUIC_CID_HASH_ENTRY* SourceCID =
        QuicCidNewRandomSource(
            Connection,
            0,
            Connection->PartitionID,
            Connection->Registration->CidPrefixLength,
            Connection->Registration->CidPrefix,
            Connection->State.ShareBinding ?
                MSQUIC_CONNECTION_ID_LENGTH : 0);
    if (SourceCID == NULL) {
        Status = QUIC_STATUS_OUT_OF_MEMORY;
        goto Exit;
    }

    Connection->NextSourceCidSequenceNumber++;
    EventWriteQuicConnSourceCidAdded(Connection, SourceCID->CID.Length, SourceCID->CID.Data);
    QuicListPushEntry(&Connection->SourceCIDs, &SourceCID->Link);

    if (!QuicBindingAddSourceConnectionID(Connection->Binding, SourceCID)) {
        InterlockedDecrement(&Connection->Binding->HandshakeConnections);
        InterlockedExchangeAdd64(
            (LONG64*)&MsQuicLib.CurrentHandshakeMemoryUsage,
            -1 * (LONG64)QUIC_CONN_HANDSHAKE_MEMORY_USAGE);
        QuicLibraryReleaseBinding(Connection->Binding);
        Connection->Binding = NULL;
        Status = QUIC_STATUS_OUT_OF_MEMORY;
        goto Exit;
    }

    Connection->State.LocalAddressSet = TRUE;
    QuicDataPathBindingGetLocalAddress(
        Connection->Binding->DatapathBinding,
        &Connection->LocalAddress);
    EventWriteQuicConnLocalAddrAdded(
        Connection,
        LOG_ADDR_LEN(Connection->LocalAddress),
        (const uint8_t*)&Connection->LocalAddress);

    //
    // Save the server name.
    //
    Connection->RemoteServerName = ServerName;
    ServerName = NULL;

    //
    // Start the handshake.
    //
    Status = QuicConnInitializeCrypto(Connection);
    if (QUIC_FAILED(Status)) {
        goto Exit;
    }

    Connection->State.Started = TRUE;

Exit:

    if (ServerName != NULL) {
        QUIC_FREE(ServerName);
    }

    if (QUIC_FAILED(Status)) {
        QuicConnCloseLocally(
            Connection,
            QUIC_CLOSE_INTERNAL_SILENT | QUIC_CLOSE_QUIC_STATUS,
            (uint64_t)Status,
            NULL);
    }

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnRestart(
    _In_ PQUIC_CONNECTION Connection,
    _In_ BOOLEAN CompleteReset
    )
{
    QUIC_TEL_ASSERT(Connection->State.Started);

    LogInfo("[conn][%p] Restart (CompleteReset=%hu)", Connection, CompleteReset);

    if (CompleteReset) {
        //
        // Don't reset current RTT measurements unless doing a full reset.
        //
        Connection->State.GotFirstRttSample = FALSE;
        Connection->SmoothedRtt = MS_TO_US(QUIC_INITIAL_RTT);
        Connection->RttVariance = 0;
    }

    for (uint32_t i = 0; i < ARRAYSIZE(Connection->Packets); ++i) {
        QUIC_DBG_ASSERT(Connection->Packets[i] != NULL);
        QuicPacketSpaceReset(Connection->Packets[i]);
    }

    QuicCongestionControlReset(&Connection->CongestionControl);
    QuicLossDetectionReset(&Connection->LossDetection);
    QuicSendReset(&Connection->Send);
    QuicCryptoReset(&Connection->Crypto, CompleteReset);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
QuicConnInitializeCrypto(
    _In_ PQUIC_CONNECTION Connection
    )
{
    QUIC_STATUS Status;
    BOOLEAN CryptoInitialized = FALSE;

    Status = QuicCryptoInitialize(&Connection->Crypto);
    if (QUIC_FAILED(Status)) {
        goto Error;
    }
    CryptoInitialized = TRUE;

    if (!QuicConnIsServer(Connection)) {
        Status = QuicConnHandshakeConfigure(Connection, NULL);
        if (QUIC_FAILED(Status)) {
            goto Error;
        }
    }

    if (Connection->KeepAliveIntervalMs != 0) {
        //
        // Now that we are starting the connection, start the keep alive timer
        // if enabled.
        //
        QuicConnTimerSet(
            Connection,
            QUIC_CONN_TIMER_KEEP_ALIVE,
            Connection->KeepAliveIntervalMs);
    }

Error:

    if (QUIC_FAILED(Status)) {
        if (CryptoInitialized) {
            QuicCryptoUninitialize(&Connection->Crypto);
        }
    }

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
QuicConnHandshakeConfigure(
    _In_ PQUIC_CONNECTION Connection,
    _In_opt_ QUIC_SEC_CONFIG* SecConfig
    )
{
    QUIC_STATUS Status;
    QUIC_TRANSPORT_PARAMETERS LocalTP = { 0 };

    QUIC_TEL_ASSERT(Connection->Session != NULL);

    if (QuicConnIsServer(Connection)) {

        QUIC_TEL_ASSERT(SecConfig != NULL);

        LocalTP.InitialMaxStreamDataBidiLocal = Connection->Session->Settings.StreamRecvWindowDefault;
        LocalTP.InitialMaxStreamDataBidiRemote = Connection->Session->Settings.StreamRecvWindowDefault;
        LocalTP.InitialMaxStreamDataUni = Connection->Session->Settings.StreamRecvWindowDefault;
        LocalTP.InitialMaxData = Connection->Send.MaxData;
        LocalTP.ActiveConnectionIdLimit = QUIC_ACTIVE_CONNECTION_ID_LIMIT;
        LocalTP.Flags =
            QUIC_TP_FLAG_INITIAL_MAX_DATA |
            QUIC_TP_FLAG_INITIAL_MAX_STRM_DATA_BIDI_LOCAL |
            QUIC_TP_FLAG_INITIAL_MAX_STRM_DATA_BIDI_REMOTE |
            QUIC_TP_FLAG_INITIAL_MAX_STRM_DATA_UNI |
            QUIC_TP_FLAG_MAX_PACKET_SIZE |
            QUIC_TP_FLAG_MAX_ACK_DELAY |
            QUIC_TP_FLAG_DISABLE_ACTIVE_MIGRATION |
            QUIC_TP_FLAG_ACTIVE_CONNECTION_ID_LIMIT;
        LocalTP.MaxPacketSize =
            MaxUdpPayloadSizeFromMTU(QuicDataPathBindingGetLocalMtu(Connection->Binding->DatapathBinding));

        if (Connection->IdleTimeoutMs != 0) {
            LocalTP.Flags |= QUIC_TP_FLAG_IDLE_TIMEOUT;
            LocalTP.IdleTimeout = Connection->IdleTimeoutMs;
        }

        LocalTP.MaxAckDelay =
            Connection->MaxAckDelayMs + (uint32_t)MsQuicLib.TimerResolutionMs;

        const QUIC_CID_HASH_ENTRY* SourceCID =
            QUIC_CONTAINING_RECORD(
                Connection->SourceCIDs.Next,
                QUIC_CID_HASH_ENTRY,
                Link);
        LocalTP.Flags |= QUIC_TP_FLAG_STATELESS_RESET_TOKEN;
        QuicBindingGenerateStatelessResetToken(
            Connection->Binding,
            SourceCID->CID.Data,
            LocalTP.StatelessResetToken);

        if (Connection->AckDelayExponent != QUIC_DEFAULT_ACK_DELAY_EXPONENT) {
            LocalTP.Flags |= QUIC_TP_FLAG_ACK_DELAY_EXPONENT;
            LocalTP.AckDelayExponent = Connection->AckDelayExponent;
        }

        if (Connection->Streams.Types[STREAM_ID_FLAG_IS_CLIENT | STREAM_ID_FLAG_IS_BI_DIR].MaxTotalStreamCount) {
            LocalTP.Flags |= QUIC_TP_FLAG_INITIAL_MAX_STRMS_BIDI;
            LocalTP.InitialMaxBidiStreams =
                Connection->Streams.Types[STREAM_ID_FLAG_IS_CLIENT | STREAM_ID_FLAG_IS_BI_DIR].MaxTotalStreamCount;
        }

        if (Connection->Streams.Types[STREAM_ID_FLAG_IS_CLIENT | STREAM_ID_FLAG_IS_UNI_DIR].MaxTotalStreamCount) {
            LocalTP.Flags |= QUIC_TP_FLAG_INITIAL_MAX_STRMS_UNI;
            LocalTP.InitialMaxUniStreams =
                Connection->Streams.Types[STREAM_ID_FLAG_IS_CLIENT | STREAM_ID_FLAG_IS_UNI_DIR].MaxTotalStreamCount;
        }

        if (Connection->OrigCID != NULL) {
            LocalTP.Flags |= QUIC_TP_FLAG_ORIGINAL_CONNECTION_ID;
            LocalTP.OriginalConnectionIDLength = Connection->OrigCID->Length;
            QuicCopyMemory(
                LocalTP.OriginalConnectionID,
                Connection->OrigCID->Data,
                Connection->OrigCID->Length);
            QUIC_FREE(Connection->OrigCID);
            Connection->OrigCID = NULL;
        }

    } else {

        uint32_t InitialQuicVersion = QUIC_VERSION_LATEST;
        if (Connection->RemoteServerName != NULL &&
            QuicSessionServerCacheGetState(
                Connection->Session,
                Connection->RemoteServerName,
                &InitialQuicVersion,
                &Connection->PeerTransportParams,
                &SecConfig)) {

            LogVerbose("[conn][%p] Found server cached state", Connection);
            QuicConnProcessPeerTransportParameters(Connection, TRUE);
        }

        if (Connection->Stats.QuicVersion == 0) {
            //
            // Only initialize the version if not already done (by the
            // application layer).
            //
            Connection->Stats.QuicVersion = InitialQuicVersion;
        }
        QuicConnOnQuicVersionSet(Connection);

        if (SecConfig == NULL) {
            Status =
                QuicTlsClientSecConfigCreate(
                    Connection->ServerCertValidationFlags,
                    &SecConfig);
            if (QUIC_FAILED(Status)) {
                EventWriteQuicConnErrorStatus(Connection, Status, "QuicTlsClientSecConfigCreate");
                goto Error;
            }
        }

        LocalTP.InitialMaxStreamDataBidiLocal = Connection->Session->Settings.StreamRecvWindowDefault;
        LocalTP.InitialMaxStreamDataBidiRemote = Connection->Session->Settings.StreamRecvWindowDefault;
        LocalTP.InitialMaxStreamDataUni = Connection->Session->Settings.StreamRecvWindowDefault;
        LocalTP.InitialMaxData = Connection->Send.MaxData;
        LocalTP.ActiveConnectionIdLimit = QUIC_ACTIVE_CONNECTION_ID_LIMIT;
        LocalTP.Flags =
            QUIC_TP_FLAG_INITIAL_MAX_DATA |
            QUIC_TP_FLAG_INITIAL_MAX_STRM_DATA_BIDI_LOCAL |
            QUIC_TP_FLAG_INITIAL_MAX_STRM_DATA_BIDI_REMOTE |
            QUIC_TP_FLAG_INITIAL_MAX_STRM_DATA_UNI |
            QUIC_TP_FLAG_MAX_PACKET_SIZE |
            QUIC_TP_FLAG_MAX_ACK_DELAY |
            QUIC_TP_FLAG_DISABLE_ACTIVE_MIGRATION |
            QUIC_TP_FLAG_ACTIVE_CONNECTION_ID_LIMIT;
        LocalTP.MaxPacketSize =
            MaxUdpPayloadSizeFromMTU(QuicDataPathBindingGetLocalMtu(Connection->Binding->DatapathBinding));

        if (Connection->IdleTimeoutMs != 0) {
            LocalTP.Flags |= QUIC_TP_FLAG_IDLE_TIMEOUT;
            LocalTP.IdleTimeout = Connection->IdleTimeoutMs;
        }

        LocalTP.MaxAckDelay =
            Connection->MaxAckDelayMs + MsQuicLib.TimerResolutionMs; // TODO - Include queue delay?

        if (Connection->AckDelayExponent != QUIC_DEFAULT_ACK_DELAY_EXPONENT) {
            LocalTP.Flags |= QUIC_TP_FLAG_ACK_DELAY_EXPONENT;
            LocalTP.AckDelayExponent = Connection->AckDelayExponent;
        }

        if (Connection->Streams.Types[STREAM_ID_FLAG_IS_SERVER | STREAM_ID_FLAG_IS_BI_DIR].MaxTotalStreamCount) {
            LocalTP.Flags |= QUIC_TP_FLAG_INITIAL_MAX_STRMS_BIDI;
            LocalTP.InitialMaxBidiStreams =
                Connection->Streams.Types[STREAM_ID_FLAG_IS_SERVER | STREAM_ID_FLAG_IS_BI_DIR].MaxTotalStreamCount;
        }

        if (Connection->Streams.Types[STREAM_ID_FLAG_IS_SERVER | STREAM_ID_FLAG_IS_UNI_DIR].MaxTotalStreamCount) {
            LocalTP.Flags |= QUIC_TP_FLAG_INITIAL_MAX_STRMS_UNI;
            LocalTP.InitialMaxUniStreams =
                Connection->Streams.Types[STREAM_ID_FLAG_IS_SERVER | STREAM_ID_FLAG_IS_UNI_DIR].MaxTotalStreamCount;
        }
    }

    Status =
        QuicCryptoInitializeTls(
            &Connection->Crypto,
            SecConfig,
            &LocalTP);
    QuicTlsSecConfigRelease(SecConfig); // No longer need local ref.

Error:

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnProcessPeerTransportParameters(
    _In_ PQUIC_CONNECTION Connection,
    _In_ BOOLEAN FromCache
    )
{
    LogInfo("[conn][%p] Peer Transport Parameters Set", Connection);

    if (Connection->PeerTransportParams.Flags & QUIC_TP_FLAG_STATELESS_RESET_TOKEN) {
        QUIC_DBG_ASSERT(!QuicListIsEmpty(&Connection->DestCIDs));
        QUIC_DBG_ASSERT(!QuicConnIsServer(Connection));
        QUIC_CID_QUIC_LIST_ENTRY* DestCID =
            QUIC_CONTAINING_RECORD(
                Connection->DestCIDs.Flink,
                QUIC_CID_QUIC_LIST_ENTRY,
                Link);
        QuicCopyMemory(
            DestCID->ResetToken,
            Connection->PeerTransportParams.StatelessResetToken,
            QUIC_STATELESS_RESET_TOKEN_LENGTH);
        DestCID->CID.HasResetToken = TRUE;
    }

    if (Connection->PeerTransportParams.Flags & QUIC_TP_FLAG_PREFERRED_ADDRESS) {
        /* TODO - Platform independent logging
        if (QuicAddrGetFamily(&Connection->PeerTransportParams.PreferredAddress) == AF_INET) {
            LogInfo("[conn][%p] Peer configured preferred address %!IPV4ADDR!:%d",
                Connection,
                &Connection->PeerTransportParams.PreferredAddress.Ipv4.sin_addr,
                QuicByteSwapUint16(Connection->PeerTransportParams.PreferredAddress.Ipv4.sin_port));
        } else {
            LogInfo("[conn][%p] Peer configured preferred address [%!IPV6ADDR!]:%d",
                Connection,
                &Connection->PeerTransportParams.PreferredAddress.Ipv6.sin6_addr,
                QuicByteSwapUint16(Connection->PeerTransportParams.PreferredAddress.Ipv6.sin6_port));
        }*/

        //
        // TODO - Implement preferred address feature.
        //
    }

    if (Connection->State.ReceivedRetryPacket) {
        QUIC_DBG_ASSERT(!QuicConnIsServer(Connection));
        QUIC_DBG_ASSERT(Connection->OrigCID != NULL);
        QUIC_DBG_ASSERT(!FromCache);
        //
        // If we received a Retry packet during the handshake, we (the client)
        // must validate that the server knew the original connection ID we sent,
        // so that we can be sure that no middle box injected the Retry packet.
        //
        BOOLEAN ValidOrigCID = FALSE;
        if (!(Connection->PeerTransportParams.Flags & QUIC_TP_FLAG_ORIGINAL_CONNECTION_ID)) {
            EventWriteQuicConnError(Connection, "Peer didn't provide the OrigConnID in TP");
        } else if (Connection->PeerTransportParams.OriginalConnectionIDLength != Connection->OrigCID->Length) {
            EventWriteQuicConnError(Connection, "Peer provided incorrect length of OrigConnID in TP");
        } else if (
            memcmp(
                Connection->PeerTransportParams.OriginalConnectionID,
                Connection->OrigCID->Data,
                Connection->OrigCID->Length) != 0) {
            EventWriteQuicConnError(Connection, "Peer provided incorrect OrigConnID in TP");
        } else {
            ValidOrigCID = TRUE;
            QUIC_FREE(Connection->OrigCID);
            Connection->OrigCID = NULL;
        }

        if (!ValidOrigCID) {
            goto Error;
        }

    } else if (!QuicConnIsServer(Connection) && !FromCache) {
        //
        // Per spec, the client must validate no original CID TP was sent if no
        // Retry occurred. No need to validate cached values, as they don't
        // apply to the current connection attempt.
        //
        if (!!(Connection->PeerTransportParams.Flags & QUIC_TP_FLAG_ORIGINAL_CONNECTION_ID)) {
            EventWriteQuicConnError(Connection, "Peer provided the OrigConnID in TP when no Retry occurred");
            goto Error;
        }
    }

    Connection->Send.PeerMaxData =
        Connection->PeerTransportParams.InitialMaxData;

    QuicStreamSetInitializeTransportParameters(
        &Connection->Streams,
        Connection->PeerTransportParams.InitialMaxBidiStreams,
        Connection->PeerTransportParams.InitialMaxUniStreams,
        !FromCache);

    return;

Error:

    QuicConnTransportError(Connection, QUIC_ERROR_TRANSPORT_PARAMETER_ERROR);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicConnQueueRecvDatagram(
    _In_ PQUIC_CONNECTION Connection,
    _In_ QUIC_RECV_DATAGRAM* DatagramChain,
    _In_ uint32_t DatagramChainLength
    )
{
    QUIC_RECV_DATAGRAM** DatagramChainTail = &DatagramChain->Next;
    DatagramChain->QueuedOnConnection = TRUE;
    QuicDataPathRecvDatagramToRecvPacket(DatagramChain)->AssignedToConnection = TRUE;
    while (*DatagramChainTail != NULL) {
        (*DatagramChainTail)->QueuedOnConnection = TRUE;
        QuicDataPathRecvDatagramToRecvPacket(*DatagramChainTail)->AssignedToConnection = TRUE;
        DatagramChainTail = &((*DatagramChainTail)->Next);
    }

    LogVerbose("[conn][%p] Queuing %u UDP datagrams", Connection, DatagramChainLength);

    BOOLEAN QueueOperation;
    QuicDispatchLockAcquire(&Connection->ReceiveQueueLock);
    if (Connection->ReceiveQueueCount >= QUIC_MAX_RECEIVE_QUEUE_COUNT) {
        QueueOperation = FALSE;
    } else {
        *Connection->ReceiveQueueTail = DatagramChain;
        Connection->ReceiveQueueTail = DatagramChainTail;
        DatagramChain = NULL;
        QueueOperation = (Connection->ReceiveQueueCount == 0);
        Connection->ReceiveQueueCount += DatagramChainLength;
    }
    QuicDispatchLockRelease(&Connection->ReceiveQueueLock);

    if (DatagramChain != NULL) {
        QUIC_RECV_DATAGRAM* Datagram = DatagramChain;
        do {
            Datagram->QueuedOnConnection = FALSE;
            QuicPacketLogDrop(
                Connection,
                QuicDataPathRecvDatagramToRecvPacket(Datagram),
                "Max queue limit reached");
        } while ((Datagram = Datagram->Next) != NULL);
        QuicDataPathBindingReturnRecvDatagrams(DatagramChain);
        return;
    }

    if (QueueOperation) {
        PQUIC_OPERATION ConnOper =
            QuicOperationAlloc(Connection->Worker, QUIC_OPER_TYPE_FLUSH_RECV);
        if (ConnOper != NULL) {
            QuicConnQueueOper(Connection, ConnOper);
        } else {
            EventWriteQuicAllocFailure("Flush Recv operation", 0);
        }
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicConnQueueUnreachable(
    _In_ PQUIC_CONNECTION Connection,
    _In_ const QUIC_ADDR* RemoteAddress
    )
{
    if (Connection->Crypto.TlsState.ReadKey > QUIC_PACKET_KEY_INITIAL) {
        //
        // Only queue unreachable events at the beginning of the handshake.
        // Otherwise, it opens up an attack surface.
        //
        LogWarning("[conn][%p] Ignoring received unreachable event (inline).", Connection);
        return;
    }

    PQUIC_OPERATION ConnOper =
        QuicOperationAlloc(Connection->Worker, QUIC_OPER_TYPE_UNREACHABLE);
    if (ConnOper != NULL) {
        ConnOper->UNREACHABLE.RemoteAddress = *RemoteAddress;
        QuicConnQueueOper(Connection, ConnOper);
    } else {
        EventWriteQuicAllocFailure("Unreachable operation", 0);
    }
}

//
// Updates the current destination CID to the received packet's source CID, if
// not already equal. Only used during the handshake, on the client side.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
BOOLEAN
QuicConnUpdateDestCID(
    _In_ PQUIC_CONNECTION Connection,
    _In_ const QUIC_RECV_PACKET* const Packet
    )
{
    QUIC_DBG_ASSERT(!QuicConnIsServer(Connection));
    QUIC_DBG_ASSERT(!Connection->State.Connected);

    QUIC_DBG_ASSERT(!QuicListIsEmpty(&Connection->DestCIDs));
    QUIC_CID_QUIC_LIST_ENTRY* DestCID =
        QUIC_CONTAINING_RECORD(
            Connection->DestCIDs.Flink,
            QUIC_CID_QUIC_LIST_ENTRY,
            Link);

    if (Packet->SourceCIDLen != DestCID->CID.Length ||
        memcmp(Packet->SourceCID, DestCID->CID.Data, DestCID->CID.Length) != 0) {

        // TODO - Only update for the first packet of each type (Initial and Retry).

        EventWriteQuicConnDestCidRemoved(Connection, DestCID->CID.Length, DestCID->CID.Data);

        //
        // We have just received the a packet from a new source CID
        // from the server. Remove the current DestCID we have for the
        // server (which we randomly generated) and replace it with
        // the one we have just received.
        //
        if (Packet->SourceCIDLen <= DestCID->CID.Length) {
            //
            // Since the current structure has enough room for the
            // new CID, we will just reuse it.
            //
            DestCID->CID.IsInitial = FALSE;
            DestCID->CID.Length = Packet->SourceCIDLen;
            QuicCopyMemory(DestCID->CID.Data, Packet->SourceCID, DestCID->CID.Length);
        } else {
            //
            // There isn't enough room in the existing structure,
            // so we must allocate a new one and free the old one.
            //
            QuicListEntryRemove(&DestCID->Link);
            QUIC_FREE(DestCID);
            DestCID =
                QuicCidNewDestination(
                    Packet->SourceCIDLen,
                    Packet->SourceCID);
            if (DestCID == NULL) {
                Connection->DestCIDCount--;
                QuicConnFatalError(Connection, QUIC_STATUS_OUT_OF_MEMORY, "Out of memory");
                return FALSE;
            } else {
                QuicListInsertHead(&Connection->DestCIDs, &DestCID->Link);
            }
        }

        if (DestCID != NULL) {
            EventWriteQuicConnDestCidAdded(Connection, DestCID->CID.Length, DestCID->CID.Data);
        }
    }

    return TRUE;
}

/*
//
// Version negotiation is removed for the first version of QUIC.
// When it is put back, it will probably be implemented as in this
// function.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnRecvVerNeg(
    _In_ PQUIC_CONNECTION Connection,
    _In_ const QUIC_RECV_PACKET* const Packet
    )
{
    uint32_t SupportedVersion = 0;

    // TODO - Validate the packet's SourceCID is equal to our DestCID.

    const uint32_t* ServerVersionList =
        (const uint32_t*)(
        Packet->VerNeg->DestCID +
        QuicCidDecodeLength(Packet->VerNeg->SourceCIDLength) +
        QuicCidDecodeLength(Packet->VerNeg->DestCIDLength));
    uint16_t ServerVersionListLength =
        (Packet->BufferLength - (uint16_t)((uint8_t*)ServerVersionList - Packet->Buffer)) / sizeof(uint32_t);

    //
    // Go through the list and make sure it doesn't include our originally
    // requested version. If it does, we are supposed to ignore it. Cache the
    // first supported version.
    //
    LogVerbose("[conn][%p] Received Version Negotation:", Connection);
    for (uint16_t i = 0; i < ServerVersionListLength; i++) {

        LogVerbose("[conn][%p]   Ver[%d]: 0x%x", Connection, i,
            QuicByteSwapUint32(ServerVersionList[i]));

        //
        // Check to see if this is the current version.
        //
        if (ServerVersionList[i] == Connection->Stats.QuicVersion) {
            LogVerbose("[conn][%p] Dropping version negotation that includes the current version.", Connection);
            goto Exit;
        }

        //
        // Check to see if this is supported, if we haven't already found a
        // supported version.
        //
        if (SupportedVersion == 0 &&
            QuicIsVersionSupported(ServerVersionList[i])) {
            SupportedVersion = ServerVersionList[i];
        }
    }

    //
    // Did we find a supported version?
    //
    if (SupportedVersion != 0) {

        Connection->Stats.QuicVersion = SupportedVersion;
        QuicConnOnQuicVersionSet(Connection);

        //
        // Match found! Start connecting with selected version.
        //
        QuicConnRestart(Connection, TRUE);

    } else {

        //
        // No match! Connection failure.
        //
        QuicConnCloseLocally(
            Connection,
            QUIC_CLOSE_INTERNAL_SILENT,
            QUIC_ERROR_VERSION_NEGOTIATION_ERROR,
            NULL);
    }

Exit:

    return;
}
*/

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnRecvRetry(
    _In_ PQUIC_CONNECTION Connection,
    _In_ QUIC_RECV_PACKET* Packet
    )
{
    //
    // Only clients should receive Retry packets.
    //
    if (QuicConnIsServer(Connection)) {
        QuicPacketLogDrop(Connection, Packet, "Retry sent to server");
        return;
    }

    //
    // Make sure we are in the correct state of the handshake.
    //
    if (Connection->State.GotFirstServerResponse) {
        QuicPacketLogDrop(Connection, Packet, "Already received server response");
        return;
    }

    //
    // Decode and validate the Retry packet.
    //

    uint16_t Offset = Packet->HeaderLength;
    uint8_t OrigDestCIDLength = *(Packet->Buffer + Offset);
    Offset += sizeof(uint8_t);

    if (Packet->BufferLength < Offset + OrigDestCIDLength) {
        QuicPacketLogDrop(Connection, Packet, "No room for ODCID");
        return;
    }

    QuicPacketLogHeader(
        Connection,
        TRUE,
        0,
        0,
        Packet->BufferLength,
        Packet->Buffer,
        0);

    const uint8_t* OrigDestCID = Packet->Buffer + Offset;
    Offset += OrigDestCIDLength;

    QUIC_DBG_ASSERT(!QuicListIsEmpty(&Connection->DestCIDs));
    QUIC_CID_QUIC_LIST_ENTRY* DestCID =
        QUIC_CONTAINING_RECORD(
            Connection->DestCIDs.Flink,
            QUIC_CID_QUIC_LIST_ENTRY,
            Link);

    if (OrigDestCIDLength != DestCID->CID.Length ||
        memcmp(DestCID->CID.Data, OrigDestCID, OrigDestCIDLength) != 0) {
        QuicPacketLogDrop(Connection, Packet, "Invalid ODCID");
        return;
    }

    //
    // Cache the Retry token.
    //

    const uint8_t* Token = Packet->Buffer + Offset;
    uint16_t TokenLength = Packet->BufferLength - Offset;

    Connection->Send.InitialToken = QUIC_ALLOC_PAGED(TokenLength);
    if (Connection->Send.InitialToken == NULL) {
        EventWriteQuicAllocFailure("InitialToken", TokenLength);
        QuicPacketLogDrop(Connection, Packet, "InitialToken alloc failed");
        return;
    }

    Connection->Send.InitialTokenLength = TokenLength;
    memcpy((uint8_t*)Connection->Send.InitialToken, Token, TokenLength);

    //
    // Save the original CID for later validation in the TP.
    //
    Connection->OrigCID =
        QUIC_ALLOC_NONPAGED(
            sizeof(QUIC_CID) +
            OrigDestCIDLength);
    if (Connection->OrigCID == NULL) {
        EventWriteQuicAllocFailure("OrigCID", TokenLength);
        QuicPacketLogDrop(Connection, Packet, "OrigCID alloc failed");
        return;
    }

    Connection->OrigCID->Length = OrigDestCIDLength;
    QuicCopyMemory(
        Connection->OrigCID->Data,
        OrigDestCID,
        OrigDestCIDLength);

    //
    // Update the (destination) server's CID.
    //
    if (!QuicConnUpdateDestCID(Connection, Packet)) {
        return;
    }

    Connection->State.GotFirstServerResponse = TRUE;
    Connection->State.ReceivedRetryPacket = TRUE;

    //
    // Update the Initial packet's key based on the new CID.
    //
    QuicPacketKeyFree(Connection->Crypto.TlsState.ReadKeys[QUIC_PACKET_KEY_INITIAL]);
    QuicPacketKeyFree(Connection->Crypto.TlsState.WriteKeys[QUIC_PACKET_KEY_INITIAL]);
    Connection->Crypto.TlsState.ReadKeys[QUIC_PACKET_KEY_INITIAL] = NULL;
    Connection->Crypto.TlsState.WriteKeys[QUIC_PACKET_KEY_INITIAL] = NULL;

    QUIC_DBG_ASSERT(!QuicListIsEmpty(&Connection->DestCIDs));
    DestCID =
        QUIC_CONTAINING_RECORD(
            Connection->DestCIDs.Flink,
            QUIC_CID_QUIC_LIST_ENTRY,
            Link);

    QUIC_STATUS Status;
    if (QUIC_FAILED(
        Status =
        QuicPacketKeyCreateInitial(
            QuicConnIsServer(Connection),
            QuicInitialSaltVersion1,
            DestCID->CID.Length,
            DestCID->CID.Data,
            &Connection->Crypto.TlsState.ReadKeys[QUIC_PACKET_KEY_INITIAL],
            &Connection->Crypto.TlsState.WriteKeys[QUIC_PACKET_KEY_INITIAL]))) {
        QuicConnFatalError(Connection, Status, "Failed to create initial keys");
        return;
    }

    Connection->Stats.StatelessRetry = TRUE;

    //
    // Restart the connection, using the new CID and Retry Token.
    //
    QuicConnRestart(Connection, FALSE);

    Packet->CompletelyValid = TRUE;
}

//
// Tries to get the requested decryption key or defers the packet for later
// processing.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
BOOLEAN
QuicConnGetKeyOrDeferDatagram(
    _In_ PQUIC_CONNECTION Connection,
    _In_ QUIC_RECV_PACKET* Packet
    )
{
    if (Packet->KeyType > Connection->Crypto.TlsState.ReadKey) {
        //
        // We don't have the necessary key yet so defer the packet until we get
        // the key.
        //
        QUIC_ENCRYPT_LEVEL EncryptLevel = QuicKeyTypeToEncryptLevel(Packet->KeyType);
        PQUIC_PACKET_SPACE Packets = Connection->Packets[EncryptLevel];
        if (Packets->DeferredDatagramsCount == QUIC_MAX_PENDING_DATAGRAMS) {
            //
            // We already have too many packets queued up. Just drop this one.
            //
            QuicPacketLogDrop(Connection, Packet, "Max deferred datagram count reached");

        } else {
            LogVerbose("[conn][%p] Deferring datagram (type=%hu).",
                Connection, Packet->KeyType);

            Packets->DeferredDatagramsCount++;
            Packet->DecryptionDeferred = TRUE;

            //
            // Add it to the list of pending packets that are waiting on a key
            // to decrypt with.
            //
            QUIC_RECV_DATAGRAM** Tail = &Packets->DeferredDatagrams;
            while (*Tail != NULL) {
                Tail = &((*Tail)->Next);
            }
            *Tail = QuicDataPathRecvPacketToRecvDatagram(Packet);
            (*Tail)->Next = NULL;
        }

        return FALSE;
    }

    _Analysis_assume_(Packet->KeyType >= 0 && Packet->KeyType < QUIC_PACKET_KEY_COUNT);
    if (Connection->Crypto.TlsState.ReadKeys[Packet->KeyType] == NULL) {
        //
        // This key is no longer being accepted. Throw the packet away.
        //
        QuicPacketLogDrop(Connection, Packet, "Key no longer accepted");
        return FALSE;
    }

    return TRUE;
}

//
// Validates the receives packet's header. Returns TRUE if the packet should be
// processed further.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
_Success_(return != FALSE)
BOOLEAN
QuicConnRecvHeader(
    _In_ PQUIC_CONNECTION Connection,
    _In_ QUIC_RECV_PACKET* Packet,
    _Out_writes_all_(16) uint8_t* Cipher
    )
{
    //
    // Check invariants and packet version.
    //

    if (!Packet->ValidatedHeaderInv &&
        !QuicPacketValidateInvariant(Connection, Packet, Connection->State.ShareBinding)) {
        return FALSE;
    }

    if (!Packet->IsShortHeader) {
        if (Packet->Invariant->LONG_HDR.Version != Connection->Stats.QuicVersion) {
            if (Packet->Invariant->LONG_HDR.Version == QUIC_VERSION_VER_NEG) {
                Connection->Stats.VersionNegotiation = TRUE;

                //
                // Version negotiation is removed for the first version of QUIC.
                // When it is put back, it will probably be implemented as in this
                // function:
                // QuicConnRecvVerNeg(Connection, Packet);
                //
                // For now, since there is a single version, receiving
                // a version negotation packet means there is a version
                // mismatch, so abandon the connect attempt.
                //

                QuicConnCloseLocally(
                    Connection,
                    QUIC_CLOSE_INTERNAL_SILENT | QUIC_CLOSE_QUIC_STATUS,
                    (uint64_t)QUIC_STATUS_VER_NEG_ERROR,
                    NULL);
            } else {
                QuicPacketLogDropWithValue(
                    Connection,
                    Packet,
                    "Invalid version",
                    QuicByteSwapUint32(Packet->Invariant->LONG_HDR.Version));
            }
            return FALSE;
        }
    } else {
        if (!QuicIsVersionSupported(Connection->Stats.QuicVersion)) {
            QuicPacketLogDrop(Connection, Packet, "SH packet during version negotiation");
            return FALSE;
        }
    }

    QUIC_FRE_ASSERT(QuicIsVersionSupported(Connection->Stats.QuicVersion));

    //
    // Begin non-version-independent logic. When future versions are supported,
    // there may be some switches based on packet version.
    //

    if (!Packet->IsShortHeader) {
        if (Packet->LH->Type == QUIC_RETRY) {
            QuicConnRecvRetry(Connection, Packet);
            return FALSE;
        }

        const uint8_t* TokenBuffer = NULL;
        uint16_t TokenLength = 0;

        if (!Packet->ValidatedHeaderVer &&
            !QuicPacketValidateLongHeaderD23(
                Connection,
                QuicConnIsServer(Connection),
                Packet,
                &TokenBuffer,
                &TokenLength)) {
            return FALSE;
        }

        if (!Connection->State.SourceAddressValidated && Packet->ValidToken) {

            QUIC_DBG_ASSERT(TokenBuffer == NULL);
            QuicPacketDecodeRetryTokenD23(Packet, &TokenBuffer, &TokenLength);
            QUIC_DBG_ASSERT(TokenLength == sizeof(QUIC_RETRY_TOKEN_CONTENTS));

            QUIC_RETRY_TOKEN_CONTENTS Token;
            if (!QuicRetryTokenDecrypt(Packet, TokenBuffer, &Token)) {
                QUIC_DBG_ASSERT(FALSE);
                return FALSE;
            }

            QUIC_DBG_ASSERT(Token.OrigConnIdLength <= sizeof(Token.OrigConnId));

            Connection->OrigCID =
                QUIC_ALLOC_NONPAGED(
                    sizeof(QUIC_CID) +
                    Token.OrigConnIdLength);
            if (Connection->OrigCID == NULL) {
                EventWriteQuicAllocFailure("OrigCID", sizeof(QUIC_CID) + Token.OrigConnIdLength);
                return FALSE;
            }

            Connection->OrigCID->Length = Token.OrigConnIdLength;
            QuicCopyMemory(
                Connection->OrigCID->Data,
                Token.OrigConnId,
                Token.OrigConnIdLength);

            Connection->State.SourceAddressValidated = TRUE;
            Connection->Send.Allowance = UINT32_MAX;
            LogInfo("[conn][%p] Source address validated via Initial token.", Connection);
        }

        Packet->KeyType = QuicPacketTypeToKeyType(Packet->LH->Type);

    } else {

        if (!Packet->ValidatedHeaderVer &&
            !QuicPacketValidateShortHeaderD23(Connection, Packet)) {
            return FALSE;
        }

        Packet->KeyType = QUIC_PACKET_KEY_1_RTT;
    }

    if (Connection->State.EncryptionEnabled &&
        Connection->State.HeaderProtectionEnabled &&
        Packet->PayloadLength < 4 + QUIC_HP_SAMPLE_LENGTH) {
        QuicPacketLogDrop(Connection, Packet, "Too short for HP");
        return FALSE;
    }

    //
    // If the key is not present then we will attempt to queue the packet
    // and defer processing for later.
    //
    // For compound packets, we defer processing the rest of the UDP packet
    // once we reach a QUIC packet we can't decrypt.
    //
    if (!QuicConnGetKeyOrDeferDatagram(Connection, Packet)) {
        return FALSE;
    }

    //
    // To decrypt the header, the payload after the header is used as the IV. We
    // don't actually know the length of the packet number so we assume maximum
    // (per spec) and start sampling 4 bytes after the start of the packet number.
    //
    QuicCopyMemory(
        Cipher,
        Packet->Buffer + Packet->HeaderLength + 4,
        QUIC_HP_SAMPLE_LENGTH);

    return TRUE;
}

//
// Decodes and decompresses the packet number. If necessary, updates the key
// phase accordingly, to allow for decryption as the next step. Returns TRUE if
// the packet should continue to be processed further.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
BOOLEAN
QuicConnRecvPrepareDecrypt(
    _In_ PQUIC_CONNECTION Connection,
    _In_ QUIC_RECV_PACKET* Packet,
    _In_reads_(16) const uint8_t* HpMask
    )
{
    QUIC_DBG_ASSERT(Packet->ValidatedHeaderInv);
    QUIC_DBG_ASSERT(Packet->ValidatedHeaderVer);
    QUIC_DBG_ASSERT(Packet->HeaderLength <= Packet->BufferLength);
    QUIC_DBG_ASSERT(Packet->PayloadLength <= Packet->BufferLength);
    QUIC_DBG_ASSERT(Packet->HeaderLength + Packet->PayloadLength <= Packet->BufferLength);

    //
    // Packet->HeaderLength currently points to the start of the encrypted
    // packet number and Packet->PayloadLength includes the length of the rest
    // of the packet from that point on.
    //

    //
    // Decrypt the first byte of the header to get the packet number length.
    //
    uint8_t CompressedPacketNumberLength = 0;
    if (Packet->IsShortHeader) {
        ((uint8_t*)Packet->Buffer)[0] ^= HpMask[0] & 0x1f; // Only the first 5 bits
        CompressedPacketNumberLength = Packet->SH->PnLength + 1;
    } else {
        ((uint8_t*)Packet->Buffer)[0] ^= HpMask[0] & 0x0f; // Only the first 4 bits
        CompressedPacketNumberLength = Packet->LH->PnLength + 1;
    }

    QUIC_DBG_ASSERT(CompressedPacketNumberLength >= 1 && CompressedPacketNumberLength <= 4);
    QUIC_DBG_ASSERT(Packet->HeaderLength + CompressedPacketNumberLength <= Packet->BufferLength);

    //
    // Decrypt the packet number now that we have the length.
    //
    for (uint8_t i = 0; i < CompressedPacketNumberLength; i++) {
        ((uint8_t*)Packet->Buffer)[Packet->HeaderLength + i] ^= HpMask[1 + i];
    }

    //
    // Decode the packet number into the compressed packet number. The
    // compressed packet number only represents the least significant N bytes of
    // the true packet number.
    //

    uint64_t CompressedPacketNumber = 0;
    QuicPktNumDecode(
        CompressedPacketNumberLength,
        Packet->Buffer + Packet->HeaderLength,
        &CompressedPacketNumber);

    Packet->HeaderLength += CompressedPacketNumberLength;
    Packet->PayloadLength -= CompressedPacketNumberLength;

    //
    // Decompress the packet number into the full packet number.
    //

    QUIC_ENCRYPT_LEVEL EncryptLevel = QuicKeyTypeToEncryptLevel(Packet->KeyType);
    Packet->PacketNumber =
        QuicPacketNumberDecompress(
            Connection->Packets[EncryptLevel]->NextRecvPacketNumber,
            CompressedPacketNumber,
            CompressedPacketNumberLength);
    Packet->PacketNumberSet = TRUE;

    if (Packet->PacketNumber > QUIC_VAR_INT_MAX) {
        QuicPacketLogDrop(Connection, Packet, "Packet number too big");
        return FALSE;
    }

    QUIC_DBG_ASSERT(Packet->IsShortHeader || Packet->LH->Type != QUIC_RETRY);

    //
    // Ensure minimum encrypted payload length.
    //
    if (Connection->State.EncryptionEnabled &&
        Packet->PayloadLength < QUIC_ENCRYPTION_OVERHEAD) {
        QuicPacketLogDrop(Connection, Packet, "Payload length less than encryption tag");
        return FALSE;
    }

    PQUIC_PACKET_SPACE PacketSpace = Connection->Packets[QUIC_ENCRYPT_LEVEL_1_RTT];
    if (Packet->IsShortHeader && EncryptLevel == QUIC_ENCRYPT_LEVEL_1_RTT &&
        Packet->SH->KeyPhase != PacketSpace->CurrentKeyPhase) {
        if (PacketSpace->AwaitingKeyPhaseConfirmation ||
            Packet->PacketNumber < PacketSpace->ReadKeyPhaseStartPacketNumber) {
            //
            // The packet doesn't match our current key phase and we're awaiting
            // confirmation of our current key phase or the packet number is less
            // than the start of the current key phase, so this is likely using
            // the old key phase.
            //
            LogVerbose("[conn][%p] Using old key to decrypt.", Connection);
            QUIC_DBG_ASSERT(Connection->Crypto.TlsState.ReadKeys[QUIC_PACKET_KEY_1_RTT_OLD] != NULL);
            QUIC_DBG_ASSERT(Connection->Crypto.TlsState.WriteKeys[QUIC_PACKET_KEY_1_RTT_OLD] != NULL);
            Packet->KeyType = QUIC_PACKET_KEY_1_RTT_OLD;
        } else {
            //
            // The packet doesn't match our key phase, and we're not awaiting
            // confirmation of a key phase change, or this is a newer packet
            // number, so most likely using a new key phase. Update the keys
            // and try it out.
            //

            LogVerbose("[conn][%p] Possible peer initiated key update [packet %llu]",
                Connection, Packet->PacketNumber);

            QUIC_STATUS Status = QuicCryptoGenerateNewKeys(Connection);
            if (QUIC_FAILED(Status)) {
                QuicPacketLogDrop(Connection, Packet, "Generate new packet keys");
                return FALSE;
            }
            Packet->KeyType = QUIC_PACKET_KEY_1_RTT_NEW;
        }
    }

    return TRUE;
}

//
// Decrypts the packet's payload and authenticates the whole packet. On
// successful authentication of the packet, does some final processing of the
// packet header (key and CID updates). Returns TRUE if the packet should
// continue to be processed further.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
BOOLEAN
QuicConnRecvDecryptAndAuthenticate(
    _In_ PQUIC_CONNECTION Connection,
    _In_ QUIC_RECV_PACKET* Packet
    )
{
    QUIC_DBG_ASSERT(Packet->BufferLength >= Packet->HeaderLength + Packet->PayloadLength);

    const uint8_t* Payload = Packet->Buffer + Packet->HeaderLength;

    //
    // We need to copy the end of the packet before trying decryption, as a
    // failed decryption trashes the stateless reset token.
    //
    BOOLEAN CanCheckForStatelessReset = FALSE;
    uint8_t PacketResetToken[QUIC_STATELESS_RESET_TOKEN_LENGTH];
    if (!QuicConnIsServer(Connection) &&
        Packet->IsShortHeader &&
        Packet->HeaderLength + Packet->PayloadLength >= QUIC_MIN_STATELESS_RESET_PACKET_LENGTH) {
        CanCheckForStatelessReset = TRUE;
        QuicCopyMemory(
            PacketResetToken,
            Payload + Packet->PayloadLength - QUIC_STATELESS_RESET_TOKEN_LENGTH,
            QUIC_STATELESS_RESET_TOKEN_LENGTH);
    }

    uint8_t Iv[QUIC_IV_LENGTH];
    QuicCryptoCombineIvAndPacketNumber(
        Connection->Crypto.TlsState.ReadKeys[Packet->KeyType]->Iv,
        (uint8_t*) &Packet->PacketNumber,
        Iv);

    //
    // Decrypt the payload with the appropriate key.
    //
    if (Connection->State.EncryptionEnabled &&
        QUIC_FAILED(
        QuicDecrypt(
            Connection->Crypto.TlsState.ReadKeys[Packet->KeyType]->PacketKey,
            Iv,
            Packet->HeaderLength,   // HeaderLength
            Packet->Buffer,         // Header
            Packet->PayloadLength,  // BufferLength
            (uint8_t*)Payload))) {  // Buffer

        //
        // Check for a stateless reset packet.
        //
        if (CanCheckForStatelessReset) {
            for (QUIC_LIST_ENTRY* Entry = Connection->DestCIDs.Flink;
                    Entry != &Connection->DestCIDs;
                    Entry = Entry->Flink) {
                //
                // Loop through all our stored stateless reset tokens to see if
                // we have a match.
                //
                QUIC_CID_QUIC_LIST_ENTRY* DestCID =
                    QUIC_CONTAINING_RECORD(
                        Entry,
                        QUIC_CID_QUIC_LIST_ENTRY,
                        Link);
                if (DestCID->CID.HasResetToken &&
                    memcmp(
                        DestCID->ResetToken,
                        PacketResetToken,
                        QUIC_STATELESS_RESET_TOKEN_LENGTH) == 0) {
                    LogPacketInfo("[S][RX][-] SR %s",
                        QuicCidBufToStr(PacketResetToken, QUIC_STATELESS_RESET_TOKEN_LENGTH).Buffer);
                    LogInfo("[conn][%p] Received stateless reset", Connection);
                    QuicConnCloseLocally(
                        Connection,
                        QUIC_CLOSE_INTERNAL_SILENT | QUIC_CLOSE_QUIC_STATUS,
                        (uint64_t)QUIC_STATUS_ABORTED,
                        NULL);
                    return FALSE;
                }
            }
        }

        if (WPP_COMPID_LEVEL_ENABLED(FLAG_PACKET, TRACE_LEVEL_INFORMATION)) {
            QuicPacketLogHeader(
                Connection,
                TRUE,
                Connection->State.ShareBinding ? MSQUIC_CONNECTION_ID_LENGTH : 0,
                Packet->PacketNumber,
                Packet->HeaderLength,
                Packet->Buffer,
                Connection->Stats.QuicVersion);
        }
        Connection->Stats.Recv.DecryptionFailures++;
        QuicPacketLogDrop(Connection, Packet, "Decryption failure");

        return FALSE;
    }

    //
    // Validate the header's reserved bits now that the packet has been
    // decrypted.
    //
    if (Packet->IsShortHeader) {
        if (Packet->SH->Reserved != 0) {
            QuicPacketLogDrop(Connection, Packet, "Invalid SH Reserved bits values");
            QuicConnTransportError(Connection, QUIC_ERROR_PROTOCOL_VIOLATION);
            return FALSE;
        }
    } else {
        if (Packet->LH->Reserved != 0) {
            QuicPacketLogDrop(Connection, Packet, "Invalid LH Reserved bits values");
            QuicConnTransportError(Connection, QUIC_ERROR_PROTOCOL_VIOLATION);
            return FALSE;
        }
    }

    //
    // Account for updated payload length after decryption.
    //
    if (Connection->State.EncryptionEnabled) {
        Packet->PayloadLength -= QUIC_ENCRYPTION_OVERHEAD;
    }

    //
    // At this point the packet has been completely decrypted and authenticated.
    // Now all header processing that can only be done on an authenticated
    // packet may continue.
    //

    //
    // Drop any duplicate packet numbers now that we know the packet number is
    // valid.
    //
    QUIC_ENCRYPT_LEVEL EncryptLevel = QuicKeyTypeToEncryptLevel(Packet->KeyType);
    if (QuicAckTrackerAddPacketNumber(
            &Connection->Packets[EncryptLevel]->AckTracker,
            Packet->PacketNumber)) {

        if (WPP_COMPID_LEVEL_ENABLED(FLAG_PACKET, TRACE_LEVEL_INFORMATION)) {
            QuicPacketLogHeader(
                Connection,
                TRUE,
                Connection->State.ShareBinding ? MSQUIC_CONNECTION_ID_LENGTH : 0,
                Packet->PacketNumber,
                Packet->BufferLength,
                Packet->Buffer,
                Connection->Stats.QuicVersion);
        }
        QuicPacketLogDrop(Connection, Packet, "Duplicate packet number");
        Connection->Stats.Recv.DuplicatePackets++;
        return FALSE;
    }

    //
    // Log the received packet header and payload now that it's decrypted.
    //

    if (WPP_COMPID_LEVEL_ENABLED(FLAG_PACKET, TRACE_LEVEL_INFORMATION)) {
        QuicPacketLogHeader(
            Connection,
            TRUE,
            Connection->State.ShareBinding ? MSQUIC_CONNECTION_ID_LENGTH : 0,
            Packet->PacketNumber,
            Packet->HeaderLength + Packet->PayloadLength,
            Packet->Buffer,
            Connection->Stats.QuicVersion);
        QuicFrameLogAll(
            Connection,
            TRUE,
            Packet->PacketNumber,
            Packet->HeaderLength + Packet->PayloadLength,
            Packet->Buffer,
            Packet->HeaderLength);
        QuicLogBuffer(Packet->Buffer, Packet->HeaderLength + Packet->PayloadLength);
    }

    EventWriteQuicConnPacketRecv(
        Connection,
        Packet->PacketNumber,
        Packet->IsShortHeader ? QUIC_TRACE_PACKET_ONE_RTT : (Packet->LH->Type + 1),
        Packet->HeaderLength + Packet->PayloadLength);

    //
    // Process any connection ID updates as necessary.
    //

    if (!Packet->IsShortHeader) {
        switch (Packet->LH->Type) {
        case QUIC_INITIAL:
            if (!Connection->State.Connected &&
                !QuicConnIsServer(Connection) &&
                !QuicConnUpdateDestCID(Connection, Packet)) {
                //
                // Client side needs to respond to the server's new source
                // connection ID that is received in the first Initial packet.
                //
                return FALSE;
            }
            break;

        case QUIC_0_RTT_PROTECTED:
            QUIC_DBG_ASSERT(QuicConnIsServer(Connection));
            Packet->EncryptedWith0Rtt = TRUE;
            break;

        default:
            break;
        }
    }

    //
    // Update key state if the keys have been updated.
    //

    if (Packet->IsShortHeader) {
        PQUIC_PACKET_SPACE PacketSpace = Connection->Packets[QUIC_ENCRYPT_LEVEL_1_RTT];
        if (Packet->KeyType == QUIC_PACKET_KEY_1_RTT_NEW) {

            QuicCryptoUpdateKeyPhase(Connection, FALSE);
            PacketSpace->ReadKeyPhaseStartPacketNumber = Packet->PacketNumber;

            LogVerbose("[conn][%p] Updating current read key phase and packet number[%llu]",
                Connection, Packet->PacketNumber);

        } else if (Packet->KeyType == QUIC_PACKET_KEY_1_RTT &&
            Packet->PacketNumber < PacketSpace->ReadKeyPhaseStartPacketNumber) {
            //
            // If this packet is the current key phase, but has an earlier packet
            // number than this key phase's start, update the key phase start.
            //
            PacketSpace->ReadKeyPhaseStartPacketNumber = Packet->PacketNumber;
            LogVerbose("[conn][%p] Updating current key phase read packet number[%llu]",
                Connection, Packet->PacketNumber);
        }
    }

    if (Packet->KeyType == QUIC_PACKET_KEY_HANDSHAKE &&
        QuicConnIsServer(Connection)) {
        //
        // Per spec, server MUST discard Initial keys when it starts
        // decrypting packets using handshake keys.
        //
        QuicCryptoDiscardKeys(&Connection->Crypto, QUIC_PACKET_KEY_INITIAL);

        if (!Connection->State.SourceAddressValidated) {
            LogInfo("[conn][%p] Source address validated via Handshake packet.", Connection);
            Connection->State.SourceAddressValidated = TRUE;
            QuicSendSetAllowance(&Connection->Send, UINT32_MAX);
        }
    }

    return TRUE;
}

//
// Reads the payload (QUIC frames) of the packet, and if everything is
// successful marks the packet for acknowledgement. Returns TRUE if the packet
// was successfully processed.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
BOOLEAN
QuicConnRecvPayload(
    _In_ PQUIC_CONNECTION Connection,
    _In_ QUIC_RECV_PACKET* Packet
    )
{
    BOOLEAN AckPacketImmediately = FALSE; // Allows skipping delayed ACK timer.
    BOOLEAN UpdatedFlowControl = FALSE;
    QUIC_ENCRYPT_LEVEL EncryptLevel = QuicKeyTypeToEncryptLevel(Packet->KeyType);
    BOOLEAN Closed = Connection->State.ClosedLocally || Connection->State.ClosedRemotely;
    const uint8_t* Payload = Packet->Buffer + Packet->HeaderLength;
    uint16_t PayloadLength = Packet->PayloadLength;

    //
    // Process the payload.
    //
    uint16_t Offset = 0;
    while (Offset < PayloadLength) {

        //
        // Read the frame type.
        //
        QUIC_FRAME_TYPE FrameType = Payload[Offset];
        if (FrameType > MAX_QUIC_FRAME) {
            EventWriteQuicConnError(Connection, "Unknown frame type");
            QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
            return FALSE;
        }

        //
        // Validate allowable frames based on the packet type.
        //
        if (EncryptLevel != QUIC_ENCRYPT_LEVEL_1_RTT) {
            switch (FrameType) {
            //
            // The following frames are allowed pre-1-RTT encryption level:
            //
            case QUIC_FRAME_PADDING:
            case QUIC_FRAME_PING:
            case QUIC_FRAME_ACK:
            case QUIC_FRAME_ACK_1:
            case QUIC_FRAME_CRYPTO:
            case QUIC_FRAME_CONNECTION_CLOSE:
            case QUIC_FRAME_CONNECTION_CLOSE_1:
                break;
            //
            // All other frame types are disallowed.
            //
            default:
                EventWriteQuicConnErrorStatus(Connection, FrameType, "Disallowed frame type");
                QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
                return FALSE;
            }

        } else if (Packet->KeyType == QUIC_PACKET_KEY_0_RTT) {
            switch (FrameType) {
            //
            // The following frames are are disallowed in 0-RTT.
            //
            case QUIC_FRAME_ACK:
            case QUIC_FRAME_ACK_1:
                EventWriteQuicConnErrorStatus(Connection, FrameType, "Disallowed frame type");
                QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
                return FALSE;
            //
            // All other frame types are allowed.
            //
            default:
                break;
            }
        }

        Offset += sizeof(uint8_t);

        //
        // Process the frame based on the frame type.
        //
        switch (FrameType) {

        case QUIC_FRAME_PADDING: {
            while (Offset < PayloadLength &&
                Payload[Offset] == QUIC_FRAME_PADDING) {
                Offset += sizeof(uint8_t);
            }
            break;
        }

        case QUIC_FRAME_PING: {
            //
            // No other payload. Just need to acknowledge the packet this was
            // contained in.
            //
            AckPacketImmediately = TRUE;
            break;
        }

        case QUIC_FRAME_ACK:
        case QUIC_FRAME_ACK_1: {
            BOOLEAN InvalidAckFrame;
            if (!QuicLossDetectionProcessAckFrame(
                    &Connection->LossDetection,
                    EncryptLevel,
                    FrameType,
                    PayloadLength,
                    Payload,
                    &Offset,
                    &InvalidAckFrame)) {
                if (InvalidAckFrame) {
                    EventWriteQuicConnError(Connection, "Invalid ACK frame");
                    QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
                }
                return FALSE;
            }

            break;
        }

        case QUIC_FRAME_CRYPTO: {
            QUIC_CRYPTO_EX Frame;
            if (!QuicCryptoFrameDecode(PayloadLength, Payload, &Offset, &Frame)) {
                EventWriteQuicConnError(Connection, "Decoding CRYPTO frame");
                QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
                return FALSE;
            }

            if (Closed) {
                break; // Ignore frame if we are closed.
            }

            QUIC_STATUS Status =
                QuicCryptoProcessFrame(
                    &Connection->Crypto,
                    Packet->KeyType,
                    &Frame);
            if (QUIC_SUCCEEDED(Status)) {
                AckPacketImmediately = TRUE;
                if (!QuicConnIsServer(Connection) &&
                    !Connection->State.GotFirstServerResponse) {
                    Connection->State.GotFirstServerResponse = TRUE;
                }
            } else if (Status == QUIC_STATUS_OUT_OF_MEMORY) {
                return FALSE;
            } else {
                EventWriteQuicConnError(Connection, "Invalid CRYPTO frame");
                QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
                return FALSE;
            }
            break;
        }

        case QUIC_FRAME_NEW_TOKEN: {
            QUIC_NEW_TOKEN_EX Frame;
            if (!QuicNewTokenFrameDecode(PayloadLength, Payload, &Offset, &Frame)) {
                EventWriteQuicConnError(Connection, "Decoding NEW_TOKEN frame");
                QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
                return FALSE;
            }

            if (Closed) {
                break; // Ignore frame if we are closed.
            }

            //
            // TODO - Save the token for future use.
            //

            AckPacketImmediately = TRUE;
            break;
        }

        case QUIC_FRAME_RESET_STREAM:
        case QUIC_FRAME_STOP_SENDING:
        case QUIC_FRAME_STREAM:
        case QUIC_FRAME_STREAM_1:
        case QUIC_FRAME_STREAM_2:
        case QUIC_FRAME_STREAM_3:
        case QUIC_FRAME_STREAM_4:
        case QUIC_FRAME_STREAM_5:
        case QUIC_FRAME_STREAM_6:
        case QUIC_FRAME_STREAM_7:
        case QUIC_FRAME_MAX_STREAM_DATA:
        case QUIC_FRAME_STREAM_DATA_BLOCKED: {
            if (Closed) {
                if (!QuicStreamFrameSkip(
                        FrameType, PayloadLength, Payload, &Offset)) {
                    EventWriteQuicConnError(Connection, "Skipping closed stream frame");
                    QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
                    return FALSE;
                }
                break; // Ignore frame if we are closed.
            }

            uint64_t StreamId;
            if (!QuicStreamFramePeekID(
                    PayloadLength, Payload, Offset, &StreamId)) {
                EventWriteQuicConnError(Connection, "Decoding stream ID from frame");
                QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
                return FALSE;
            }

            AckPacketImmediately = TRUE;

            BOOLEAN PeerOriginatedStream =
                QuicConnIsServer(Connection) ?
                    STREAM_ID_IS_CLIENT(StreamId) :
                    STREAM_ID_IS_SERVER(StreamId);

            if (STREAM_ID_IS_UNI_DIR(StreamId)) {
                BOOLEAN IsReceiverSideFrame =
                    FrameType == QUIC_FRAME_MAX_STREAM_DATA ||
                    FrameType == QUIC_FRAME_STOP_SENDING;
                if (PeerOriginatedStream == IsReceiverSideFrame) {
                    //
                    // For locally initiated unidirectional streams, the peer
                    // should only send receiver frame types, and vice versa
                    // for peer initiated unidirectional streams.
                    //
                    EventWriteQuicConnError(Connection, "Invalid frame on unidirectional stream");
                    QuicConnTransportError(Connection, QUIC_ERROR_STREAM_STATE_ERROR);
                    break;
                }
            }

            BOOLEAN ProtocolViolation;
            PQUIC_STREAM Stream =
                QuicStreamSetGetStreamForPeer(
                    &Connection->Streams,
                    StreamId,
                    Packet->EncryptedWith0Rtt,
                    PeerOriginatedStream,
                    &ProtocolViolation);

            if (Stream) {
                QUIC_STATUS Status =
                    QuicStreamRecv(
                        Stream,
                        Packet->EncryptedWith0Rtt,
                        FrameType,
                        PayloadLength,
                        Payload,
                        &Offset,
                        &UpdatedFlowControl);
                if (Status == QUIC_STATUS_OUT_OF_MEMORY) {
                    return FALSE;
                } else if (QUIC_FAILED(Status)) {
                    EventWriteQuicConnError(Connection, "Invalid stream frame");
                    QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
                    return FALSE;
                }

                QuicStreamRelease(Stream, QUIC_STREAM_REF_LOOKUP);

            } else if (ProtocolViolation) {
                EventWriteQuicConnError(Connection, "Getting stream from ID");
                QuicConnTransportError(Connection, QUIC_ERROR_STREAM_STATE_ERROR);
                return FALSE;
            } else {
                //
                // Didn't find a matching Stream. Skip the frame as the Stream
                // might have been closed already.
                //
                LogWarning("[conn][%p] Ignoring frame (%hu) for already closed stream id = %llu",
                    Connection, FrameType, StreamId);
                if (!QuicStreamFrameSkip(
                        FrameType, PayloadLength, Payload, &Offset)) {
                    EventWriteQuicConnError(Connection, "Skipping ignored stream frame");
                    QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
                    return FALSE;
                }
            }

            break;
        }

        case QUIC_FRAME_MAX_DATA: {
            QUIC_MAX_DATA_EX Frame;
            if (!QuicMaxDataFrameDecode(PayloadLength, Payload, &Offset, &Frame)) {
                EventWriteQuicConnError(Connection, "Decoding MAX_DATA frame");
                QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
                return FALSE;
            }

            if (Closed) {
                break; // Ignore frame if we are closed.
            }

            if (Connection->Send.PeerMaxData < Frame.MaximumData) {
                Connection->Send.PeerMaxData = Frame.MaximumData;
                //
                // The peer has given us more allowance. Send packets from
                // any previously blocked streams.
                //
                UpdatedFlowControl = TRUE;
                QuicConnRemoveOutFlowBlockedReason(
                    Connection, QUIC_FLOW_BLOCKED_CONN_FLOW_CONTROL);
                QuicSendQueueFlush(
                    &Connection->Send, REASON_CONNECTION_FLOW_CONTROL);
            }

            AckPacketImmediately = TRUE;
            break;
        }

        case QUIC_FRAME_MAX_STREAMS:
        case QUIC_FRAME_MAX_STREAMS_1: {
            QUIC_MAX_STREAMS_EX Frame;
            if (!QuicMaxStreamsFrameDecode(FrameType, PayloadLength, Payload, &Offset, &Frame)) {
                EventWriteQuicConnError(Connection, "Decoding MAX_STREAMS frame");
                QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
                return FALSE;
            }

            if (Closed) {
                break; // Ignore frame if we are closed.
            }

            if (Frame.MaximumStreams > QUIC_TP_MAX_MAX_STREAMS) {
                QuicConnTransportError(Connection, QUIC_ERROR_STREAM_LIMIT_ERROR);
                break;
            }

            QuicStreamSetUpdateMaxStreams(
                &Connection->Streams,
                Frame.BidirectionalStreams,
                Frame.MaximumStreams);

            AckPacketImmediately = TRUE;
            break;
        }

        case QUIC_FRAME_DATA_BLOCKED: {
            QUIC_DATA_BLOCKED_EX Frame;
            if (!QuicDataBlockedFrameDecode(PayloadLength, Payload, &Offset, &Frame)) {
                EventWriteQuicConnError(Connection, "Decoding BLOCKED frame");
                QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
                return FALSE;
            }

            if (Closed) {
                break; // Ignore frame if we are closed.
            }

            //
            // TODO - Should we do anything else with this?
            //
            LogVerbose("[conn][%p] Peer Connection FC blocked (%llu).", Connection, Frame.DataLimit);
            QuicSendSetSendFlag(&Connection->Send, QUIC_CONN_SEND_FLAG_MAX_DATA);

            AckPacketImmediately = TRUE;
            break;
        }

        case QUIC_FRAME_STREAMS_BLOCKED:
        case QUIC_FRAME_STREAMS_BLOCKED_1: {
            QUIC_STREAMS_BLOCKED_EX Frame;
            if (!QuicStreamsBlockedFrameDecode(FrameType, PayloadLength, Payload, &Offset, &Frame)) {
                EventWriteQuicConnError(Connection, "Decoding STREAMS_BLOCKED frame");
                QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
                return FALSE;
            }

            if (Closed) {
                break; // Ignore frame if we are closed.
            }

            LogVerbose("[conn][%p] Peer Streams[%hu] FC blocked (%llu).", Connection, Frame.BidirectionalStreams, Frame.StreamLimit);
            AckPacketImmediately = TRUE;

            QUIC_CONNECTION_EVENT Event;
            Event.Type = QUIC_CONNECTION_EVENT_PEER_NEEDS_STREAMS; // TODO - Uni/Bidi
            LogVerbose("[conn][%p] Indicating QUIC_CONNECTION_EVENT_PEER_NEEDS_STREAMS",
                Connection);
            (void)QuicConnIndicateEvent(Connection, &Event);
            break;
        }

        case QUIC_FRAME_NEW_CONNECTION_ID: {
            QUIC_NEW_CONNECTION_ID_EX Frame;
            if (!QuicNewConnectionIDFrameDecode(PayloadLength, Payload, &Offset, &Frame)) {
                EventWriteQuicConnError(Connection, "Decoding NEW_CONNECTION_ID frame");
                QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
                return FALSE;
            }

            if (Closed) {
                break; // Ignore frame if we are closed.
            }

            if (Connection->DestCIDCount < QUIC_ACTIVE_CONNECTION_ID_LIMIT) {
                QUIC_CID_QUIC_LIST_ENTRY* DestCID =
                    QuicCidNewDestination(
                        Frame.Length,
                        Frame.Buffer);
                if (DestCID == NULL) {
                    EventWriteQuicAllocFailure("new DestCID", sizeof(QUIC_CID_QUIC_LIST_ENTRY) + Frame.Length);
                } else {
                    DestCID->CID.HasResetToken = TRUE;
                    DestCID->CID.SequenceNumber = Frame.Sequence;
                    QuicCopyMemory(
                        DestCID->ResetToken,
                        Frame.Buffer + Frame.Length,
                        QUIC_STATELESS_RESET_TOKEN_LENGTH);
                    EventWriteQuicConnDestCidAdded(Connection, DestCID->CID.Length, DestCID->CID.Data);
                    QuicListInsertTail(&Connection->DestCIDs, &DestCID->Link);
                    Connection->DestCIDCount++;
                }
            } else {
                LogWarning("[conn][%p] Ignoring new CID from peer, as we have hit our limit (%hu).", Connection, QUIC_ACTIVE_CONNECTION_ID_LIMIT);
            }

            AckPacketImmediately = TRUE;
            break;
        }

        case QUIC_FRAME_RETIRE_CONNECTION_ID: {
            QUIC_RETIRE_CONNECTION_ID_EX Frame;
            if (!QuicRetireConnectionIDFrameDecode(PayloadLength, Payload, &Offset, &Frame)) {
                EventWriteQuicConnError(Connection, "Decoding RETIRE_CONNECTION_ID frame");
                QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
                return FALSE;
            }

            if (Closed) {
                break; // Ignore frame if we are closed.
            }

            BOOLEAN IsLastCid;
            QUIC_CID_HASH_ENTRY* SourceCid =
                QuicConnGetSourceCidFromSeq(
                    Connection,
                    Frame.Sequence,
                    TRUE,
                    &IsLastCid);
            if (SourceCid != NULL) {
                QuicBindingRemoveSourceConnectionID(Connection->Binding, SourceCid);
                EventWriteQuicConnDestCidRemoved(
                    Connection, SourceCid->CID.Length, SourceCid->CID.Data);
                QUIC_FREE(SourceCid);
                if (IsLastCid) {
                    EventWriteQuicConnError(Connection, "Last Source CID Retired!");
                    QuicConnCloseLocally(
                        Connection,
                        QUIC_CLOSE_INTERNAL_SILENT,
                        QUIC_ERROR_PROTOCOL_VIOLATION,
                        NULL);
                } else {
                    (void)QuicConnGenerateNewSourceCid(Connection, FALSE);
                }
            }

            AckPacketImmediately = TRUE;
            break;
        }

        case QUIC_FRAME_PATH_CHALLENGE: {
            QUIC_PATH_CHALLENGE_EX Frame;
            if (!QuicPathChallengeFrameDecode(PayloadLength, Payload, &Offset, &Frame)) {
                EventWriteQuicConnError(Connection, "Decoding PATH_CHALLENGE frame");
                QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
                return FALSE;
            }

            if (Closed) {
                break; // Ignore frame if we are closed.
            }

            if (memcmp(
                    Connection->Send.LastPathChallengeReceived,
                    Frame.Data,
                    sizeof(Frame.Data)) != 0) {
                //
                // This is a new path challenge that we need to respond to with
                // a path response frame.
                //
                QuicCopyMemory(
                    Connection->Send.LastPathChallengeReceived,
                    Frame.Data,
                    sizeof(Frame.Data));
                QuicSendSetSendFlag(&Connection->Send, QUIC_CONN_SEND_FLAG_PATH_RESPONSE);
            }

            AckPacketImmediately = TRUE;
            break;
        }

        case QUIC_FRAME_PATH_RESPONSE: {
            QUIC_PATH_RESPONSE_EX Frame;
            if (!QuicPathChallengeFrameDecode(PayloadLength, Payload, &Offset, &Frame)) {
                EventWriteQuicConnError(Connection, "Decoding PATH_RESPONSE frame");
                QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
                return FALSE;
            }

            if (Closed) {
                break; // Ignore frame if we are closed.
            }

            //
            // TODO - Process Frame.
            //

            AckPacketImmediately = TRUE;
            break;
        }

        case QUIC_FRAME_CONNECTION_CLOSE:
        case QUIC_FRAME_CONNECTION_CLOSE_1: {
            QUIC_CONNECTION_CLOSE_EX Frame;
            if (!QuicConnCloseFrameDecode(FrameType, PayloadLength, Payload, &Offset, &Frame)) {
                EventWriteQuicConnError(Connection, "Decoding CONNECTION_CLOSE frame");
                QuicConnTransportError(Connection, QUIC_ERROR_FRAME_ENCODING_ERROR);
                return FALSE;
            }

            uint32_t Flags = QUIC_CLOSE_REMOTE | QUIC_CLOSE_SEND_NOTIFICATION;
            if (Frame.ApplicationClosed) {
                Flags |= QUIC_CLOSE_APPLICATION;
            }
            QuicConnTryClose(
                Connection,
                Flags,
                Frame.ErrorCode,
                Frame.ReasonPhrase,
                (uint16_t)Frame.ReasonPhraseLength);

            AckPacketImmediately = TRUE;

            if (Connection->State.HandleClosed) {
                //
                // If we are now closed, we should exit immediately. No need to
                // parse anything else.
                //
                goto Done;
            }
            break;
        }

        default:
            //
            // No default case necessary, as we have already validated the frame
            // type initially, but included for clang the compiler.
            //
            break;
        }
    }

Done:

    if (UpdatedFlowControl) {
        QuicConnLogOutFlowStats(Connection);
    }

    if (Connection->State.HandleShutdown || Connection->State.HandleClosed) {
        LogPacketInfo("[%c][RX][%llu] not acked (connection is closed)",
            PtkConnPre(Connection), Packet->PacketNumber);

    } else if (Connection->Packets[EncryptLevel] != NULL) {

        if (Connection->Packets[EncryptLevel]->NextRecvPacketNumber <= Packet->PacketNumber) {
            Connection->Packets[EncryptLevel]->NextRecvPacketNumber = Packet->PacketNumber + 1;
            Packet->NewLargestPacketNumber = TRUE;
        }

        QuicAckTrackerAckPacket(
            &Connection->Packets[EncryptLevel]->AckTracker,
            Packet->PacketNumber,
            AckPacketImmediately);
    }

    Packet->CompletelyValid = TRUE;

    return TRUE;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnRecvPostProcessing(
    _In_ PQUIC_CONNECTION Connection,
    _In_ QUIC_RECV_PACKET* Packet
    )
{
    if (Packet->DestCIDLen != 0) {
        QUIC_CID_HASH_ENTRY* SourceCid =
            QuicConnGetSourceCidFromBuf(
                Connection,
                Packet->DestCIDLen,
                Packet->DestCID);
        if (SourceCid != NULL && !SourceCid->CID.UsedByPeer) {
            LogInfo("[conn][%p] First usage of SrcCID:%s",
                Connection, QuicCidBufToStr(Packet->DestCID, Packet->DestCIDLen).Buffer);
            SourceCid->CID.UsedByPeer = TRUE;

            if (SourceCid->CID.IsInitial) {
                if (QuicConnIsServer(Connection) && SourceCid->Link.Next != NULL) {
                    QUIC_CID_HASH_ENTRY* NextSourceCid =
                        QUIC_CONTAINING_RECORD(
                            SourceCid->Link.Next,
                            QUIC_CID_HASH_ENTRY,
                            Link);
                    if (NextSourceCid->CID.IsInitial) {
                        //
                        // The client has started using our new initial CID. We
                        // can discard the old (client chosen) one now.
                        //
                        SourceCid->Link.Next = NextSourceCid->Link.Next;
                        QuicBindingRemoveSourceConnectionID(
                            Connection->Binding, NextSourceCid);
                        EventWriteQuicConnDestCidRemoved(
                            Connection, NextSourceCid->CID.Length, NextSourceCid->CID.Data);
                        QUIC_FREE(NextSourceCid);
                    }
                }
            } else {
                //
                // If we didn't initiate the CID change locally, we need to
                // respond to this change with a change of our own.
                //
                if (!Connection->State.InitiatedCidUpdate) {
                    QuicConnRetireCurrentDestCid(Connection);
                } else {
                    Connection->State.InitiatedCidUpdate = FALSE;
                }
            }
        }
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnRecvRemoteAddrChanged(
    _In_ PQUIC_CONNECTION Connection,
    _In_ const QUIC_ADDR* NewRemoteAddress
    )
{
    QUIC_FRE_ASSERT(FALSE); // TODO - Remove this when migration support is added.

    if (!Connection->State.Connected) {
        EventWriteQuicConnError(Connection, "Remote address changed during handshake");
        QuicConnTransportError(Connection, QUIC_ERROR_PROTOCOL_VIOLATION);
        return;
    }

    //
    // TODO - Validate remote address change.
    //

    EventWriteQuicConnRemoteAddrAdded(
        Connection,
        LOG_ADDR_LEN(*NewRemoteAddress),
        (const uint8_t*)NewRemoteAddress);
    Connection->RemoteAddress = *NewRemoteAddress;

    QUIC_CONNECTION_EVENT Event;
    Event.Type = QUIC_CONNECTION_EVENT_PEER_ADDRESS_CHANGED;
    Event.PEER_ADDRESS_CHANGED.Address = &Connection->RemoteAddress;
    LogVerbose("[conn][%p] Indicating QUIC_CONNECTION_EVENT_PEER_ADDRESS_CHANGED", Connection);
    (void)QuicConnIndicateEvent(Connection, &Event);

    //
    // TODO - Indicate immediate retransmit of pending sends?
    //
}

_IRQL_requires_max_(PASSIVE_LEVEL)
BOOLEAN
QuicConnRecvBatch(
    _In_ PQUIC_CONNECTION Connection,
    _In_ uint8_t BatchCount,
    _In_reads_(BatchCount) QUIC_RECV_DATAGRAM** Datagrams,
    _In_reads_(BatchCount * QUIC_HP_SAMPLE_LENGTH)
        const uint8_t* Cipher
    )
{
    BOOLEAN UpdateIdleTimeout = FALSE;
    uint8_t HpMask[QUIC_HP_SAMPLE_LENGTH * QUIC_MAX_CRYPTO_BATCH_COUNT];

    QUIC_DBG_ASSERT(BatchCount > 0 && BatchCount <= QUIC_MAX_CRYPTO_BATCH_COUNT);
    QUIC_RECV_PACKET* Packet = QuicDataPathRecvDatagramToRecvPacket(Datagrams[0]);

    LogVerbose("[conn][%p] Batch Recv %u UDP datagrams", Connection, BatchCount);

    if (Connection->Crypto.TlsState.ReadKeys[Packet->KeyType] == NULL) {
        QuicPacketLogDrop(Connection, Packet, "Key no longer accepted (batch)");
        return FALSE;
    }

    if (Connection->State.EncryptionEnabled &&
        Connection->State.HeaderProtectionEnabled) {
        if (QUIC_FAILED(
            QuicHpComputeMask(
                Connection->Crypto.TlsState.ReadKeys[Packet->KeyType]->HeaderKey,
                BatchCount,
                Cipher,
                HpMask))) {
            QuicPacketLogDrop(Connection, Packet, "Failed to compute HP mask");
            return FALSE;
        }
    } else {
        QuicZeroMemory(HpMask, BatchCount * QUIC_HP_SAMPLE_LENGTH);
    }

    for (uint8_t i = 0; i < BatchCount; ++i) {
        QUIC_DBG_ASSERT(Datagrams[i]->Allocated);
        Packet = QuicDataPathRecvDatagramToRecvPacket(Datagrams[i]);
        if (QuicConnRecvPrepareDecrypt(
                Connection, Packet, HpMask + i * QUIC_HP_SAMPLE_LENGTH) &&
            QuicConnRecvDecryptAndAuthenticate(Connection, Packet) &&
            QuicConnRecvPayload(Connection, Packet)) {

            QuicConnRecvPostProcessing(Connection, Packet);
            UpdateIdleTimeout |= Packet->CompletelyValid;

            if (Packet->IsShortHeader && Packet->NewLargestPacketNumber) {

                if (QuicConnIsServer(Connection)) {
                    Connection->Send.SpinBit = Packet->SH->SpinBit;
                } else {
                    Connection->Send.SpinBit = !Packet->SH->SpinBit;
                }

                if (!QuicAddrCompare(
                        &Datagrams[i]->Tuple->RemoteAddress,
                        &Connection->RemoteAddress)) {
                    QuicConnRecvRemoteAddrChanged(
                        Connection,
                        &Datagrams[i]->Tuple->RemoteAddress);
                }
            }

        } else {
            Connection->Stats.Recv.DroppedPackets++;
        }
    }

    return UpdateIdleTimeout;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnRecvDatagrams(
    _In_ PQUIC_CONNECTION Connection,
    _In_ QUIC_RECV_DATAGRAM* DatagramChain,
    _In_ uint32_t DatagramChainCount,
    _In_ BOOLEAN IsDeferredDatagram
    )
{
    QUIC_RECV_DATAGRAM* ReleaseChain = NULL;
    QUIC_RECV_DATAGRAM** ReleaseChainTail = &ReleaseChain;
    uint32_t ReleaseChainCount = 0;
    BOOLEAN UpdateIdleTimeout = FALSE;

    QUIC_PASSIVE_CODE();

    if (IsDeferredDatagram) {
        LogVerbose("[conn][%p] Recv %u deferred UDP datagrams", Connection, DatagramChainCount);
    } else {
        LogVerbose("[conn][%p] Recv %u UDP datagrams", Connection, DatagramChainCount);
    }

    //
    // Iterate through each QUIC packet in the chain of UDP datagrams until an
    // error is encountered or we run out of buffer.
    //

    uint8_t BatchCount = 0;
    QUIC_RECV_DATAGRAM* Batch[QUIC_MAX_CRYPTO_BATCH_COUNT];
    uint8_t Cipher[QUIC_HP_SAMPLE_LENGTH * QUIC_MAX_CRYPTO_BATCH_COUNT];

    QUIC_RECV_DATAGRAM* Datagram;
    while ((Datagram = DatagramChain) != NULL) {
        QUIC_DBG_ASSERT(Datagram->Allocated);
        QUIC_DBG_ASSERT(Datagram->QueuedOnConnection);
        DatagramChain = Datagram->Next;
        Datagram->Next = NULL;

        QUIC_RECV_PACKET* Packet =
            QuicDataPathRecvDatagramToRecvPacket(Datagram);
        QUIC_DBG_ASSERT(Packet != NULL);

        QUIC_DBG_ASSERT(Packet->DecryptionDeferred == IsDeferredDatagram);
        BOOLEAN WasDeferredPreviously = Packet->DecryptionDeferred;
        UNREFERENCED_PARAMETER(WasDeferredPreviously);
        Packet->DecryptionDeferred = FALSE;

        if (!IsDeferredDatagram) {
            Connection->Stats.Recv.TotalBytes += Datagram->BufferLength;
            QuicConnLogInFlowStats(Connection);

            if (!Connection->State.SourceAddressValidated) {
                QuicSendIncrementAllowance(
                    &Connection->Send,
                    QUIC_AMPLIFICATION_RATIO * Datagram->BufferLength);
            }
        }

        if (!QuicAddrCompare( // TODO - Remove this restriction once migration is supported.
                &Datagram->Tuple->RemoteAddress,
                &Connection->RemoteAddress)) {
            QuicPacketLogDrop(Connection, Packet, "Different remote address");
            goto Drop;
        }

        do {
            QUIC_DBG_ASSERT(BatchCount < QUIC_MAX_CRYPTO_BATCH_COUNT);
            QUIC_DBG_ASSERT(Datagram->Allocated);
            Connection->Stats.Recv.TotalPackets++;

            Packet->BufferLength =
                Datagram->BufferLength - (uint16_t)(Packet->Buffer - Datagram->Buffer);

            if (!QuicConnRecvHeader(
                    Connection,
                    Packet,
                    Cipher + BatchCount * QUIC_HP_SAMPLE_LENGTH)) {
                if (Packet->DecryptionDeferred) {
                    QUIC_DBG_ASSERT(!WasDeferredPreviously); // Should never be deferred twice.
                    Connection->Stats.Recv.TotalPackets--; // Don't count the packet right now.
                } else {
                    Connection->Stats.Recv.DroppedPackets++;
                    if (!Packet->IsShortHeader && Packet->ValidatedHeaderVer) {
                        goto NextPacket;
                    }
                }
                break;
            }

            if (!Packet->IsShortHeader && BatchCount != 0) {
                //
                // We already had some batched short header packets and then
                // encountered a long header packet. Finish off the short
                // headers first and then continue with the current packet.
                //
                UpdateIdleTimeout |=
                    QuicConnRecvBatch(Connection, BatchCount, Batch, Cipher);
                QuicMoveMemory(
                    Cipher + BatchCount * QUIC_HP_SAMPLE_LENGTH,
                    Cipher,
                    QUIC_HP_SAMPLE_LENGTH);
                BatchCount = 0;
            }

            Batch[BatchCount++] = Datagram;
            if (Packet->IsShortHeader && BatchCount < QUIC_MAX_CRYPTO_BATCH_COUNT) {
                break;
            }

            UpdateIdleTimeout |=
                QuicConnRecvBatch(Connection, BatchCount, Batch, Cipher);
            BatchCount = 0;

            if (Packet->IsShortHeader) {
                break; // Short header packets aren't followed by additional packets.
            }

            //
            // Move to the next QUIC packet (if available) and reset the packet
            // state.
            //

        NextPacket:

            Packet->Buffer += Packet->BufferLength;

            Packet->ValidatedHeaderInv = FALSE;
            Packet->ValidatedHeaderVer = FALSE;
            Packet->ValidToken = FALSE;
            Packet->PacketNumberSet = FALSE;
            Packet->EncryptedWith0Rtt = FALSE;
            Packet->DecryptionDeferred = FALSE;
            Packet->CompletelyValid = FALSE;
            Packet->NewLargestPacketNumber = FALSE;

        } while (Packet->Buffer - Datagram->Buffer < Datagram->BufferLength);

    Drop:

        if (!Packet->DecryptionDeferred) {
            *ReleaseChainTail = Datagram;
            ReleaseChainTail = &Datagram->Next;
            Datagram->QueuedOnConnection = FALSE;
            if (++ReleaseChainCount == QUIC_MAX_RECEIVE_BATCH_COUNT) {
                if (BatchCount != 0) {
                    UpdateIdleTimeout |=
                        QuicConnRecvBatch(Connection, BatchCount, Batch, Cipher);
                    BatchCount = 0;
                }
                QuicDataPathBindingReturnRecvDatagrams(ReleaseChain);
                ReleaseChain = NULL;
                ReleaseChainTail = &ReleaseChain;
                ReleaseChainCount = 0;
            }
        }
    }

    if (BatchCount != 0) {
        UpdateIdleTimeout |=
            QuicConnRecvBatch(Connection, BatchCount, Batch, Cipher);
        BatchCount = 0;
    }

    if (UpdateIdleTimeout) {
        QuicConnResetIdleTimeout(Connection);
    }

    if (ReleaseChain != NULL) {
        QuicDataPathBindingReturnRecvDatagrams(ReleaseChain);
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnFlushRecv(
    _In_ PQUIC_CONNECTION Connection
    )
{
    uint32_t ReceiveQueueCount;
    QUIC_RECV_DATAGRAM* ReceiveQueue;

    QuicDispatchLockAcquire(&Connection->ReceiveQueueLock);
    ReceiveQueueCount = Connection->ReceiveQueueCount;
    Connection->ReceiveQueueCount = 0;
    ReceiveQueue = Connection->ReceiveQueue;
    Connection->ReceiveQueue = NULL;
    Connection->ReceiveQueueTail = &Connection->ReceiveQueue;
    QuicDispatchLockRelease(&Connection->ReceiveQueueLock);

    QuicConnRecvDatagrams(
        Connection, ReceiveQueue, ReceiveQueueCount, FALSE);

    if (Connection->Session == NULL) {
        //
        // This means an initial packet failed to initialize
        // the connection.
        //
        QuicConnSilentlyAbort(Connection);
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnFlushDeferred(
    _In_ PQUIC_CONNECTION Connection
    )
{
    for (uint8_t i = 1; i <= (uint8_t)Connection->Crypto.TlsState.ReadKey; ++i) {

        if (Connection->Crypto.TlsState.ReadKeys[i] == NULL) {
            continue;
        }

        QUIC_ENCRYPT_LEVEL EncryptLevel =
            QuicKeyTypeToEncryptLevel((QUIC_PACKET_KEY_TYPE)i);
        PQUIC_PACKET_SPACE Packets = Connection->Packets[EncryptLevel];

        if (Packets->DeferredDatagrams != NULL) {
            QUIC_RECV_DATAGRAM* DeferredDatagrams = Packets->DeferredDatagrams;
            uint8_t DeferredDatagramsCount = Packets->DeferredDatagramsCount;

            Packets->DeferredDatagramsCount = 0;
            Packets->DeferredDatagrams = NULL;

            QuicConnRecvDatagrams(
                Connection,
                DeferredDatagrams,
                DeferredDatagramsCount,
                TRUE);
        }
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnProcessUdpUnreachable(
    _In_ PQUIC_CONNECTION Connection,
    _In_ const QUIC_ADDR* RemoteAddress
    )
{
    if (Connection->Crypto.TlsState.ReadKey > QUIC_PACKET_KEY_INITIAL) {
        //
        // Only accept unreachable events at the beginning of the handshake.
        // Otherwise, it opens up an attack surface.
        //
        LogWarning("[conn][%p] Ignoring received unreachable event.", Connection);

    } else if (QuicAddrCompare(&Connection->RemoteAddress, RemoteAddress)) {
        LogInfo("[conn][%p] Received unreachable event.", Connection);
        //
        // Close the connection since the peer is unreachable.
        //
        QuicConnCloseLocally(
            Connection,
            QUIC_CLOSE_INTERNAL_SILENT | QUIC_CLOSE_QUIC_STATUS,
            (uint64_t)QUIC_STATUS_UNREACHABLE,
            NULL);

    } else {
        LogWarning("[conn][%p] Received invalid unreachable event.", Connection);
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnResetIdleTimeout(
    _In_ PQUIC_CONNECTION Connection
    )
{
    //
    // Use the (non-zero) min value between local and peer's configuration.
    //
    uint64_t IdleTimeoutMs = Connection->PeerTransportParams.IdleTimeout;
    if (IdleTimeoutMs == 0 ||
        (Connection->IdleTimeoutMs != 0 && Connection->IdleTimeoutMs < IdleTimeoutMs)) {
        IdleTimeoutMs = Connection->IdleTimeoutMs;
    }

    if (IdleTimeoutMs != 0) {
        //
        // Idle timeout must be no less than the PTOs for closing.
        //
        uint32_t MinIdleTimeoutMs =
            US_TO_MS(QuicLossDetectionComputeProbeTimeout(
                &Connection->LossDetection,
                QUIC_CLOSE_PTO_COUNT));
        if (IdleTimeoutMs < MinIdleTimeoutMs) {
            IdleTimeoutMs = MinIdleTimeoutMs;
        }

        QuicConnTimerSet(Connection, QUIC_CONN_TIMER_IDLE, IdleTimeoutMs);
    }

    if (Connection->KeepAliveIntervalMs != 0) {
        QuicConnTimerSet(
            Connection,
            QUIC_CONN_TIMER_KEEP_ALIVE,
            Connection->KeepAliveIntervalMs);
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnProcessIdleTimerOperation(
    _In_ PQUIC_CONNECTION Connection
    )
{
    //
    // Close the connection, as the agreed-upon idle time period has elapsed.
    //
    QuicConnCloseLocally(
        Connection,
        QUIC_CLOSE_INTERNAL_SILENT | QUIC_CLOSE_QUIC_STATUS,
        (uint64_t)QUIC_STATUS_CONNECTION_IDLE,
        NULL);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnProcessKeepAliveOperation(
    _In_ PQUIC_CONNECTION Connection
    )
{
    //
    // Send a PING frame to keep the connection alive.
    //
    Connection->Send.TailLossProbeNeeded = TRUE;
    QuicSendSetSendFlag(&Connection->Send, QUIC_CONN_SEND_FLAG_PING);

    //
    // Restart the keep alive timer.
    //
    QuicConnTimerSet(
        Connection,
        QUIC_CONN_TIMER_KEEP_ALIVE,
        Connection->KeepAliveIntervalMs);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
QuicConnParamSet(
    _In_ PQUIC_CONNECTION Connection,
    _In_ uint32_t Param,
    _In_ uint32_t BufferLength,
    _In_reads_bytes_(BufferLength)
        const void* Buffer
    )
{
    QUIC_STATUS Status;

    switch (Param) {

    case QUIC_PARAM_CONN_QUIC_VERSION:

        if (BufferLength != sizeof(uint32_t)) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        //
        // Validate new version. We allow the application to set a reserved
        // version number to force version negotiation.
        //
        uint32_t NewVersion = QuicByteSwapUint32(*(uint32_t*)Buffer);
        if (!QuicIsVersionSupported(NewVersion) &&
            !QuicIsVersionReserved(NewVersion)) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        //
        // Only allowed before connection attempt.
        //
        if (Connection->State.Started) {
            Status = QUIC_STATUS_INVALID_STATE;
            break;
        }

        Connection->Stats.QuicVersion = NewVersion;
        QuicConnOnQuicVersionSet(Connection);

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_LOCAL_ADDRESS: {

        if (BufferLength != sizeof(QUIC_ADDR)) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        if (Connection->Type == QUIC_HANDLE_TYPE_CHILD) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        const QUIC_ADDR* LocalAddress = (const QUIC_ADDR*)Buffer;

        if (!QuicAddrIsValid(LocalAddress)) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        Connection->State.LocalAddressSet = TRUE;
        QuicCopyMemory(&Connection->LocalAddress, Buffer, sizeof(QUIC_ADDR));
        EventWriteQuicConnLocalAddrAdded(
            Connection,
            LOG_ADDR_LEN(Connection->LocalAddress),
            (const uint8_t*)&Connection->LocalAddress);

        if (Connection->State.Connected) {

            QUIC_DBG_ASSERT(Connection->Binding);
            QUIC_DBG_ASSERT(Connection->State.RemoteAddressSet);

            PQUIC_BINDING OldBinding = Connection->Binding;

            Status =
                QuicLibraryGetBinding(
                    Connection->Session,
                    Connection->State.ShareBinding,
                    LocalAddress,
                    &Connection->RemoteAddress,
                    &Connection->Binding);
            if (QUIC_FAILED(Status)) {
                Connection->Binding = OldBinding;
                break;
            }

            //
            // TODO - Need to free any queued recv packets from old binding.
            //

            QuicBindingMoveSourceConnectionIDs(
                OldBinding, Connection->Binding, Connection);
            if (!Connection->State.Connected) {
                InterlockedDecrement(&OldBinding->HandshakeConnections);
                InterlockedExchangeAdd64(
                    (LONG64*)&MsQuicLib.CurrentHandshakeMemoryUsage,
                    -1 * (LONG64)QUIC_CONN_HANDSHAKE_MEMORY_USAGE);
            }
            QuicLibraryReleaseBinding(OldBinding);
            EventWriteQuicConnLocalAddrRemoved(
                Connection,
                LOG_ADDR_LEN(Connection->LocalAddress),
                (const uint8_t*)&Connection->LocalAddress);

            QuicDataPathBindingGetLocalAddress(
                Connection->Binding->DatapathBinding,
                &Connection->LocalAddress);
            EventWriteQuicConnLocalAddrAdded(
                Connection,
                LOG_ADDR_LEN(Connection->LocalAddress),
                (const uint8_t*)&Connection->LocalAddress);

            QuicSendSetSendFlag(&Connection->Send, QUIC_CONN_SEND_FLAG_PING);
        }

        Status = QUIC_STATUS_SUCCESS;
        break;
    }

    case QUIC_PARAM_CONN_REMOTE_ADDRESS: {

        if (BufferLength != sizeof(QUIC_ADDR)) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        if (Connection->Type == QUIC_HANDLE_TYPE_CHILD) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        if (Connection->State.Started) {
            Status = QUIC_STATUS_INVALID_STATE;
            break;
        }

        Connection->State.RemoteAddressSet = TRUE;
        QuicCopyMemory(&Connection->RemoteAddress, Buffer, sizeof(QUIC_ADDR));
        //
        // Don't log new Remote address added here because it is logged when
        // the connection is started.
        //

        Status = QUIC_STATUS_SUCCESS;
        break;
    }

    case QUIC_PARAM_CONN_IDLE_TIMEOUT:

        if (BufferLength != sizeof(Connection->IdleTimeoutMs)) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        if (Connection->State.Started) {
            Status = QUIC_STATUS_INVALID_STATE;
            break;
        }

        Connection->IdleTimeoutMs = *(uint64_t*)Buffer;

        LogInfo("[conn][%p] Updated idle timeout to %llu milliseconds",
            Connection, Connection->IdleTimeoutMs);

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_PEER_BIDI_STREAM_COUNT:

        if (BufferLength != sizeof(uint16_t)) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        QuicStreamSetUpdateMaxCount(
            &Connection->Streams,
            QuicConnIsServer(Connection) ?
                STREAM_ID_FLAG_IS_CLIENT | STREAM_ID_FLAG_IS_BI_DIR :
                STREAM_ID_FLAG_IS_SERVER | STREAM_ID_FLAG_IS_BI_DIR,
            *(uint16_t*)Buffer);

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_PEER_UNIDI_STREAM_COUNT:

        if (BufferLength != sizeof(uint16_t)) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        QuicStreamSetUpdateMaxCount(
            &Connection->Streams,
            QuicConnIsServer(Connection) ?
                STREAM_ID_FLAG_IS_CLIENT | STREAM_ID_FLAG_IS_UNI_DIR :
                STREAM_ID_FLAG_IS_SERVER | STREAM_ID_FLAG_IS_UNI_DIR,
            *(uint16_t*)Buffer);

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_CLOSE_REASON_PHRASE:

        if (BufferLength >= 513) { // TODO - Practically, must fit in 1 packet.
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        //
        // Require the reason to be null terminated.
        //
        if (Buffer && ((char*)Buffer)[BufferLength - 1] != 0) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        //
        // Free any old data.
        //
        if (Connection->CloseReasonPhrase != NULL) {
            QUIC_FREE(Connection->CloseReasonPhrase);
        }

        //
        // Allocate new space.
        //
        Connection->CloseReasonPhrase =
            QUIC_ALLOC_NONPAGED(BufferLength);

        if (Connection->CloseReasonPhrase != NULL) {
            QuicCopyMemory(
                Connection->CloseReasonPhrase,
                Buffer,
                BufferLength);

            Status = QUIC_STATUS_SUCCESS;

        } else {
            Status = QUIC_STATUS_OUT_OF_MEMORY;
        }

        break;

    case QUIC_PARAM_CONN_CERT_VALIDATION_FLAGS:

        if (BufferLength != sizeof(Connection->ServerCertValidationFlags)) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        if (QuicConnIsServer(Connection) || Connection->State.Started) {
            //
            // Only allowed on client connections, before the connection starts.
            //
            Status = QUIC_STATUS_INVALID_STATE;
            break;
        }

        Connection->ServerCertValidationFlags = *(uint32_t*)Buffer;

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_KEEP_ALIVE:

        if (BufferLength != sizeof(Connection->KeepAliveIntervalMs)) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        if (Connection->State.Started &&
            Connection->KeepAliveIntervalMs != 0) {
            //
            // Cancel any current timer first.
            //
            QuicConnTimerCancel(Connection, QUIC_CONN_TIMER_KEEP_ALIVE);
        }

        Connection->KeepAliveIntervalMs = *(uint32_t*)Buffer;

        LogInfo("[conn][%p] Updated keep alive interval to %u milliseconds",
            Connection, Connection->KeepAliveIntervalMs);

        if (Connection->State.Started &&
            Connection->KeepAliveIntervalMs != 0) {
            QuicConnProcessKeepAliveOperation(Connection);
        }

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_DISCONNECT_TIMEOUT:

        if (BufferLength != sizeof(Connection->DisconnectTimeoutUs)) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        if (*(uint32_t*)Buffer == 0 ||
            *(uint32_t*)Buffer > QUIC_MAX_DISCONNECT_TIMEOUT) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        Connection->DisconnectTimeoutUs = MS_TO_US(*(uint32_t*)Buffer);

        LogInfo("[conn][%p] Updated disconnect timeout = %u milliseconds",
            Connection, *(uint32_t*)Buffer);

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_SEC_CONFIG: {

        if (BufferLength != sizeof(QUIC_SEC_CONFIG*)) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        QUIC_SEC_CONFIG* SecConfig = *(QUIC_SEC_CONFIG**)Buffer;

        if (SecConfig == NULL) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        if (!QuicConnIsServer(Connection) ||
            Connection->State.ListenerAccepted == FALSE ||
            Connection->Crypto.TLS != NULL) {
            Status = QUIC_STATUS_INVALID_STATE;
            break;
        }

        LogInfo("[conn][%p] Security config set, %p.", Connection, SecConfig);
        (void)QuicTlsSecConfigAddRef(SecConfig);

        Status =
            QuicConnHandshakeConfigure(
                Connection,
                SecConfig);
        if (QUIC_FAILED(Status)) {
            break;
        }

        QuicCryptoProcessData(&Connection->Crypto, FALSE);
        break;
    }

    case QUIC_PARAM_CONN_SEND_BUFFERING:

        if (BufferLength != sizeof(uint8_t)) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }
        Connection->State.UseSendBuffer = *(uint8_t*)Buffer;

        LogInfo("[conn][%p] Updated UseSendBuffer = %u",
            Connection, Connection->State.UseSendBuffer);

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_SEND_PACING:

        if (BufferLength != sizeof(uint8_t)) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }
        Connection->State.UsePacing = *(uint8_t*)Buffer;

        LogInfo("[conn][%p] Updated UsePacing = %u",
            Connection, Connection->State.UsePacing);

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_SHARE_UDP_BINDING:

        if (BufferLength != sizeof(uint8_t)) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        if (Connection->State.Started || QuicConnIsServer(Connection)) {
            Status = QUIC_STATUS_INVALID_STATE;
            break;
        }

        Connection->State.ShareBinding = *(uint8_t*)Buffer;

        LogInfo("[conn][%p] Updated ShareBinding = %u",
            Connection, Connection->State.ShareBinding);

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_FORCE_KEY_UPDATE:

        if (!Connection->State.Connected ||
            !Connection->State.EncryptionEnabled ||
            Connection->Packets[QUIC_ENCRYPT_LEVEL_1_RTT] == NULL ||
            Connection->Packets[QUIC_ENCRYPT_LEVEL_1_RTT]->AwaitingKeyPhaseConfirmation ||
            !Connection->State.HandshakeConfirmed) {
            Status = QUIC_STATUS_INVALID_STATE;
            break;
        }

        LogVerbose("[conn][%p] Forced key update.", Connection);

        Status = QuicCryptoGenerateNewKeys(Connection);
        if (QUIC_FAILED(Status)) {
            EventWriteQuicConnErrorStatus(Connection, Status, "Forced key update");
            break;
        }

        QuicCryptoUpdateKeyPhase(Connection, TRUE);
        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_FORCE_CID_UPDATE:

        if (!Connection->State.Connected || 
            !Connection->State.HandshakeConfirmed) {
            Status = QUIC_STATUS_INVALID_STATE;
            break;
        }

        LogVerbose("[conn][%p] Forced destination CID update.", Connection);

        Connection->State.InitiatedCidUpdate = TRUE;
        QuicConnRetireCurrentDestCid(Connection);
        Status = QUIC_STATUS_SUCCESS;
        break;

    default:
        Status = QUIC_STATUS_INVALID_PARAMETER;
        break;
    }

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
QuicConnParamGet(
    _In_ PQUIC_CONNECTION Connection,
    _In_ uint32_t Param,
    _Inout_ uint32_t* BufferLength,
    _Out_writes_bytes_opt_(*BufferLength)
        void* Buffer
    )
{
    QUIC_STATUS Status;
    uint32_t Length;
    uint8_t Type;

    switch (Param) {

    case QUIC_PARAM_CONN_QUIC_VERSION:

        if (*BufferLength < sizeof(Connection->Stats.QuicVersion)) {
            *BufferLength = sizeof(Connection->Stats.QuicVersion);
            Status = QUIC_STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (Buffer == NULL) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        *BufferLength = sizeof(Connection->Stats.QuicVersion);
        *(uint32_t*)Buffer = QuicByteSwapUint32(Connection->Stats.QuicVersion);

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_LOCAL_ADDRESS:

        if (*BufferLength < sizeof(Connection->LocalAddress)) {
            *BufferLength = sizeof(Connection->LocalAddress);
            Status = QUIC_STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (Buffer == NULL) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        if (!Connection->State.LocalAddressSet) {
            Status = QUIC_STATUS_INVALID_STATE;
            break;
        }

        *BufferLength = sizeof(Connection->LocalAddress);
        QuicCopyMemory(
            Buffer,
            &Connection->LocalAddress,
            sizeof(Connection->LocalAddress));

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_REMOTE_ADDRESS:

        if (*BufferLength < sizeof(Connection->RemoteAddress)) {
            *BufferLength = sizeof(Connection->RemoteAddress);
            Status = QUIC_STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (Buffer == NULL) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        if (!Connection->State.RemoteAddressSet) {
            Status = QUIC_STATUS_INVALID_STATE;
            break;
        }

        *BufferLength = sizeof(Connection->RemoteAddress);
        QuicCopyMemory(
            Buffer,
            &Connection->RemoteAddress,
            sizeof(Connection->RemoteAddress));

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_IDLE_TIMEOUT:

        if (*BufferLength < sizeof(Connection->IdleTimeoutMs)) {
            *BufferLength = sizeof(Connection->IdleTimeoutMs);
            Status = QUIC_STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (Buffer == NULL) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        *BufferLength = sizeof(Connection->IdleTimeoutMs);
        *(uint64_t*)Buffer = Connection->IdleTimeoutMs;

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_PEER_BIDI_STREAM_COUNT:
        Type =
            QuicConnIsServer(Connection) ?
                STREAM_ID_FLAG_IS_CLIENT | STREAM_ID_FLAG_IS_BI_DIR :
                STREAM_ID_FLAG_IS_SERVER | STREAM_ID_FLAG_IS_BI_DIR;
        goto Get_Stream_Count;
    case QUIC_PARAM_CONN_PEER_UNIDI_STREAM_COUNT:
        Type =
            QuicConnIsServer(Connection) ?
                STREAM_ID_FLAG_IS_CLIENT | STREAM_ID_FLAG_IS_UNI_DIR :
                STREAM_ID_FLAG_IS_SERVER | STREAM_ID_FLAG_IS_UNI_DIR;
        goto Get_Stream_Count;
    case QUIC_PARAM_CONN_LOCAL_BIDI_STREAM_COUNT:
        Type =
            QuicConnIsServer(Connection) ?
                STREAM_ID_FLAG_IS_SERVER | STREAM_ID_FLAG_IS_BI_DIR :
                STREAM_ID_FLAG_IS_CLIENT | STREAM_ID_FLAG_IS_BI_DIR;
        goto Get_Stream_Count;
    case QUIC_PARAM_CONN_LOCAL_UNIDI_STREAM_COUNT:
        Type =
            QuicConnIsServer(Connection) ?
                STREAM_ID_FLAG_IS_SERVER | STREAM_ID_FLAG_IS_UNI_DIR :
                STREAM_ID_FLAG_IS_CLIENT | STREAM_ID_FLAG_IS_UNI_DIR;
        goto Get_Stream_Count;

    Get_Stream_Count:
        if (*BufferLength < sizeof(uint16_t)) {
            *BufferLength = sizeof(uint16_t);
            Status = QUIC_STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (Buffer == NULL) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        *BufferLength = sizeof(uint16_t);
        *(uint16_t*)Buffer =
            QuicStreamSetGetCountAvailable(&Connection->Streams, Type);

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_CLOSE_REASON_PHRASE:

        if (Connection->CloseReasonPhrase == NULL) {
            Status = QUIC_STATUS_NOT_FOUND;
            break;
        }

        Length = (uint32_t)strlen(Connection->CloseReasonPhrase) + 1;
        if (*BufferLength < Length) {
            *BufferLength = Length;
            Status = QUIC_STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (Buffer == NULL) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        *BufferLength = Length;
        QuicCopyMemory(Buffer, Connection->CloseReasonPhrase, Length);

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_STATISTICS:
    case QUIC_PARAM_CONN_STATISTICS_PLAT: {

        if (*BufferLength < sizeof(QUIC_STATISTICS)) {
            *BufferLength = sizeof(QUIC_STATISTICS);
            Status = QUIC_STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (Buffer == NULL) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        QUIC_STATISTICS* Stats = (QUIC_STATISTICS*)Buffer;

        Stats->CorrelationId = Connection->Stats.CorrelationId;
        Stats->VersionNegotiation = Connection->Stats.VersionNegotiation;
        Stats->StatelessRetry = Connection->Stats.StatelessRetry;
        Stats->ResumptionAttempted = Connection->Stats.ResumptionAttempted;
        Stats->ResumptionSucceeded = Connection->Stats.ResumptionSucceeded;
        Stats->Rtt = Connection->SmoothedRtt;
        Stats->MinRtt = Connection->MinRtt;
        Stats->MaxRtt = Connection->MaxRtt;
        Stats->Timing.Start = Connection->Stats.Timing.Start;
        Stats->Timing.InitialFlightEnd = Connection->Stats.Timing.InitialFlightEnd;
        Stats->Timing.HandshakeFlightEnd = Connection->Stats.Timing.HandshakeFlightEnd;
        Stats->Send.PathMtu = Connection->Send.PathMtu;
        Stats->Send.TotalPackets = Connection->Stats.Send.TotalPackets;
        Stats->Send.RetransmittablePackets = Connection->Stats.Send.RetransmittablePackets;
        Stats->Send.SuspectedLostPackets = Connection->Stats.Send.SuspectedLostPackets;
        Stats->Send.SpuriousLostPackets = Connection->Stats.Send.SpuriousLostPackets;
        Stats->Send.TotalBytes = Connection->Stats.Send.TotalBytes;
        Stats->Send.TotalStreamBytes = Connection->Stats.Send.TotalStreamBytes;
        Stats->Send.CongestionCount = Connection->Stats.Send.CongestionCount;
        Stats->Send.PersistentCongestionCount = Connection->Stats.Send.PersistentCongestionCount;
        Stats->Recv.TotalPackets = Connection->Stats.Recv.TotalPackets;
        Stats->Recv.ReorderedPackets = Connection->Stats.Recv.ReorderedPackets;
        Stats->Recv.DroppedPackets = Connection->Stats.Recv.DroppedPackets;
        Stats->Recv.DuplicatePackets = Connection->Stats.Recv.DuplicatePackets;
        Stats->Recv.TotalBytes = Connection->Stats.Recv.TotalBytes;
        Stats->Recv.TotalStreamBytes = Connection->Stats.Recv.TotalStreamBytes;
        Stats->Recv.DecryptionFailures = Connection->Stats.Recv.DecryptionFailures;
        Stats->Misc.KeyUpdateCount = Connection->Stats.Misc.KeyUpdateCount;

        if (Param == QUIC_PARAM_CONN_STATISTICS_PLAT) {
            Stats->Timing.Start = QuicTimeUs64ToPlat(Stats->Timing.Start);
            Stats->Timing.InitialFlightEnd = QuicTimeUs64ToPlat(Stats->Timing.InitialFlightEnd);
            Stats->Timing.HandshakeFlightEnd = QuicTimeUs64ToPlat(Stats->Timing.HandshakeFlightEnd);
        }

        *BufferLength = sizeof(QUIC_STATISTICS);
        Status = QUIC_STATUS_SUCCESS;
        break;
    }

    case QUIC_PARAM_CONN_CERT_VALIDATION_FLAGS:

        if (*BufferLength < sizeof(Connection->ServerCertValidationFlags)) {
            *BufferLength = sizeof(Connection->ServerCertValidationFlags);
            Status = QUIC_STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (Buffer == NULL) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        *BufferLength = sizeof(Connection->ServerCertValidationFlags);
        *(uint32_t*)Buffer = Connection->ServerCertValidationFlags;

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_KEEP_ALIVE:

        if (*BufferLength < sizeof(Connection->KeepAliveIntervalMs)) {
            *BufferLength = sizeof(Connection->KeepAliveIntervalMs);
            Status = QUIC_STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (Buffer == NULL) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        *BufferLength = sizeof(Connection->KeepAliveIntervalMs);
        *(uint32_t*)Buffer = Connection->KeepAliveIntervalMs;

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_DISCONNECT_TIMEOUT:

        if (*BufferLength < sizeof(Connection->DisconnectTimeoutUs)) {
            *BufferLength = sizeof(Connection->DisconnectTimeoutUs);
            Status = QUIC_STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (Buffer == NULL) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        *BufferLength = sizeof(uint32_t);
        *(uint32_t*)Buffer = US_TO_MS(Connection->DisconnectTimeoutUs);

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_RESUMPTION_STATE: {

        if (QuicConnIsServer(Connection)) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        if (Connection->RemoteServerName == NULL) {
            Status = QUIC_STATUS_INVALID_STATE;
            break;
        }

        uint32_t RequiredBufferLength = 0;
        Status = QuicTlsReadTicket(Connection->Crypto.TLS, &RequiredBufferLength, NULL);

        if (Status != QUIC_STATUS_BUFFER_TOO_SMALL) {
            LogVerbose("[conn][%p] QuicTlsReadTicket failed, 0x%x", Connection, Status);
            break;
        }

        _Analysis_assume_(strlen(Connection->RemoteServerName) <= (size_t)UINT16_MAX);
        uint16_t RemoteServerNameLength = (uint16_t)strlen(Connection->RemoteServerName);

        QUIC_SERIALIZED_RESUMPTION_STATE* State =
            (QUIC_SERIALIZED_RESUMPTION_STATE*)Buffer;

        RequiredBufferLength += sizeof(QUIC_SERIALIZED_RESUMPTION_STATE);
        RequiredBufferLength += RemoteServerNameLength;

        if (*BufferLength < RequiredBufferLength) {
            *BufferLength = RequiredBufferLength;
            Status = QUIC_STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (Buffer == NULL) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        State->QuicVersion = Connection->Stats.QuicVersion;
        State->TransportParameters = Connection->PeerTransportParams;
        State->ServerNameLength = RemoteServerNameLength;
        memcpy(State->Buffer, Connection->RemoteServerName, State->ServerNameLength);

        uint32_t TempBufferLength = *BufferLength - RemoteServerNameLength;
        Status =
            QuicTlsReadTicket(
                Connection->Crypto.TLS,
                &TempBufferLength,
                State->Buffer + RemoteServerNameLength);
        *BufferLength = RequiredBufferLength;

        break;
    }

    case QUIC_PARAM_CONN_SEND_BUFFERING:

        if (*BufferLength < sizeof(uint8_t)) {
            *BufferLength = sizeof(uint8_t);
            Status = QUIC_STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (Buffer == NULL) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        *BufferLength = sizeof(uint8_t);
        *(uint8_t*)Buffer = Connection->State.UseSendBuffer;

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_SEND_PACING:

        if (*BufferLength < sizeof(uint8_t)) {
            *BufferLength = sizeof(uint8_t);
            Status = QUIC_STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (Buffer == NULL) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        *BufferLength = sizeof(uint8_t);
        *(uint8_t*)Buffer = Connection->State.UsePacing;

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_SHARE_UDP_BINDING:

        if (*BufferLength < sizeof(uint8_t)) {
            *BufferLength = sizeof(uint8_t);
            Status = QUIC_STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (Buffer == NULL) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        *BufferLength = sizeof(uint8_t);
        *(uint8_t*)Buffer = Connection->State.ShareBinding;

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_IDEAL_PROCESSOR:

        if (*BufferLength < sizeof(uint8_t)) {
            *BufferLength = sizeof(uint8_t);
            Status = QUIC_STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (Buffer == NULL) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        *BufferLength = sizeof(uint8_t);
        *(uint8_t*)Buffer = Connection->Worker->IdealProcessor;

        Status = QUIC_STATUS_SUCCESS;
        break;

    case QUIC_PARAM_CONN_MAX_STREAM_IDS:

        if (*BufferLength < sizeof(uint64_t) * NUMBER_OF_STREAM_TYPES) {
            *BufferLength = sizeof(uint64_t) * NUMBER_OF_STREAM_TYPES;
            Status = QUIC_STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (Buffer == NULL) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        *BufferLength = sizeof(uint64_t) * NUMBER_OF_STREAM_TYPES;
        QuicStreamSetGetMaxStreamIDs(&Connection->Streams, (uint64_t*)Buffer);

        Status = QUIC_STATUS_SUCCESS;
        break;

    default:
        Status = QUIC_STATUS_INVALID_PARAMETER;
        break;
    }

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnProcessApiOperation(
    _In_ PQUIC_CONNECTION Connection,
    _In_ PQUIC_API_CONTEXT ApiCtx
    )
{
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    switch (ApiCtx->Type) {

    case QUIC_API_TYPE_CONN_CLOSE:
        QuicConnCloseHandle(Connection);
        break;

    case QUIC_API_TYPE_CONN_SHUTDOWN:
        QuicConnShutdown(
            Connection,
            ApiCtx->CONN_SHUTDOWN.Flags,
            ApiCtx->CONN_SHUTDOWN.ErrorCode);
        break;

    case QUIC_API_TYPE_CONN_START:
        Status =
            QuicConnStart(
                Connection,
                ApiCtx->CONN_START.Family,
                ApiCtx->CONN_START.ServerName,
                ApiCtx->CONN_START.ServerPort);
        ApiCtx->CONN_START.ServerName = NULL;
        break;

    case QUIC_API_TYPE_STRM_CLOSE:
        QuicStreamClose(ApiCtx->STRM_CLOSE.Stream);
        break;

    case QUIC_API_TYPE_STRM_SHUTDOWN:
        QuicStreamShutdown(
            ApiCtx->STRM_SHUTDOWN.Stream,
            ApiCtx->STRM_SHUTDOWN.Flags,
            ApiCtx->STRM_SHUTDOWN.ErrorCode);
        break;

    case QUIC_API_TYPE_STRM_START:
        Status =
            QuicStreamStart(
                ApiCtx->STRM_START.Stream,
                ApiCtx->STRM_START.Flags);
        break;

    case QUIC_API_TYPE_STRM_SEND:
        QuicStreamSendFlush(
            ApiCtx->STRM_SEND.Stream);
        break;

    case QUIC_API_TYPE_STRM_RECV_COMPLETE:
        QuicStreamReceiveCompletePending(
            ApiCtx->STRM_RECV_COMPLETE.Stream,
            ApiCtx->STRM_RECV_COMPLETE.BufferLength);
        break;

    case QUIC_API_TYPE_STRM_RECV_SET_ENABLED:
        Status =
            QuicStreamRecvSetEnabledState(
                ApiCtx->STRM_RECV_SET_ENABLED.Stream,
                ApiCtx->STRM_RECV_SET_ENABLED.IsEnabled);
        break;

    case QUIC_API_TYPE_SET_PARAM:
        Status =
            QuicLibrarySetParam(
                ApiCtx->SET_PARAM.Handle,
                ApiCtx->SET_PARAM.Level,
                ApiCtx->SET_PARAM.Param,
                ApiCtx->SET_PARAM.BufferLength,
                ApiCtx->SET_PARAM.Buffer);
        break;

    case QUIC_API_TYPE_GET_PARAM:
        Status =
            QuicLibraryGetParam(
                ApiCtx->GET_PARAM.Handle,
                ApiCtx->GET_PARAM.Level,
                ApiCtx->GET_PARAM.Param,
                ApiCtx->GET_PARAM.BufferLength,
                ApiCtx->GET_PARAM.Buffer);
        break;

    default:
        QUIC_TEL_ASSERT(FALSE);
        Status = QUIC_STATUS_INVALID_PARAMETER;
        break;
    }

    if (ApiCtx->Status) {
        *ApiCtx->Status = Status;
    }
    if (ApiCtx->Completed) {
        QuicEventSet(*ApiCtx->Completed);
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicConnProcessExpiredTimer(
    _Inout_ PQUIC_CONNECTION Connection,
    _In_ QUIC_CONN_TIMER_TYPE Type
    )
{
    switch (Type) {
    case QUIC_CONN_TIMER_IDLE:
        QuicConnProcessIdleTimerOperation(Connection);
        break;
    case QUIC_CONN_TIMER_LOSS_DETECTION:
        QuicLossDetectionProcessTimerOperation(&Connection->LossDetection);
        break;
    case QUIC_CONN_TIMER_KEEP_ALIVE:
        QuicConnProcessKeepAliveOperation(Connection);
        break;
    case QUIC_CONN_TIMER_SHUTDOWN:
        QuicConnProcessShutdownTimerOperation(Connection);
        break;
    default:
        QUIC_FRE_ASSERT(FALSE);
        break;
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
BOOLEAN
QuicConnDrainOperations(
    _In_ PQUIC_CONNECTION Connection
    )
{
    PQUIC_OPERATION Oper;
    const uint32_t MaxOperationCount =
        Connection->Session == NULL ?
            MsQuicLib.Settings.MaxOperationsPerDrain :
            Connection->Session->Settings.MaxOperationsPerDrain;
    uint32_t OperationCount = 0;
    BOOLEAN HasMoreWorkToDo = TRUE;

    QUIC_PASSIVE_CODE();

    if (!Connection->State.Initialized) {
        //
        // TODO - Try to move this only after the connection is accepted by the
        // listener. But that's going to be pretty complicated.
        //
        QUIC_DBG_ASSERT(QuicConnIsServer(Connection));
        QUIC_STATUS Status;
        if (QUIC_FAILED(Status = QuicConnInitializeCrypto(Connection))) {
            QuicConnFatalError(Connection, Status, "Lazily initialize failure");
        } else {
            Connection->State.Initialized = TRUE;
            EventWriteQuicConnInitializeComplete(Connection);
        }
    }

    while (!Connection->State.HandleClosed &&
           !Connection->State.UpdateWorker &&
           OperationCount++ < MaxOperationCount) {

        Oper = QuicOperationDequeue(&Connection->OperQ);
        if (Oper == NULL) {
            HasMoreWorkToDo = FALSE;
            break;
        }

        QuicOperLog(Connection, Oper);

        BOOLEAN FreeOper = Oper->FreeAfterProcess;

        switch (Oper->Type) {

        case QUIC_OPER_TYPE_API_CALL:
            QUIC_DBG_ASSERT(Oper->API_CALL.Context != NULL);
            QuicConnProcessApiOperation(
                Connection,
                Oper->API_CALL.Context);
            break;

        case QUIC_OPER_TYPE_FLUSH_RECV:
            QuicConnFlushRecv(Connection);
            break;

        case QUIC_OPER_TYPE_UNREACHABLE:
            QuicConnProcessUdpUnreachable(
                Connection,
                &Oper->UNREACHABLE.RemoteAddress);
            break;

        case QUIC_OPER_TYPE_FLUSH_STREAM_RECV:
            QuicStreamRecvFlush(Oper->FLUSH_STREAM_RECEIVE.Stream);
            break;

        case QUIC_OPER_TYPE_FLUSH_SEND:
            if (QuicSendProcessFlushSendOperation(&Connection->Send, FALSE)) {
                //
                // Still have more packets to send. Put the operation back
                // on the queue.
                //
                FreeOper = FALSE;
                (void)QuicOperationEnqueue(&Connection->OperQ, Oper);
            }
            break;

        case QUIC_OPER_TYPE_TLS_COMPLETE:
            QuicCryptoProcessCompleteOperation(&Connection->Crypto);
            break;

        case QUIC_OPER_TYPE_TIMER_EXPIRED:
            QuicConnProcessExpiredTimer(Connection, Oper->TIMER_EXPIRED.Type);
            break;

        case QUIC_OPER_TYPE_TRACE_RUNDOWN:
            QuicConnTraceRundownOper(Connection);
            break;

        default:
            QUIC_FRE_ASSERT(FALSE);
            break;
        }

        QuicConnValidate(Connection);

        if (FreeOper) {
            QuicOperationFree(Connection->Worker, Oper);
        }

        Connection->Stats.Schedule.OperationCount++;
    }

    if (OperationCount >= MaxOperationCount &&
        (Connection->Send.SendFlags & QUIC_CONN_SEND_FLAG_ACK) &&
        !Connection->State.HandleClosed) {
        //
        // We can't process any more operations but still need to send an
        // immediate ACK. So as to not introduce additional queuing delay do one
        // immediate flush now.
        //
        QuicSendProcessFlushSendOperation(&Connection->Send, TRUE);
    }

    if (Connection->State.SendShutdownCompleteNotif && !Connection->State.HandleClosed) {
        Connection->State.SendShutdownCompleteNotif = FALSE;
        QuicConnOnShutdownComplete(Connection);
    }

    if (Connection->State.HandleClosed) {
        if (!Connection->State.Uninitialized) {
            QuicConnUninitialize(Connection);
        }
        HasMoreWorkToDo = FALSE;
    }

    QuicStreamSetDrainClosedStreams(&Connection->Streams);

    QuicConnValidate(Connection);

    return HasMoreWorkToDo;
}