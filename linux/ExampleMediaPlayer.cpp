#include <gtk/gtk.h>
#include <string>

#include <OpenHome/Media/Codec/CodecFactory.h>
#include <OpenHome/Media/Protocol/ProtocolFactory.h>
#include <OpenHome/Av/Product.h>
#include <OpenHome/Av/SourceFactory.h>
#include <OpenHome/Av/UpnpAv/UpnpAv.h>
#include <OpenHome/Av/Utils/IconDriverSongcastSender.h>
#include <OpenHome/Media/Debug.h>
#include <OpenHome/Av/Debug.h>
#include <OpenHome/Media/Pipeline/Pipeline.h>
#include <OpenHome/Private/Printer.h>
#include <OpenHome/Web/WebAppFramework.h>
#include <OpenHome/Web/ConfigUi/ConfigUi.h>

#include "ConfigGTKKeyStore.h"
#include "ControlPointProxy.h"
#include "CustomMessages.h"
#include "ExampleMediaPlayer.h"
#include "LitePipeTestApp.h"
#include "MediaPlayerIF.h"
#include "RamStore.h"

using namespace OpenHome;
using namespace OpenHome::Av;
using namespace OpenHome::Configuration;
using namespace OpenHome::Media;
using namespace OpenHome::Net;
using namespace OpenHome::Web;

// ExampleMediaPlayer

const Brn ExampleMediaPlayer::kSongcastSenderIconFileName("SongcastSenderIcon");

ExampleMediaPlayer::ExampleMediaPlayer(Net::DvStack& aDvStack,
                                       const Brx& aUdn,
                                       const TChar* aRoom,
                                       const TChar* aProductName,
                                       const Brx& aUserAgent)
    : iSemShutdown("TMPS", 0)
    , iDisabled("test", 0)
    , iPState(EPipelineStopped)
    , iLive(false)
    , iCpProxy(NULL)
    , iTxTimestamper(NULL)
    , iRxTimestamper(NULL)
    , iTxTsMapper(NULL)
    , iRxTsMapper(NULL)
    , iUserAgent(aUserAgent)
{
    Bws<256> friendlyName;
    friendlyName.Append(aRoom);
    friendlyName.Append(':');
    friendlyName.Append(aProductName);

    // create UPnP device
    iDevice = new DvDeviceStandard(aDvStack, aUdn, *this);
    iDevice->SetAttribute("Upnp.Domain", "av.openhome.org");
    iDevice->SetAttribute("Upnp.Type", "Source");
    iDevice->SetAttribute("Upnp.Version", "1");
    iDevice->SetAttribute("Upnp.FriendlyName", friendlyName.PtrZ());
    iDevice->SetAttribute("Upnp.Manufacturer", "OpenHome");
    iDevice->SetAttribute("Upnp.ModelName", "ExampleMediaPlayer");

    // create separate UPnP device for standard MediaRenderer
    Bws<256> buf(aUdn);
    buf.Append("-MediaRenderer");

    // The renderer name should be <room name>:<UPnP AV source name> to allow
    // our control point to match the renderer device to the upnp av source.
    Bws<256> rendererName(aRoom);
    rendererName.Append(":");
    rendererName.Append(SourceUpnpAv::kSourceName);
    iDeviceUpnpAv = new DvDeviceStandard(aDvStack, buf);
    iDeviceUpnpAv->SetAttribute("Upnp.Domain", "upnp.org");
    iDeviceUpnpAv->SetAttribute("Upnp.Type", "MediaRenderer");
    iDeviceUpnpAv->SetAttribute("Upnp.Version", "1");
    friendlyName.Append(":MediaRenderer");
    iDeviceUpnpAv->SetAttribute("Upnp.FriendlyName", rendererName.PtrZ());
    iDeviceUpnpAv->SetAttribute("Upnp.Manufacturer", "OpenHome");
    iDeviceUpnpAv->SetAttribute("Upnp.ModelName", "ExampleMediaPlayer");

    // create read/write store.  This creates a number of static (constant)
    // entries automatically
    iRamStore = new RamStore();

    // create a read/write store using the new config framework
    iConfigStore = ConfigGTKKeyStore::getInstance();

    iConfigStore->Write(Brn("Product.Room"), Brn(aRoom));
    iConfigStore->Write(Brn("Product.Name"), Brn(aProductName));

    // Volume Control
    VolumeProfile  volumeProfile;
    VolumeConsumer volumeInit;

    if (iVolume.IsVolumeSupported())
    {
        volumeInit.SetVolume(iVolume);
        volumeInit.SetBalance(iVolume);
        volumeInit.SetFade(iVolume);
    }
    else
    {
        Log::Print("Volume Control Unavailable\n");
    }

    // Set pipeline thread priority just below the pipeline animator.
    iInitParams = PipelineInitParams::New();
    iInitParams->SetThreadPriorityMax(kPriorityHighest);
    iInitParams->SetStarvationMonitorMaxSize(100 * Jiffies::kPerMs);

    // create MediaPlayer
    iMediaPlayer = new MediaPlayer( aDvStack, *iDevice, *iRamStore,
                                   *iConfigStore, iInitParams,
                                    volumeInit, volumeProfile, aUdn,
                                    Brn(aRoom), Brn(aProductName));

    // Register an observer, primarily to monitor the pipeline status.
    iMediaPlayer->Pipeline().AddObserver(*this);

    iPipelineStateLogger = new LoggingPipelineObserver();
    iMediaPlayer->Pipeline().AddObserver(*iPipelineStateLogger);

    // Set up config app.
    static const TUint addr = 0;    // Bind to all addresses.
    static const TUint port = 0;    // Bind to whatever free port the OS
                                    // allocates to the framework server.

    iAppFramework = new WebAppFramework(aDvStack.Env(),
                                        addr,
                                        port,
                                        kMaxUiTabs,
                                        kUiSendQueueSize);

}

ExampleMediaPlayer::~ExampleMediaPlayer()
{
    ASSERT(!iDevice->Enabled());
    delete iAppFramework;
    delete iCpProxy;
    delete iPipelineStateLogger;
    delete iMediaPlayer;
    delete iDevice;
    delete iDeviceUpnpAv;
    delete iRamStore;
}

Environment& ExampleMediaPlayer::Env()
{
    return iMediaPlayer->Env();
}

void ExampleMediaPlayer::SetSongcastTimestampers(
               IOhmTimestamper& aTxTimestamper, IOhmTimestamper& aRxTimestamper)
{
    iTxTimestamper = &aTxTimestamper;
    iRxTimestamper = &aRxTimestamper;
}

void ExampleMediaPlayer::SetSongcastTimestampMappers(
                                              IOhmTimestampMapper& aTxTsMapper,
                                              IOhmTimestampMapper& aRxTsMapper)
{
    iTxTsMapper = &aTxTsMapper;
    iRxTsMapper = &aRxTsMapper;
}

void ExampleMediaPlayer::StopPipeline()
{
    TUint waitCount = 0;

    if (TryDisable(*iDevice))
    {
        waitCount++;
    }

    if (TryDisable(*iDeviceUpnpAv))
    {
        waitCount++;
    }

    while (waitCount > 0)
    {
        iDisabled.Wait();
        waitCount--;
    }

    iMediaPlayer->Quit();
    iSemShutdown.Signal();
}

TBool ExampleMediaPlayer::CanPlay()
{
    return ((iPState == EPipelineStopped) || (iPState == EPipelinePaused));
}

TBool ExampleMediaPlayer::CanPause()
{
    return (!iLive &&
            ((iPState == EPipelinePlaying) || (iPState == EPipelineBuffering)));
}

TBool ExampleMediaPlayer::CanHalt()
{
    return ((iPState == EPipelinePlaying) || (iPState == EPipelinePaused));
}

void ExampleMediaPlayer::PlayPipeline()
{
    if (CanPlay())
    {
        iCpProxy->playlistPlay();
    }
}

void ExampleMediaPlayer::PausePipeline()
{
    if (CanPause())
    {
        iCpProxy->playlistPause();
    }
}

void ExampleMediaPlayer::HaltPipeline()
{
    if (CanHalt())
    {
        iCpProxy->playlistStop();
    }
}

void ExampleMediaPlayer::AddAttribute(const TChar* aAttribute)
{
    iMediaPlayer->AddAttribute(aAttribute);
}

void ExampleMediaPlayer::RunWithSemaphore(Net::CpStack& aCpStack)
{
    RegisterPlugins(iMediaPlayer->Env());
    AddConfigApp();
    iMediaPlayer->Start();
    iAppFramework->Start();
    iDevice->SetEnabled();
    iDeviceUpnpAv->SetEnabled();

    iCpProxy = new ControlPointProxy(aCpStack, *(Device()));

    iSemShutdown.Wait();
}

PipelineManager& ExampleMediaPlayer::Pipeline()
{
    return iMediaPlayer->Pipeline();
}

DvDeviceStandard* ExampleMediaPlayer::Device()
{
    return iDevice;
}

void ExampleMediaPlayer::RegisterPlugins(Environment& aEnv)
{
    const Brn kSupportedProtocols(
        "http-get:*:audio/x-flac:*,"    // Flac
        "http-get:*:audio/wav:*,"       // Wav
        "http-get:*:audio/wave:*,"      // Wav
        "http-get:*:audio/x-wav:*,"     // Wav
        "http-get:*:audio/aiff:*,"      // AIFF
        "http-get:*:audio/x-aiff:*,"    // AIFF
        "http-get:*:audio/x-m4a:*,"     // Alac
        "http-get:*:audio/x-scpls:*,"   // M3u (content processor)
        "http-get:*:text/xml:*,"        // Opml ??  (content processor)
        "http-get:*:audio/aac:*,"       // Aac
        "http-get:*:audio/aacp:*,"      // Aac
        "http-get:*:audio/mp4:*,"       // Mpeg4 (container)
        "http-get:*:audio/ogg:*,"       // Vorbis
        "http-get:*:audio/x-ogg:*,"     // Vorbis
        "http-get:*:application/ogg:*," // Vorbis
        //"tidalhifi.com:*:*:*,"          // Tidal
        //"qobuz.com:*:*:*"               // Qobuz
        );
    DoRegisterPlugins(aEnv, kSupportedProtocols);
}

void ExampleMediaPlayer::DoRegisterPlugins(Environment& aEnv, const Brx& aSupportedProtocols)
{
    // Add codecs
    Log::Print("Codec Registration: [\n");

    // Disabled by default - requires patent license
    //Log::Print("Codec\tAac\n");
    //iMediaPlayer->Add(Codec::CodecFactory::NewAac());
    Log::Print("Codec\tAiff\n");
    iMediaPlayer->Add(Codec::CodecFactory::NewAiff());
    Log::Print("Codec\tAifc\n");
    iMediaPlayer->Add(Codec::CodecFactory::NewAifc());
    Log::Print("Codec\tAlac\n");
    iMediaPlayer->Add(Codec::CodecFactory::NewAlac());
    Log::Print("Codec\tAdts\n");
    iMediaPlayer->Add(Codec::CodecFactory::NewAdts());
    Log::Print("Codec:\tFlac\n");
    iMediaPlayer->Add(Codec::CodecFactory::NewFlac());
    // Disabled by default - requires patent and copyright licenses
    //Log::Print("Codec:\tMP3\n");
    //iMediaPlayer->Add(Codec::CodecFactory::NewMp3());
    Log::Print("Codec\tPcm\n");
    iMediaPlayer->Add(Codec::CodecFactory::NewPcm());
    Log::Print("Codec\tVorbis\n");
    iMediaPlayer->Add(Codec::CodecFactory::NewVorbis());
    Log::Print("Codec\tWav\n");
    iMediaPlayer->Add(Codec::CodecFactory::NewWav());

    Log::Print("]\n");

    // Add protocol modules
    iMediaPlayer->Add(ProtocolFactory::NewHttp(aEnv, iUserAgent));
    iMediaPlayer->Add(ProtocolFactory::NewHttp(aEnv, iUserAgent));
    iMediaPlayer->Add(ProtocolFactory::NewHttp(aEnv, iUserAgent));
    iMediaPlayer->Add(ProtocolFactory::NewHttp(aEnv, iUserAgent));
    iMediaPlayer->Add(ProtocolFactory::NewHttp(aEnv, iUserAgent));
    iMediaPlayer->Add(ProtocolFactory::NewHls(aEnv, iUserAgent));

    // Add sources
    iMediaPlayer->Add(SourceFactory::NewPlaylist(*iMediaPlayer,
                                                 aSupportedProtocols));

    iMediaPlayer->Add(SourceFactory::NewUpnpAv(*iMediaPlayer,
                                               *iDeviceUpnpAv,
                                                aSupportedProtocols));

    iMediaPlayer->Add(SourceFactory::NewReceiver(*iMediaPlayer,
                                                  NULL,
                                                  iTxTimestamper,
                                                  iTxTsMapper,
                                                  iRxTimestamper,
                                                  iRxTsMapper,
                                                  kSongcastSenderIconFileName));
}

void ExampleMediaPlayer::WriteResource(const Brx&          aUriTail,
                                       TIpAddress          /*aInterface*/,
                                       std::vector<char*>& /*aLanguageList*/,
                                       IResourceWriter&    aResourceWriter)
{
    if (aUriTail == kSongcastSenderIconFileName)
    {
        aResourceWriter.WriteResourceBegin(sizeof(kIconDriverSongcastSender),
                                           kIconDriverSongcastSenderMimeType);
        aResourceWriter.WriteResource(kIconDriverSongcastSender,
                                      sizeof(kIconDriverSongcastSender));
        aResourceWriter.WriteResourceEnd();
    }
}

void ExampleMediaPlayer::AddConfigApp()
{
    std::vector<const Brx*> sourcesBufs;
    Product& product = iMediaPlayer->Product();
    for (TUint i=0; i<product.SourceCount(); i++) {
        Bws<ISource::kMaxSystemNameBytes> systemName;
        Bws<ISource::kMaxSourceNameBytes> name;
        Bws<ISource::kMaxSourceTypeBytes> type;
        TBool visible;
        product.GetSourceDetails(i, systemName, type, name, visible);
        sourcesBufs.push_back(new Brh(systemName));
    }

    iConfigApp = new ConfigAppMediaPlayer(iMediaPlayer->ConfigManager(),
                                          sourcesBufs,
                                          Brn("Softplayer"),
                                          Brn("/usr/share/"
                                              "litepipe-test-app/res/"),
                                          kMaxUiTabs,
                                          kUiSendQueueSize);

    iAppFramework->Add(iConfigApp,              // iAppFramework takes ownership
                       MakeFunctorGeneric(*this, &ExampleMediaPlayer::PresentationUrlChanged));

    for (TUint i=0;i<sourcesBufs.size(); i++) {
        delete sourcesBufs[i];
    }
}

void ExampleMediaPlayer::PresentationUrlChanged(const Brx& aUrl)
{
    if (!iDevice->Enabled()) {
        // FIXME - can only set Product attribute once (meaning no updates on subnet change)
        const TBool firstChange = (iPresentationUrl.Bytes() == 0);
        iPresentationUrl.Replace(aUrl);
        iDevice->SetAttribute("Upnp.PresentationUrl", iPresentationUrl.PtrZ());
        if (firstChange) {
            Bws<128> configAtt("App:Config=");
            configAtt.Append(iPresentationUrl);
            iMediaPlayer->Product().AddAttribute(configAtt);
        }
    }
}

TBool ExampleMediaPlayer::TryDisable(DvDevice& aDevice)
{
    if (aDevice.Enabled())
    {
        aDevice.SetDisabled(MakeFunctor(*this, &ExampleMediaPlayer::Disabled));
        return true;
    }

    return false;
}

void ExampleMediaPlayer::Disabled()
{
    iDisabled.Signal();
}

// ExampleMediaPlayerInit

OpenHome::Net::Library* ExampleMediaPlayerInit::CreateLibrary(TUint32 preferredSubnet)
{
    TUint                 index         = 0;
    InitialisationParams *initParams    = InitialisationParams::Create();
    TIpAddress            lastSubnet    = InitArgs::NO_SUBNET;;
    const TChar          *lastSubnetStr = "Subnet.LastUsed";

    //initParams->SetDvEnableBonjour();

    Net::Library* lib = new Net::Library(initParams);

    std::vector<NetworkAdapter*>* subnetList = lib->CreateSubnetList();

    if (subnetList->size() == 0)
    {
        Log::Print("ERROR: No adapters found\n");
        ASSERTS();
    }

    ConfigGTKKeyStore *configStore = ConfigGTKKeyStore::getInstance();

    // Check the configuration store for the last subnet joined.
    try
    {
        Bwn lastSubnetBuf = Bwn((TByte *)&lastSubnet, sizeof(lastSubnet));

        configStore->Read(Brn(lastSubnetStr), lastSubnetBuf);
    }
    catch (StoreKeyNotFound&)
    {
        // No previous subnet stored.
    }
    catch (StoreReadBufferUndersized&)
    {
        // This shouldn't happen.
        Log::Print("ERROR: Invalid 'Subnet.LastUsed' property in Config "
                   "Store\n");
    }

    for (TUint i=0; i<subnetList->size(); ++i)
    {
        TIpAddress subnet = (*subnetList)[i]->Subnet();

        // If the requested subnet is available, choose it.
        if (subnet == preferredSubnet)
        {
            index = i;
            break;
        }

        // If the last used subnet is available, note it.
        // We'll fall back to it if the requested subnet is not available.
        if (subnet == lastSubnet)
        {
            index = i;
        }
    }

    // Choose the required adapter.
    TIpAddress subnet = (*subnetList)[index]->Subnet();

    Library::DestroySubnetList(subnetList);
    lib->SetCurrentSubnet(subnet);

    // Store the selected subnet in persistent storage.
    configStore->Write(Brn(lastSubnetStr),
                       Brn((TByte *)&subnet, sizeof(subnet)));

    Log::Print("Using Subnet %d.%d.%d.%d\n", subnet&0xff, (subnet>>8)&0xff,
                                             (subnet>>16)&0xff,
                                             (subnet>>24)&0xff);

    return lib;
}

// Pipeline Observer callbacks.
void ExampleMediaPlayer::NotifyPipelineState(Media::EPipelineState aState)
{
    unsigned int mediaOptions = 0;

    iPState = aState;

    // Update the playback options available in the UI.
    if (CanPlay())
    {
        mediaOptions |= MEDIAPLAYER_PLAY_OPTION;
    }

    if (CanPause())
    {
        mediaOptions |= MEDIAPLAYER_PAUSE_OPTION;
    }

    if (CanHalt())
    {
        mediaOptions |= MEDIAPLAYER_STOP_OPTION;
    }

    gdk_threads_add_idle((GSourceFunc)updateUI, GINT_TO_POINTER(mediaOptions));

    switch (iPState)
    {
        case EPipelineStopped:
            Log::Print("Pipeline State: Stopped\n");
            break;
        case EPipelinePaused:
            Log::Print("Pipeline State: Paused\n");
            break;
        case EPipelinePlaying:
            Log::Print("Pipeline State: Playing\n");
            break;
        case EPipelineBuffering:
            Log::Print("Pipeline State: Buffering\n");
            break;
        case EPipelineWaiting:
            Log::Print("Pipeline State: Waiting\n");
            break;
        default:
            Log::Print("Pipeline State: UNKNOWN\n");
            break;
    }
}

void ExampleMediaPlayer::NotifyMode(const Brx& /*aMode*/, const Media::ModeInfo& /*aInfo*/)
{
}

void ExampleMediaPlayer::NotifyTrack(Media::Track& /*aTrack*/,
                                     const Brx&    /*aMode*/,
                                     TBool         /*aStartOfStream*/)
{
}

void ExampleMediaPlayer::NotifyMetaText(const Brx& /*aText*/)
{
}

void ExampleMediaPlayer::NotifyTime(TUint /*aSeconds*/,
                                    TUint /*aTrackDurationSeconds*/)
{
}

void ExampleMediaPlayer::NotifyStreamInfo(const Media::DecodedStreamInfo& aStreamInfo)
{
    iLive = aStreamInfo.Live();
}