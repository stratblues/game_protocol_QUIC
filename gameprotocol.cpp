﻿/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    Provides a very simple MsQuic API sample server and client application.

    The quicsample app implements a simple protocol (ALPN "sample") where the
    client connects to the server, opens a single bidirectional stream, sends
    some data and shuts down the stream in the send direction. On the server
    side all connections, streams and data are accepted. After the stream is
    shut down, the server then sends its own data and shuts down its send
    direction. The connection only shuts down when the 1 second idle timeout
    triggers.

    A certificate needs to be available for the server to function.

    On Windows, the following PowerShell command can be used to generate a self
    signed certificate with the correct settings. This works for both Schannel
    and OpenSSL TLS providers, assuming the KeyExportPolicy parameter is set to
    Exportable. The Thumbprint received from the command is then passed to this
    sample with -cert_hash:PASTE_THE_THUMBPRINT_HERE

    New-SelfSignedCertificate -DnsName $env:computername,localhost -FriendlyName MsQuic-Test -KeyUsageProperty Sign -KeyUsage DigitalSignature -CertStoreLocation cert:\CurrentUser\My -HashAlgorithm SHA256 -Provider "Microsoft Software Key Storage Provider" -KeyExportPolicy Exportable

    On Linux, the following command can be used to generate a self signed
    certificate that works with the OpenSSL TLS Provider. This can also be used
    for Windows OpenSSL, however we recommend the certificate store method above
    for ease of use. Currently key files with password protections are not
    supported. With these files, they can be passed to the sample with
    -cert_file:path/to/server.cert -key_file path/to/server.key

    openssl req  -nodes -new -x509  -keyout server.key -out server.cert

--*/

#define _CRT_SECURE_NO_WARNINGS 1

#define QUIC_API_ENABLE_PREVIEW_FEATURES 1

#ifdef _WIN32
//
// The conformant preprocessor along with the newest SDK throws this warning for
// a macro in C mode. As users might run into this exact bug, exclude this
// warning here. This is not an MsQuic bug but a Windows SDK bug.
//
#pragma warning(disable:5105)
#include <share.h>
#endif
#include "msquic.h"
#include <stdio.h>
#include <stdlib.h>
#include "protocol.hpp"
#include <string.h>

#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(P) (void)(P)
#endif


struct counter : GameObject
{
    uint32_t value = 0;
    //only one class id for now
    uint8_t getClassId() const { return 1; }
	// const since data is immutable
    void serialize(std::vector<uint8_t>& buf) const 
    {
        //build the packet into bytes
        auto start = buf.size();
        buf.resize(start + sizeof(value));
        std::memcpy(buf.data() + start, &value, sizeof(value));
    }
    // read in const ref to buf + offset 
    void deserialize(const std::vector<uint8_t>& buf, size_t& offset) 
    {
        std::memcpy(&value, buf.data() + offset, sizeof(value));
        offset += sizeof(value);
    }
};

struct stream_context
{
    counter        cnter;
    uint32_t       seq = 1;
};

void SendNextMessage(HQUIC Stream, stream_context* ctx);

//
// The (optional) registration configuration for the app. This sets a name for
// the app (used for persistent storage and for debugging). It also configures
// the execution profile, using the default "low latency" profile.
//
const QUIC_REGISTRATION_CONFIG RegConfig = { "quicsample", QUIC_EXECUTION_PROFILE_LOW_LATENCY };

//
// The protocol name used in the Application Layer Protocol Negotiation (ALPN).
//
const QUIC_BUFFER Alpn = { sizeof("sample") - 1, (uint8_t*)"sample" };

//
// The UDP port used by the server side of the protocol.
//
const uint16_t UdpPort = 4567;

//
// The default idle timeout period (1 second) used for the protocol.
//
const uint64_t IdleTimeoutMs = 1000;

//
// The length of buffer sent over the streams in the protocol.
//
const uint32_t SendBufferLength = 100;

//
// The QUIC API/function table returned from MsQuicOpen2. It contains all the
// functions called by the app to interact with MsQuic.
//
const QUIC_API_TABLE* MsQuic;

//
// The QUIC handle to the registration object. This is the top level API object
// that represents the execution context for all work done by MsQuic on behalf
// of the app.
//
HQUIC Registration;

//
// The QUIC handle to the configuration object. This object abstracts the
// connection configuration. This includes TLS configuration and any other
// QUIC layer settings.
//
HQUIC Configuration;


//
// The struct to be filled with TLS secrets
// for debugging packet captured with e.g. Wireshark.
//
QUIC_TLS_SECRETS ClientSecrets = {0};

//
// The name of the environment variable being
// used to get the path to the ssl key log file.
//
const char* SslKeyLogEnvVar = "SSLKEYLOGFILE";

void PrintUsage()
{
    printf(
        "\n"
        "quicsample runs a simple client or server.\n"
        "\n"
        "Usage:\n"
        "\n"
        "  quicsample.exe -client -unsecure -target:{IPAddress|Hostname} [-ticket:<ticket>]\n"
#ifdef QUIC_API_ENABLE_PREVIEW_FEATURES
        "  quicsample.exe -multiclient -count:<N> -unsecure -target:{IPAddress|Hostname}\n"
#endif
        "  quicsample.exe -server -cert_hash:<...>\n"
        "  quicsample.exe -server -cert_file:<...> -key_file:<...> [-password:<...>]\n"
        );
}

//
// Helper functions to look up a command line arguments.
//
BOOLEAN
GetFlag(
    _In_ int argc,
    _In_reads_(argc) _Null_terminated_ char* argv[],
    _In_z_ const char* name
    )
{
    const size_t nameLen = strlen(name);
    for (int i = 0; i < argc; i++) {
        if (_strnicmp(argv[i] + 1, name, nameLen) == 0
            && strlen(argv[i]) == nameLen + 1) {
            return TRUE;
        }
    }
    return FALSE;
}

_Ret_maybenull_ _Null_terminated_ const char*
GetValue(
    _In_ int argc,
    _In_reads_(argc) _Null_terminated_ char* argv[],
    _In_z_ const char* name
    )
{
    const size_t nameLen = strlen(name);
    for (int i = 0; i < argc; i++) {
        if (_strnicmp(argv[i] + 1, name, nameLen) == 0
            && strlen(argv[i]) > 1 + nameLen + 1
            && *(argv[i] + 1 + nameLen) == ':') {
            return argv[i] + 1 + nameLen + 1;
        }
    }
    return NULL;
}

//
// Helper function to convert a hex character to its decimal value.
//
uint8_t
DecodeHexChar(
    _In_ char c
    )
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    return 0;
}

//
// Helper function to convert a string of hex characters to a byte buffer.
//
uint32_t
DecodeHexBuffer(
    _In_z_ const char* HexBuffer,
    _In_ uint32_t OutBufferLen,
    _Out_writes_to_(OutBufferLen, return)
        uint8_t* OutBuffer
    )
{
    uint32_t HexBufferLen = (uint32_t)strlen(HexBuffer) / 2;
    if (HexBufferLen > OutBufferLen) {
        return 0;
    }

    for (uint32_t i = 0; i < HexBufferLen; i++) {
        OutBuffer[i] =
            (DecodeHexChar(HexBuffer[i * 2]) << 4) |
            DecodeHexChar(HexBuffer[i * 2 + 1]);
    }

    return HexBufferLen;
}

void
EncodeHexBuffer(
    _In_reads_(BufferLen) uint8_t* Buffer,
    _In_ uint8_t BufferLen,
    _Out_writes_bytes_(2*BufferLen) char* HexString
    )
{
    #define HEX_TO_CHAR(x) ((x) > 9 ? ('a' + ((x) - 10)) : '0' + (x))
    for (uint8_t i = 0; i < BufferLen; i++) {
        HexString[i*2]     = HEX_TO_CHAR(Buffer[i] >> 4);
        HexString[i*2 + 1] = HEX_TO_CHAR(Buffer[i] & 0xf);
    }
}

void
WriteSslKeyLogFile(
    _In_z_ const char* FileName,
    _In_ QUIC_TLS_SECRETS* TlsSecrets
    )
{
    printf("Writing SSLKEYLOGFILE at %s\n", FileName);
    FILE* File = NULL;
#ifdef _WIN32
    File = _fsopen(FileName, "ab", _SH_DENYNO);
#else
    File = fopen(FileName, "ab");
#endif

    if (File == NULL) {
        printf("Failed to open sslkeylogfile %s\n", FileName);
        return;
    }
    if (fseek(File, 0, SEEK_END) == 0 && ftell(File) == 0) {
        fprintf(File, "# TLS 1.3 secrets log file, generated by msquic\n");
    }

    char ClientRandomBuffer[(2 * sizeof(((QUIC_TLS_SECRETS*)0)->ClientRandom)) + 1] = {0};

    char TempHexBuffer[(2 * QUIC_TLS_SECRETS_MAX_SECRET_LEN) + 1] = {0};
    if (TlsSecrets->IsSet.ClientRandom) {
        EncodeHexBuffer(
            TlsSecrets->ClientRandom,
            (uint8_t)sizeof(TlsSecrets->ClientRandom),
            ClientRandomBuffer);
    }

    if (TlsSecrets->IsSet.ClientEarlyTrafficSecret) {
        EncodeHexBuffer(
            TlsSecrets->ClientEarlyTrafficSecret,
            TlsSecrets->SecretLength,
            TempHexBuffer);
        fprintf(
            File,
            "CLIENT_EARLY_TRAFFIC_SECRET %s %s\n",
            ClientRandomBuffer,
            TempHexBuffer);
    }

    if (TlsSecrets->IsSet.ClientHandshakeTrafficSecret) {
        EncodeHexBuffer(
            TlsSecrets->ClientHandshakeTrafficSecret,
            TlsSecrets->SecretLength,
            TempHexBuffer);
        fprintf(
            File,
            "CLIENT_HANDSHAKE_TRAFFIC_SECRET %s %s\n",
            ClientRandomBuffer,
            TempHexBuffer);
    }

    if (TlsSecrets->IsSet.ServerHandshakeTrafficSecret) {
        EncodeHexBuffer(
            TlsSecrets->ServerHandshakeTrafficSecret,
            TlsSecrets->SecretLength,
            TempHexBuffer);
        fprintf(
            File,
            "SERVER_HANDSHAKE_TRAFFIC_SECRET %s %s\n",
            ClientRandomBuffer,
            TempHexBuffer);
    }

    if (TlsSecrets->IsSet.ClientTrafficSecret0) {
        EncodeHexBuffer(
            TlsSecrets->ClientTrafficSecret0,
            TlsSecrets->SecretLength,
            TempHexBuffer);
        fprintf(
            File,
            "CLIENT_TRAFFIC_SECRET_0 %s %s\n",
            ClientRandomBuffer,
            TempHexBuffer);
    }

    if (TlsSecrets->IsSet.ServerTrafficSecret0) {
        EncodeHexBuffer(
            TlsSecrets->ServerTrafficSecret0,
            TlsSecrets->SecretLength,
            TempHexBuffer);
        fprintf(
            File,
            "SERVER_TRAFFIC_SECRET_0 %s %s\n",
            ClientRandomBuffer,
            TempHexBuffer);
    }

    fflush(File);
    fclose(File);
}

//
// Allocates and sends some data over a QUIC stream.
//
void
ServerSend(
    _In_ HQUIC Stream
    )
{
    //
    // Allocates and builds the buffer to send over the stream.
    //
    void* SendBufferRaw = malloc(sizeof(QUIC_BUFFER) + SendBufferLength + sizeof(PDU));
    if (SendBufferRaw == NULL) {
        printf("SendBuffer allocation failed!\n");
        MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        return;
    }
    QUIC_BUFFER* SendBuffer = (QUIC_BUFFER*)SendBufferRaw;
    SendBuffer->Buffer = (uint8_t*)SendBufferRaw + sizeof(QUIC_BUFFER);
    SendBuffer->Length = SendBufferLength + sizeof(PDU);

    printf("[strm][%p] Sending data...\n", Stream);

    //
    // Sends the buffer over the stream. Note the FIN flag is passed along with
    // the buffer. This indicates this is the last buffer on the stream and the
    // the stream is shut down (in the send direction) immediately after.
    //
    QUIC_STATUS Status;
    if (QUIC_FAILED(Status = MsQuic->StreamSend(Stream, SendBuffer, 1, QUIC_SEND_FLAG_FIN, SendBuffer))) {
        printf("StreamSend failed, 0x%x!\n", Status);
        free(SendBufferRaw);
        MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
    }
}

//
// The server's callback for stream events from MsQuic.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_STREAM_CALLBACK)
QUIC_STATUS
QUIC_API
ServerStreamCallback(
    _In_ HQUIC Stream,
    _In_opt_ void* Context,
    _Inout_ QUIC_STREAM_EVENT* Event
    )
{
    UNREFERENCED_PARAMETER(Context);
    auto* ctx = static_cast<stream_context*>(Context);

    switch (Event->Type)
    {
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        //
        // A previous StreamSend call has completed, and the context is being
        // returned back to the app.
        //
        //free(Event->SEND_COMPLETE.ClientContext);
       
        delete[] reinterpret_cast<uint8_t*>(Event->SEND_COMPLETE.ClientContext);
        printf("[strm][%p] Data sent\n", Stream);
        break;
    case QUIC_STREAM_EVENT_RECEIVE: {
        //
        // Data was received from the peer on the stream.
        //
        const auto& in = Event->RECEIVE.Buffers[0];

        PDU hdr{}; 
        memcpy(&hdr, in.Buffer, sizeof(hdr));
        if (hdr.msgType != uint8_t(MsgType::STATE_UPDATE))
            break;

    
        memcpy(&ctx->cnter.value, in.Buffer + sizeof(hdr),sizeof(ctx->cnter.value));
        printf("[srv][%p] got %u\n", Stream, ctx->cnter.value);

        if (ctx->cnter.value > 10)
        {
            MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL,0);
            break;
        }
           

       
        //ctx->cnter.value++;
        hdr.sequenceNumber = ctx->cnter.value;

        const uint32_t payLen = sizeof(hdr) + sizeof(ctx->cnter.value);
        uint8_t* raw = new uint8_t[sizeof(QUIC_BUFFER) + payLen];
        auto* qb = reinterpret_cast<QUIC_BUFFER*>(raw);
        qb->Buffer = raw + sizeof(QUIC_BUFFER);
        qb->Length = payLen;

        memcpy(qb->Buffer, &hdr, sizeof(hdr));
        memcpy(qb->Buffer + sizeof(hdr), &ctx->cnter.value, sizeof(ctx->cnter.value));

        if (QUIC_FAILED(MsQuic->StreamSend(Stream, qb, 1,
            QUIC_SEND_FLAG_NONE, qb)))
        {
            delete[] raw;                     
        }
        break;
    }
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        //
        // The peer gracefully shut down its send direction of the stream.
        //
        printf("[strm][%p] Peer shut down\n", Stream);
        MsQuic->StreamShutdown(Stream,
            QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL,
            0);
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        //
        // The peer aborted its send direction of the stream.
        //
        printf("[strm][%p] Peer aborted\n", Stream);
        MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        //
        // Both directions of the stream have been shut down and MsQuic is done
        // with the stream. It can now be safely cleaned up.
        //
        delete ctx;
        printf("[strm][%p] All done\n", Stream);
        MsQuic->StreamClose(Stream);
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

//
// The server's callback for connection events from MsQuic.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_CONNECTION_CALLBACK)
QUIC_STATUS
QUIC_API
ServerConnectionCallback(
    _In_ HQUIC Connection,
    _In_opt_ void* Context,
    _Inout_ QUIC_CONNECTION_EVENT* Event
    )
{
    UNREFERENCED_PARAMETER(Context);
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        //
        // The handshake has completed for the connection.
        //
        printf("[conn][%p] Connected\n", Connection);
        MsQuic->ConnectionSendResumptionTicket(Connection, QUIC_SEND_RESUMPTION_FLAG_NONE, 0, NULL);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        //
        // The connection has been shut down by the transport. Generally, this
        // is the expected way for the connection to shut down with this
        // protocol, since we let idle timeout kill the connection.
        //
        if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status == QUIC_STATUS_CONNECTION_IDLE) {
            printf("[conn][%p] Successfully shut down on idle.\n", Connection);
        } else {
            printf("[conn][%p] Shut down by transport, 0x%x\n", Connection, Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        //
        // The connection was explicitly shut down by the peer.
        //
        printf("[conn][%p] Shut down by peer, 0x%llu\n", Connection, (unsigned long long)Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        //
        // The connection has completed the shutdown process and is ready to be
        // safely cleaned up.
        //
        printf("[conn][%p] All done\n", Connection);
        MsQuic->ConnectionClose(Connection);
        break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        //
        // The peer has started/created a new stream. The app MUST set the
        // callback handler before returning.
        //
        auto* ctx = new stream_context();
        printf("[strm][%p] Peer started\n", Event->PEER_STREAM_STARTED.Stream);
        MsQuic->SetCallbackHandler(Event->PEER_STREAM_STARTED.Stream, (void*)ServerStreamCallback, ctx);
        break;
    }
    case QUIC_CONNECTION_EVENT_RESUMED:
        //
        // The connection succeeded in doing a TLS resumption of a previous
        // connection's session.
        //
        printf("[conn][%p] Connection resumed!\n", Connection);
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

//
// The server's callback for listener events from MsQuic.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
_Function_class_(QUIC_LISTENER_CALLBACK)
QUIC_STATUS
QUIC_API
ServerListenerCallback(
    _In_ HQUIC Listener,
    _In_opt_ void* Context,
    _Inout_ QUIC_LISTENER_EVENT* Event
    )
{
    UNREFERENCED_PARAMETER(Listener);
    UNREFERENCED_PARAMETER(Context);
    QUIC_STATUS Status = QUIC_STATUS_NOT_SUPPORTED;
    switch (Event->Type) {
    case QUIC_LISTENER_EVENT_NEW_CONNECTION:
        //
        // A new connection is being attempted by a client. For the handshake to
        // proceed, the server must provide a configuration for QUIC to use. The
        // app MUST set the callback handler before returning.
        //
        MsQuic->SetCallbackHandler(Event->NEW_CONNECTION.Connection, (void*)ServerConnectionCallback, NULL);
        Status = MsQuic->ConnectionSetConfiguration(Event->NEW_CONNECTION.Connection, Configuration);
        break;
    default:
        break;
    }
    return Status;
}

typedef struct QUIC_CREDENTIAL_CONFIG_HELPER {
    QUIC_CREDENTIAL_CONFIG CredConfig;
    union {
        QUIC_CERTIFICATE_HASH CertHash;
        QUIC_CERTIFICATE_HASH_STORE CertHashStore;
        QUIC_CERTIFICATE_FILE CertFile;
        QUIC_CERTIFICATE_FILE_PROTECTED CertFileProtected;
    };
} QUIC_CREDENTIAL_CONFIG_HELPER;

//
// Helper function to load a server configuration. Uses the command line
// arguments to load the credential part of the configuration.
//
BOOLEAN
ServerLoadConfiguration(
    _In_ int argc,
    _In_reads_(argc) _Null_terminated_ char* argv[]
    )
{
    QUIC_SETTINGS Settings = {0};
    //
    // Configures the server's idle timeout.
    //
    Settings.IdleTimeoutMs = IdleTimeoutMs;
    Settings.IsSet.IdleTimeoutMs = TRUE;
    //
    // Configures the server's resumption level to allow for resumption and
    // 0-RTT.
    //
    Settings.ServerResumptionLevel = QUIC_SERVER_RESUME_AND_ZERORTT;
    Settings.IsSet.ServerResumptionLevel = TRUE;
    //
    // Configures the server's settings to allow for the peer to open a single
    // bidirectional stream. By default connections are not configured to allow
    // any streams from the peer.
    //
    Settings.PeerBidiStreamCount = 1;
    Settings.IsSet.PeerBidiStreamCount = TRUE;

    QUIC_CREDENTIAL_CONFIG_HELPER Config;
    memset(&Config, 0, sizeof(Config));
    Config.CredConfig.Flags = QUIC_CREDENTIAL_FLAG_NONE;

    const char* Cert;
    const char* KeyFile;
    if ((Cert = GetValue(argc, argv, "cert_hash")) != NULL) {
        //
        // Load the server's certificate from the default certificate store,
        // using the provided certificate hash.
        //
        uint32_t CertHashLen =
            DecodeHexBuffer(
                Cert,
                sizeof(Config.CertHash.ShaHash),
                Config.CertHash.ShaHash);
        if (CertHashLen != sizeof(Config.CertHash.ShaHash)) {
            return FALSE;
        }
        Config.CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH;
        Config.CredConfig.CertificateHash = &Config.CertHash;

    } else if ((Cert = GetValue(argc, argv, "cert_file")) != NULL &&
               (KeyFile = GetValue(argc, argv, "key_file")) != NULL) {
        //
        // Loads the server's certificate from the file.
        //
        const char* Password = GetValue(argc, argv, "password");
        if (Password != NULL) {
            Config.CertFileProtected.CertificateFile = (char*)Cert;
            Config.CertFileProtected.PrivateKeyFile = (char*)KeyFile;
            Config.CertFileProtected.PrivateKeyPassword = (char*)Password;
            Config.CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE_PROTECTED;
            Config.CredConfig.CertificateFileProtected = &Config.CertFileProtected;
        } else {
            Config.CertFile.CertificateFile = (char*)Cert;
            Config.CertFile.PrivateKeyFile = (char*)KeyFile;
            Config.CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
            Config.CredConfig.CertificateFile = &Config.CertFile;
        }

    } else {
        printf("Must specify ['-cert_hash'] or ['cert_file' and 'key_file' (and optionally 'password')]!\n");
        return FALSE;
    }

    //
    // Allocate/initialize the configuration object, with the configured ALPN
    // and settings.
    //
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    if (QUIC_FAILED(Status = MsQuic->ConfigurationOpen(Registration, &Alpn, 1, &Settings, sizeof(Settings), NULL, &Configuration))) {
        printf("ConfigurationOpen failed, 0x%x!\n", Status);
        return FALSE;
    }

    //
    // Loads the TLS credential part of the configuration.
    //
    if (QUIC_FAILED(Status = MsQuic->ConfigurationLoadCredential(Configuration, &Config.CredConfig))) {
        printf("ConfigurationLoadCredential failed, 0x%x!\n", Status);
        return FALSE;
    }

    return TRUE;
}

//
// Runs the server side of the protocol.
//
void
RunServer(
    _In_ int argc,
    _In_reads_(argc) _Null_terminated_ char* argv[]
    )
{
    QUIC_STATUS Status;
    HQUIC Listener = NULL;

    //
    // Configures the address used for the listener to listen on all IP
    // addresses and the given UDP port.
    //
    QUIC_ADDR Address = {0};
    QuicAddrSetFamily(&Address, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&Address, UdpPort);

    //
    // Load the server configuration based on the command line.
    //
    if (!ServerLoadConfiguration(argc, argv)) {
        return;
    }

    //
    // Create/allocate a new listener object.
    //
    if (QUIC_FAILED(Status = MsQuic->ListenerOpen(Registration, ServerListenerCallback, NULL, &Listener))) {
        printf("ListenerOpen failed, 0x%x!\n", Status);
        goto Error;
    }

    //
    // Starts listening for incoming connections.
    //
    if (QUIC_FAILED(Status = MsQuic->ListenerStart(Listener, &Alpn, 1, &Address))) {
        printf("ListenerStart failed, 0x%x!\n", Status);
        goto Error;
    }

    //
    // Continue listening for connections until the Enter key is pressed.
    //
    printf("Press Enter to exit.\n\n");
    (void)getchar();

Error:

    if (Listener != NULL) {
        MsQuic->ListenerClose(Listener);
    }
}

//
// The clients's callback for stream events from MsQuic.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_STREAM_CALLBACK)
QUIC_STATUS
QUIC_API
ClientStreamCallback(
    _In_ HQUIC Stream,
    _In_opt_ void* Context,
    _Inout_ QUIC_STREAM_EVENT* Event
    )
{
    auto* ctx = static_cast<stream_context*>(Context);
    UNREFERENCED_PARAMETER(Context);
    switch (Event->Type) {
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        //
        // A previous StreamSend call has completed, and the context is being
        // returned back to the app.
        //
        //free(Event->SEND_COMPLETE.ClientContext);
        delete[] reinterpret_cast<uint8_t*>(Event->SEND_COMPLETE.ClientContext);
        printf("[strm][%p] Data sent\n", Stream);
        break;
    case QUIC_STREAM_EVENT_RECEIVE: {
        //
        // Data was received from the peer on the stream.
        //
        //auto* ctx = static_cast<stream_context*>(Context);
        auto  buf = Event->RECEIVE.Buffers[0];

       
        PDU hdr;
        std::memcpy(&hdr, buf.Buffer, sizeof(hdr));
        size_t off = sizeof(hdr);
        std::vector<uint8_t> data(buf.Buffer, buf.Buffer + buf.Length);
        ctx->cnter.deserialize(data, off);
        printf("[cli] got %u\n", ctx->cnter.value);

        if (ctx->cnter.value < 10)
        {
            SendNextMessage(Stream, ctx);
        }
        else
        {
            MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        }
        break;
        
    }
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        //
        // The peer gracefully shut down its send direction of the stream.
        //
        printf("[strm][%p] Peer aborted\n", Stream);
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        //
        // The peer aborted its send direction of the stream.
        //
        printf("[strm][%p] Peer shut down\n", Stream);
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        //
        // Both directions of the stream have been shut down and MsQuic is done
        // with the stream. It can now be safely cleaned up.
        //
        delete ctx;
        printf("[strm][%p] All done\n", Stream);
        if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
            MsQuic->StreamClose(Stream);
        }
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}
void
SendNextMessage(HQUIC Stream, stream_context* ctx)
{
	
    if (ctx->cnter.value > 10)
    {
        MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        return;
    }
    ctx->cnter.value++;
    PDU hdr{};
    hdr.packetLength = sizeof(hdr) + sizeof(ctx->cnter.value);
    hdr.msgType = uint8_t(MsgType::STATE_UPDATE);
    hdr.flags = 0;
    hdr.sequenceNumber = ctx->cnter.value;
    hdr.classId = ctx->cnter.getClassId();

	// allocate on heap 
    const uint32_t payLen = sizeof(hdr) + sizeof(ctx->cnter.value);
	// allocate raw buffer for QUIC_BUFFER + payload
    uint8_t* raw = new uint8_t[sizeof(QUIC_BUFFER) + payLen];
	// fill the QUIC_BUFFER structure out of the raw buffer
    auto* qb = reinterpret_cast<QUIC_BUFFER*>(raw);
	// set the buffer pointer to the payload part of the raw buffer
    qb->Buffer = raw + sizeof(QUIC_BUFFER);
	// set the length of the payload
    qb->Length = payLen;

	// copy the header and the counter value into the buffer
    memcpy(qb->Buffer, &hdr, sizeof(hdr));
	// copy the counter value into the buffer after the header
    memcpy(qb->Buffer + sizeof(hdr), &ctx->cnter.value, sizeof(ctx->cnter.value));

    QUIC_SEND_FLAGS flags = (ctx->cnter.value == 10) ? QUIC_SEND_FLAG_FIN : QUIC_SEND_FLAG_NONE;

    if (QUIC_FAILED(MsQuic->StreamSend(Stream, qb, 1, flags, qb)))
    {
        // cleanup 
        delete[] raw;                        
        return;
    }

    printf("[cli][%p] Sent %u\n", Stream, ctx->cnter.value);
};

void
ClientSend(
    _In_ HQUIC Connection
)
{
    QUIC_STATUS Status;
    HQUIC Stream = nullptr;
    auto* ctx = new stream_context();
    if (QUIC_FAILED(Status = MsQuic->StreamOpen(Connection, QUIC_STREAM_OPEN_FLAG_NONE, ClientStreamCallback,ctx,&Stream)))
    {
        printf("StreamOpen failed, 0x%x!\n", Status);
        delete ctx;
        return;
    }

    
    printf("[cli][%p] Starting stream...\n", Stream);
    if (QUIC_FAILED(Status = MsQuic->StreamStart(Stream, QUIC_STREAM_START_FLAG_NONE)))
    {
        printf("StreamStart failed, 0x%x!\n", Status);
        MsQuic->StreamClose(Stream);
        delete ctx;
        return;
    }

    
    SendNextMessage(Stream, ctx);

 
}




//
// The clients's callback for connection events from MsQuic.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_CONNECTION_CALLBACK)
QUIC_STATUS
QUIC_API
ClientConnectionCallback(
    _In_ HQUIC Connection,
    _In_opt_ void* Context,
    _Inout_ QUIC_CONNECTION_EVENT* Event
    )
{
    UNREFERENCED_PARAMETER(Context);

    if (Event->Type == QUIC_CONNECTION_EVENT_CONNECTED) {
        const char* SslKeyLogFile = getenv(SslKeyLogEnvVar);
        if (SslKeyLogFile != NULL) {
            WriteSslKeyLogFile(SslKeyLogFile, &ClientSecrets);
        }
    }

    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        //
        // The handshake has completed for the connection.
        //
        printf("[conn][%p] Connected\n", Connection);
        ClientSend(Connection);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        //
        // The connection has been shut down by the transport. Generally, this
        // is the expected way for the connection to shut down with this
        // protocol, since we let idle timeout kill the connection.
        //
        if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status == QUIC_STATUS_CONNECTION_IDLE) {
            printf("[conn][%p] Successfully shut down on idle.\n", Connection);
        } else {
            printf("[conn][%p] Shut down by transport, 0x%x\n", Connection, Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        //
        // The connection was explicitly shut down by the peer.
        //
        printf("[conn][%p] Shut down by peer, 0x%llu\n", Connection, (unsigned long long)Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        //
        // The connection has completed the shutdown process and is ready to be
        // safely cleaned up.
        //
        printf("[conn][%p] All done\n", Connection);
        if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
            MsQuic->ConnectionClose(Connection);
        }
        break;
    case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED:
        //
        // A resumption ticket (also called New Session Ticket or NST) was
        // received from the server.
        //
        printf("[conn][%p] Resumption ticket received (%u bytes):\n", Connection, Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength);
        for (uint32_t i = 0; i < Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength; i++) {
            printf("%.2X", (uint8_t)Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicket[i]);
        }
        printf("\n");
        break;
    case QUIC_CONNECTION_EVENT_IDEAL_PROCESSOR_CHANGED:
        printf(
            "[conn][%p] Ideal Processor is: %u, Partition Index %u\n",
            Connection,
            Event->IDEAL_PROCESSOR_CHANGED.IdealProcessor,
            Event->IDEAL_PROCESSOR_CHANGED.PartitionIndex);
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

//
// Helper function to load a client configuration.
//
BOOLEAN
ClientLoadConfiguration(
    BOOLEAN Unsecure
    )
{
    QUIC_SETTINGS Settings = {0};
    //
    // Configures the client's idle timeout.
    //
    Settings.IdleTimeoutMs = IdleTimeoutMs;
    Settings.IsSet.IdleTimeoutMs = TRUE;

    //
    // Configures a default client configuration, optionally disabling
    // server certificate validation.
    //
    QUIC_CREDENTIAL_CONFIG CredConfig;
    memset(&CredConfig, 0, sizeof(CredConfig));
    CredConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
    CredConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
    if (Unsecure) {
        CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    }

    //
    // Allocate/initialize the configuration object, with the configured ALPN
    // and settings.
    //
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    if (QUIC_FAILED(Status = MsQuic->ConfigurationOpen(Registration, &Alpn, 1, &Settings, sizeof(Settings), NULL, &Configuration))) {
        printf("ConfigurationOpen failed, 0x%x!\n", Status);
        return FALSE;
    }

    //
    // Loads the TLS credential part of the configuration. This is required even
    // on client side, to indicate if a certificate is required or not.
    //
    if (QUIC_FAILED(Status = MsQuic->ConfigurationLoadCredential(Configuration, &CredConfig))) {
        printf("ConfigurationLoadCredential failed, 0x%x!\n", Status);
        return FALSE;
    }

    return TRUE;
}

//
// Runs the client side of the protocol.
//
void
RunClient(
    _In_ int argc,
    _In_reads_(argc) _Null_terminated_ char* argv[]
    )
{
    //
    // Load the client configuration based on the "unsecure" command line option.
    //
    if (!ClientLoadConfiguration(GetFlag(argc, argv, "unsecure"))) {
        return;
    }

    QUIC_STATUS Status;
    const char* ResumptionTicketString = NULL;
    const char* SslKeyLogFile = getenv(SslKeyLogEnvVar);
    HQUIC Connection = NULL;

    //
    // Allocate a new connection object.
    //
    if (QUIC_FAILED(Status = MsQuic->ConnectionOpen(Registration, ClientConnectionCallback, NULL, &Connection))) {
        printf("ConnectionOpen failed, 0x%x!\n", Status);
        goto Error;
    }

    if ((ResumptionTicketString = GetValue(argc, argv, "ticket")) != NULL) {
        //
        // If provided at the command line, set the resumption ticket that can
        // be used to resume a previous session.
        //
        uint8_t ResumptionTicket[10240];
        uint16_t TicketLength = (uint16_t)DecodeHexBuffer(ResumptionTicketString, sizeof(ResumptionTicket), ResumptionTicket);
        if (QUIC_FAILED(Status = MsQuic->SetParam(Connection, QUIC_PARAM_CONN_RESUMPTION_TICKET, TicketLength, ResumptionTicket))) {
            printf("SetParam(QUIC_PARAM_CONN_RESUMPTION_TICKET) failed, 0x%x!\n", Status);
            goto Error;
        }
    }

    if (SslKeyLogFile != NULL) {
        if (QUIC_FAILED(Status = MsQuic->SetParam(Connection, QUIC_PARAM_CONN_TLS_SECRETS, sizeof(ClientSecrets), &ClientSecrets))) {
            printf("SetParam(QUIC_PARAM_CONN_TLS_SECRETS) failed, 0x%x!\n", Status);
            goto Error;
        }
    }

    //
    // Get the target / server name or IP from the command line.
    //
    const char* Target;
    if ((Target = GetValue(argc, argv, "target")) == NULL) {
        printf("Must specify '-target' argument!\n");
        Status = QUIC_STATUS_INVALID_PARAMETER;
        goto Error;
    }

    printf("[conn][%p] Connecting...\n", Connection);

    //
    // Start the connection to the server.
    //
    if (QUIC_FAILED(Status = MsQuic->ConnectionStart(Connection, Configuration, QUIC_ADDRESS_FAMILY_UNSPEC, Target, UdpPort))) {
        printf("ConnectionStart failed, 0x%x!\n", Status);
        goto Error;
    }

Error:

    if (QUIC_FAILED(Status) && Connection != NULL) {
        MsQuic->ConnectionClose(Connection);
    }
}




//
// Runs the multi client side of the protocol.
//
#ifdef QUIC_API_ENABLE_PREVIEW_FEATURES

void
RunMultiClient(
    _In_ int argc,
    _In_reads_(argc) _Null_terminated_ char* argv[]
    )
{
    //
    // Load the client configuration based on the "unsecure" command line option.
    //
    if (!ClientLoadConfiguration(GetFlag(argc, argv, "unsecure"))) {
        return;
    }
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    uint32_t NumberOfConnections = 0;
    HQUIC* Connections = NULL;
    QUIC_CONNECTION_POOL_CONFIG PoolConfig = { 0 };

    //
    // Get the target / server name or IP from the command line.
    //
    const char* Target;
    if ((Target = GetValue(argc, argv, "target")) == NULL) {
        printf("Must specify '-target' argument!\n");
        Status = QUIC_STATUS_INVALID_PARAMETER;
        goto Error;
    }

    const char* NumberOfConnectionsString;
    if ((NumberOfConnectionsString = GetValue(argc, argv, "count")) == NULL) {
        printf("Must specify '-count' argument with -multiclient!\n");
        Status = QUIC_STATUS_INVALID_PARAMETER;
        goto Error;
    }

    NumberOfConnections = strtoul(NumberOfConnectionsString, NULL, 10);
    if (NumberOfConnections > UINT16_MAX) {
        printf("'-count' parameter %s > 65535!\n", NumberOfConnectionsString);
        Status = QUIC_STATUS_INVALID_PARAMETER;
        goto Error;
    }

    Connections = (HQUIC*)malloc(sizeof(HQUIC) * NumberOfConnections);

    
    PoolConfig.Registration = Registration;
    PoolConfig.Configuration = Configuration;
    PoolConfig.Handler = ClientConnectionCallback;
    PoolConfig.Family = QUIC_ADDRESS_FAMILY_UNSPEC;
    PoolConfig.ServerName = Target;
    PoolConfig.ServerPort = UdpPort;
    PoolConfig.NumberOfConnections = (uint16_t)NumberOfConnections;

    printf("Connection Pool Connecting...\n");

    //
    // Start the connections to the server.
    //
    if (QUIC_FAILED(Status = MsQuic->ConnectionPoolCreate(&PoolConfig, Connections))) {
        printf("ConnectionPoolCreate failed, 0x%x!\n", Status);
        goto Error;
    }

Error:

    if (Connections != NULL) {
        if (QUIC_FAILED(Status)) {
            for (uint16_t i = 0; i < NumberOfConnections; i++) {
                HQUIC Connection = Connections[i];
                if (Connection != NULL) {
                    MsQuic->ConnectionClose(Connection);
                }
            }
        }
        //
        // This is safe to free here because the connections clean themselves
        // up at shutdown.
        //
        free(Connections);
    }
}

#endif // QUIC_API_ENABLE_PREVIEW_FEATURES

int
QUIC_MAIN_EXPORT
main(
    _In_ int argc,
    _In_reads_(argc) _Null_terminated_ char* argv[]
    )
{
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;

    //
    // Open a handle to the library and get the API function table.
    //
    if (QUIC_FAILED(Status = MsQuicOpen2(&MsQuic))) {
        printf("MsQuicOpen2 failed, 0x%x!\n", Status);
        goto Error;
    }

    //
    // Create a registration for the app's connections.
    //
    if (QUIC_FAILED(Status = MsQuic->RegistrationOpen(&RegConfig, &Registration))) {
        printf("RegistrationOpen failed, 0x%x!\n", Status);
        goto Error;
    }

    if (GetFlag(argc, argv, "help") || GetFlag(argc, argv, "?")) {
        PrintUsage();
    } else if (GetFlag(argc, argv, "client")) {
        RunClient(argc, argv);
    } else if (GetFlag(argc, argv, "multiclient")) {
#ifdef QUIC_API_ENABLE_PREVIEW_FEATURES
        RunMultiClient(argc, argv);
#else
        printf("Error: Multiclient requires the sample to be built with QUIC_API_ENABLE_PREVIEW_FEATURES.\n\n");
        Status = QUIC_STATUS_NOT_SUPPORTED;
        PrintUsage();
#endif
    } else if (GetFlag(argc, argv, "server")) {
        RunServer(argc, argv);
    } else {
        PrintUsage();
    }

Error:

    if (MsQuic != NULL) {
        if (Configuration != NULL) {
            MsQuic->ConfigurationClose(Configuration);
        }
        if (Registration != NULL) {
            //
            // This will block until all outstanding child objects have been
            // closed.
            //
            MsQuic->RegistrationClose(Registration);
        }
        MsQuicClose(MsQuic);
    }

    return (int)Status;
}


