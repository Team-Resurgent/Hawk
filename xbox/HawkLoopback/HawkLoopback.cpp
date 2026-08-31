//-----------------------------------------------------------------------------
// File: HawkLoopback.cpp
//
// Desc: End-to-end test app for the Hawk (Xbox Communicator) emulator.
//       Listens to the communicator microphone on every controller port and
//       plays the audio back BOTH ways:
//         1. to the communicator's own headphone (device loopback), and
//         2. to the TV/receiver speakers through a DirectSound stream,
//       so a Hawk emulator sending its test tone is audible in the room and
//       measurable on the device side at the same time.
//
//       Headless on purpose: all status goes to stdout (the RXDK "Xbox Title"
//       output / debug monitor), including a level + zero-crossing frequency
//       estimate of the incoming mic audio. The emulator's tone is a C5-E5-G5
//       arpeggio (523/659/784 Hz), so the printed frequency should step
//       through roughly those values once per second.
//
// Deploy: build with the RXDK engine (rxdk.project.json) and deploy/launch
//       over xbdm as usual (VS Code F5, VS20XX, or Rxdk.Cli + xbox-launch).
//-----------------------------------------------------------------------------
#include <xtl.h>
#include <xvoice.h>
#include <stdio.h>
#include <string.h>

// Voice processing: mono 16-bit, 4 packets of 40 ms each. 8 kHz keeps parity
// with the XDK voice samples (the driver tells the device rate index 0).
static const DWORD SAMPLE_RATE      = 8000;
static const DWORD BYTES_PER_SAMPLE = 2;
static const DWORD NUM_PACKETS      = 4;
static const DWORD PACKET_SIZE      = SAMPLE_RATE * BYTES_PER_SAMPLE / 25; // 40 ms

//-----------------------------------------------------------------------------
// One communicator: microphone XMO -> headphone XMO + speaker stream
//-----------------------------------------------------------------------------
class CHawkLoopback
{
public:
    CHawkLoopback();
    ~CHawkLoopback() { Removed(); }

    HRESULT Inserted( DWORD dwPort );
    VOID    Removed();
    VOID    Process();

    // Stats accumulated from mic data, reset by the caller when printed
    DWORD   m_dwPackets;
    DWORD   m_dwSamples;
    DWORD   m_dwEnergy;         // sum of |sample|
    DWORD   m_dwZeroCrossings;
    SHORT   m_sPeak;

private:
    DWORD   m_dwPort;
    SHORT   m_sLastSample;

    BYTE*   m_pMicBuffer;
    DWORD   m_adwMicStatus[NUM_PACKETS];
    DWORD   m_dwMicPacket;
    XMediaObject* m_pMicXMO;

    BYTE*   m_pHpBuffer;
    DWORD   m_adwHpStatus[NUM_PACKETS];
    DWORD   m_dwHpPacket;
    XMediaObject* m_pHpXMO;

    BYTE*   m_pSpkBuffer;
    DWORD   m_adwSpkStatus[NUM_PACKETS];
    DWORD   m_dwSpkPacket;
    LPDIRECTSOUNDSTREAM m_pSpkStream;

    VOID    Account( const BYTE* pData, DWORD dwBytes );
};

CHawkLoopback::CHawkLoopback()
{
    m_dwPort      = 0xFFFFFFFF;
    m_pMicBuffer  = NULL;
    m_pHpBuffer   = NULL;
    m_pSpkBuffer  = NULL;
    m_pMicXMO     = NULL;
    m_pHpXMO      = NULL;
    m_pSpkStream  = NULL;
    m_dwPackets   = m_dwSamples = m_dwEnergy = m_dwZeroCrossings = 0;
    m_sPeak       = 0;
    m_sLastSample = 0;
}

HRESULT CHawkLoopback::Inserted( DWORD dwPort )
{
    HRESULT hr;
    m_dwPort = dwPort;
    printf( "port %lu: communicator inserted\n", dwPort );

    m_pMicBuffer = new BYTE[ PACKET_SIZE * NUM_PACKETS ];
    m_pHpBuffer  = new BYTE[ PACKET_SIZE * NUM_PACKETS ];
    m_pSpkBuffer = new BYTE[ PACKET_SIZE * NUM_PACKETS ];
    if( !m_pMicBuffer || !m_pHpBuffer || !m_pSpkBuffer )
        { Removed(); return E_OUTOFMEMORY; }

    WAVEFORMATEX wfx;
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.cbSize          = 0;
    wfx.nChannels       = 1;
    wfx.nSamplesPerSec  = SAMPLE_RATE;
    wfx.wBitsPerSample  = (WORD)( BYTES_PER_SAMPLE * 8 );
    wfx.nBlockAlign     = (WORD)( wfx.nChannels * wfx.wBitsPerSample / 8 );
    wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;

    hr = XVoiceCreateMediaObject( XDEVICE_TYPE_VOICE_MICROPHONE, dwPort,
                                  NUM_PACKETS, &wfx, &m_pMicXMO );
    if( FAILED( hr ) )
    {
        printf( "port %lu: microphone XMO failed 0x%08lx\n", dwPort, hr );
        Removed(); return hr;
    }

    hr = XVoiceCreateMediaObject( XDEVICE_TYPE_VOICE_HEADPHONE, dwPort,
                                  NUM_PACKETS, &wfx, &m_pHpXMO );
    if( FAILED( hr ) )
    {
        printf( "port %lu: headphone XMO failed 0x%08lx\n", dwPort, hr );
        Removed(); return hr;
    }

    // Speaker path: a DirectSound stream routed to the front speakers
    static DSMIXBINVOLUMEPAIR s_dsmbvp[] =
    {
        { DSMIXBIN_FRONT_LEFT,  DSBVOLUME_MAX },
        { DSMIXBIN_FRONT_RIGHT, DSBVOLUME_MAX },
    };
    DSMIXBINS dsmb = { 2, s_dsmbvp };

    DSSTREAMDESC dssd;
    ZeroMemory( &dssd, sizeof( dssd ) );
    dssd.dwMaxAttachedPackets = NUM_PACKETS;
    dssd.lpwfxFormat          = &wfx;
    dssd.lpMixBins            = &dsmb;

    hr = DirectSoundCreateStream( &dssd, &m_pSpkStream );
    if( FAILED( hr ) )
    {
        printf( "port %lu: speaker stream failed 0x%08lx\n", dwPort, hr );
        Removed(); return hr;
    }

    // Seed the microphone with all our packets; mark the sinks available
    for( DWORD i = 0; i < NUM_PACKETS; i++ )
    {
        XMEDIAPACKET xmp;
        ZeroMemory( &xmp, sizeof( xmp ) );
        xmp.dwMaxSize = PACKET_SIZE;
        xmp.pvBuffer  = m_pMicBuffer + i * PACKET_SIZE;
        xmp.pdwStatus = &m_adwMicStatus[i];
        m_pMicXMO->Process( NULL, &xmp );

        m_adwHpStatus[i]  = XMEDIAPACKET_STATUS_SUCCESS;
        m_adwSpkStatus[i] = XMEDIAPACKET_STATUS_SUCCESS;
    }
    m_dwMicPacket = m_dwHpPacket = m_dwSpkPacket = 0;
    m_sLastSample = 0;

    printf( "port %lu: loopback running (mic -> headphone + speakers)\n", dwPort );
    return S_OK;
}

VOID CHawkLoopback::Removed()
{
    if( m_dwPort != 0xFFFFFFFF )
        printf( "port %lu: communicator removed\n", m_dwPort );

    if( m_pMicXMO )    { m_pMicXMO->Release();    m_pMicXMO = NULL; }
    if( m_pHpXMO )     { m_pHpXMO->Release();     m_pHpXMO = NULL; }
    if( m_pSpkStream ) { m_pSpkStream->Release(); m_pSpkStream = NULL; }
    delete[] m_pMicBuffer; m_pMicBuffer = NULL;
    delete[] m_pHpBuffer;  m_pHpBuffer  = NULL;
    delete[] m_pSpkBuffer; m_pSpkBuffer = NULL;
    m_dwPort = 0xFFFFFFFF;
}

VOID CHawkLoopback::Account( const BYTE* pData, DWORD dwBytes )
{
    const SHORT* pS = (const SHORT*)pData;
    DWORD n = dwBytes / 2;
    SHORT prev = m_sLastSample;
    for( DWORD i = 0; i < n; i++ )
    {
        SHORT v = pS[i];
        m_dwEnergy += ( v < 0 ) ? -v : v;
        if( v > m_sPeak ) m_sPeak = v;
        if( ( prev < 0 && v >= 0 ) || ( prev >= 0 && v < 0 ) )
            m_dwZeroCrossings++;
        prev = v;
    }
    m_sLastSample = prev;
    m_dwSamples += n;
    m_dwPackets++;
}

VOID CHawkLoopback::Process()
{
    if( !m_pMicXMO )
        return;

    // Move every completed mic packet to both sinks (when both have room)
    while( m_adwMicStatus[ m_dwMicPacket ] != XMEDIAPACKET_STATUS_PENDING &&
           m_adwHpStatus [ m_dwHpPacket  ] != XMEDIAPACKET_STATUS_PENDING &&
           m_adwSpkStatus[ m_dwSpkPacket ] != XMEDIAPACKET_STATUS_PENDING )
    {
        BYTE* pMic = m_pMicBuffer + m_dwMicPacket * PACKET_SIZE;

        Account( pMic, PACKET_SIZE );
        memcpy( m_pHpBuffer  + m_dwHpPacket  * PACKET_SIZE, pMic, PACKET_SIZE );
        memcpy( m_pSpkBuffer + m_dwSpkPacket * PACKET_SIZE, pMic, PACKET_SIZE );

        // Resubmit the mic packet for more capture
        XMEDIAPACKET xmp;
        ZeroMemory( &xmp, sizeof( xmp ) );
        xmp.dwMaxSize = PACKET_SIZE;
        xmp.pvBuffer  = pMic;
        xmp.pdwStatus = &m_adwMicStatus[ m_dwMicPacket ];
        if( FAILED( m_pMicXMO->Process( NULL, &xmp ) ) )
            break;

        // Headphone (device) playback
        xmp.pvBuffer  = m_pHpBuffer + m_dwHpPacket * PACKET_SIZE;
        xmp.pdwStatus = &m_adwHpStatus[ m_dwHpPacket ];
        if( FAILED( m_pHpXMO->Process( &xmp, NULL ) ) )
            break;

        // Speaker playback
        xmp.pvBuffer  = m_pSpkBuffer + m_dwSpkPacket * PACKET_SIZE;
        xmp.pdwStatus = &m_adwSpkStatus[ m_dwSpkPacket ];
        if( FAILED( m_pSpkStream->Process( &xmp, NULL ) ) )
            break;

        m_dwMicPacket = ( m_dwMicPacket + 1 ) % NUM_PACKETS;
        m_dwHpPacket  = ( m_dwHpPacket  + 1 ) % NUM_PACKETS;
        m_dwSpkPacket = ( m_dwSpkPacket + 1 ) % NUM_PACKETS;
    }
}

//-----------------------------------------------------------------------------
// Name: main()
//-----------------------------------------------------------------------------
VOID __cdecl main()
{
    // Voice devices must be requested up front
    XDEVICE_PREALLOC_TYPE xdpt[] =
    {
        { XDEVICE_TYPE_VOICE_MICROPHONE, 4 },
        { XDEVICE_TYPE_VOICE_HEADPHONE,  4 },
    };
    XInitDevices( sizeof( xdpt ) / sizeof( xdpt[0] ), xdpt );

    printf( "HawkLoopback: waiting for a communicator (Hawk) on any port\n" );

    // Track mic and headphone state separately: a communicator is "present"
    // on a port only once BOTH halves have enumerated
    DWORD dwMicState = XGetDevices( XDEVICE_TYPE_VOICE_MICROPHONE );
    DWORD dwHpState  = XGetDevices( XDEVICE_TYPE_VOICE_HEADPHONE );
    DWORD dwConnected = 0;

    CHawkLoopback aPorts[4];

    // After a failed open, wait before retrying that port: a tight retry loop
    // hammers EP0 with open/close cycles hard enough to wedge the USB stack.
    DWORD adwRetryAt[4] = { 0, 0, 0, 0 };

    DWORD dwLastPrint = GetTickCount();
    for( ;; )
    {
        DWORD dwIns, dwRem;
        XGetDeviceChanges( XDEVICE_TYPE_VOICE_MICROPHONE, &dwIns, &dwRem );
        dwMicState = ( dwMicState & ~dwRem ) | dwIns;
        XGetDeviceChanges( XDEVICE_TYPE_VOICE_HEADPHONE, &dwIns, &dwRem );
        dwHpState  = ( dwHpState  & ~dwRem ) | dwIns;

        for( DWORD i = 0; i < 4; i++ )
        {
            BOOL bPresent = ( dwMicState & ( 1 << i ) ) &&
                            ( dwHpState  & ( 1 << i ) );
            if( bPresent && !( dwConnected & ( 1 << i ) ) &&
                (LONG)( GetTickCount() - adwRetryAt[i] ) >= 0 )
            {
                if( SUCCEEDED( aPorts[i].Inserted( i ) ) )
                    dwConnected |= ( 1 << i );
                else
                {
                    aPorts[i].Removed();
                    adwRetryAt[i] = GetTickCount() + 2000;
                }
            }
            else if( !bPresent && ( dwConnected & ( 1 << i ) ) )
            {
                aPorts[i].Removed();
                dwConnected &= ~( 1 << i );
            }

            if( dwConnected & ( 1 << i ) )
                aPorts[i].Process();
        }

        DirectSoundDoWork();

        // Once a second: per-port level + frequency estimate of the mic audio.
        // With the Hawk emulator's arpeggio this should read ~523/659/784 Hz.
        DWORD dwNow = GetTickCount();
        if( dwNow - dwLastPrint >= 1000 )
        {
            dwLastPrint = dwNow;
            for( DWORD i = 0; i < 4; i++ )
            {
                if( !( dwConnected & ( 1 << i ) ) )
                    continue;
                CHawkLoopback* p = &aPorts[i];
                DWORD dwAvg  = p->m_dwSamples ? p->m_dwEnergy / p->m_dwSamples : 0;
                DWORD dwFreq = p->m_dwSamples
                             ? (DWORD)( (unsigned __int64)p->m_dwZeroCrossings *
                                        SAMPLE_RATE / ( 2 * p->m_dwSamples ) )
                             : 0;
                printf( "port %lu: mic pkts=%lu avg=%lu peak=%d f~%luHz\n",
                        i, p->m_dwPackets, dwAvg, (int)p->m_sPeak, dwFreq );
                p->m_dwPackets = p->m_dwSamples = 0;
                p->m_dwEnergy  = p->m_dwZeroCrossings = 0;
                p->m_sPeak     = 0;
            }
        }

        Sleep( 4 );
    }
}
