/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Throughput Scenario #3
 *
 * Main BSS: 1 AP + 1 STA
 *   SLO   – 1 link,  5 GHz ch42  80 MHz
 *   STR   – 2 links, 5 GHz ch42  +  6 GHz ch7  (both 80 MHz)
 *   EMLSR – same as STR with EMLSR enabled
 *
 * OBSS0: nObss independent BSSs on 5 GHz ch42  (always; co-channel with main link 0)
 * OBSS1: nObss independent BSSs on 6 GHz ch7   (STR/EMLSR only; co-channel with main link 1)
 * Each OBSS BSS has its own AP+STA so that every increment of nObss adds one
 * independent MAC contender, giving a monotonically increasing interference load.
 *
 * PHY: EHT 802.11be, MCS 13, 2 SS, GI 800 ns, 80 MHz
 * MPDU aggregation: nMpdus frames per A-MPDU
 * Traffic: saturated DL (AP->STA) + UL (STA->AP) UDP in main BSS;
 *           saturated DL only in OBSSs (interference generators)
 * AP-STA max distance: 5 m
 *
 * Output lines parsed by runner:
 *   "MainBSS DL MacRx: X.XX Mb/s"
 *   "MainBSS UL MacRx: X.XX Mb/s"
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/multi-model-spectrum-channel.h"
#include "ns3/network-module.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/wifi-module.h"
#include "ns3/wifi-phy-band.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("WifiMloScenario3");

// MacRx byte counters – main BSS only
static uint64_t g_dlBytes = 0; // STA receives (DL)
static uint64_t g_ulBytes = 0; // AP  receives (UL)

static void
MacRxDl(std::string /*ctx*/, Ptr<const Packet> pkt)
{
    g_dlBytes += pkt->GetSize();
}

static void
MacRxUl(std::string /*ctx*/, Ptr<const Packet> pkt)
{
    g_ulBytes += pkt->GetSize();
}

int
main(int argc, char* argv[])
{
    // -----------------------------------------------------------------------
    //  Parameters
    // -----------------------------------------------------------------------
    double      simTime     = 10.0;
    uint32_t    payloadSize = 1500;
    uint32_t    nMpdus      = 1024;
    std::string mloMode     = "STR"; // SLO | STR | EMLSR
    std::string direction   = "DL";  // DL | UL
    uint32_t    nObss       = 1;     // stations per OBSS (0..10)
    uint32_t    rtsThreshold = 1000; // RTS/CTS threshold (default ON for the paper)

    CommandLine cmd(__FILE__);
    cmd.AddValue("simTime",     "Simulation time [s]",              simTime);
    cmd.AddValue("payloadSize", "UDP payload size [B]",             payloadSize);
    cmd.AddValue("nMpdus",      "Max MPDUs per A-MPDU",             nMpdus);
    cmd.AddValue("mloMode",     "Main BSS mode: SLO | STR | EMLSR", mloMode);
    cmd.AddValue("direction",   "Main BSS direction: DL | UL",      direction);
    cmd.AddValue("nObss",       "Stations per OBSS (0 - 10)",       nObss);
    cmd.AddValue("rtsThreshold","RTS/CTS threshold [B]",            rtsThreshold);
    cmd.Parse(argc, argv); // --RngRun=N handled automatically by ns-3

    Config::SetDefault("ns3::WifiRemoteStationManager::RtsCtsThreshold",
                       UintegerValue(rtsThreshold));

    if (mloMode != "SLO" && mloMode != "STR" && mloMode != "EMLSR")
        NS_ABORT_MSG("mloMode must be SLO, STR, or EMLSR");
    if (direction != "DL" && direction != "UL")
        NS_ABORT_MSG("direction must be DL or UL");
    NS_ABORT_MSG_IF(nObss > 10u, "nObss must be 0..10");

    const bool    isMlo      = (mloMode != "SLO");
    const uint8_t nMainLinks = isMlo ? 2 : 1;   // SLO=1 (5 GHz only); MLO=2 (5 + 6 GHz)

    RngSeedManager::SetSeed(1); // run number controlled by --RngRun

    std::cout << "=== Scenario 3 ===\n"
              << "Mode: " << mloMode << "  Direction: " << direction
              << "  nObss: " << nObss
              << "  links: " << +nMainLinks << "\n\n";

    // -----------------------------------------------------------------------
    //  Nodes
    // -----------------------------------------------------------------------
    NodeContainer mainAp;   mainAp.Create(1);
    NodeContainer mainSta;  mainSta.Create(1);

    // OBSS0: nObss independent BSSs on 5 GHz ch42 — each has 1 AP + 1 STA.
    // Naming: "obss0-K" for K in 0..nObss-1.
    NodeContainer obss0Aps;  if (nObss > 0) obss0Aps.Create(nObss);
    NodeContainer obss0Stas; if (nObss > 0) obss0Stas.Create(nObss);

    // OBSS1: same on 6 GHz ch7 (STR/EMLSR only)
    NodeContainer obss1Aps;
    NodeContainer obss1Stas;
    if (isMlo && nObss > 0)
    {
        obss1Aps.Create(nObss);
        obss1Stas.Create(nObss);
    }

    // -----------------------------------------------------------------------
    //  Spectrum channels
    //  ch5 is shared by main BSS link-0 (ch42) and OBSS0 (ch42).
    //  ch6 is shared by main BSS link-1 (ch7)  and OBSS1 (ch7).
    //  Separate objects ensure cross-band isolation; WifiBandwidthFilter
    //  handles in-band adjacent-channel rejection within each object.
    // -----------------------------------------------------------------------
    auto ch5 = CreateObject<MultiModelSpectrumChannel>();
    {
        Ptr<LogDistancePropagationLossModel> loss = CreateObject<LogDistancePropagationLossModel>();
        loss->SetAttribute("Exponent",          DoubleValue(3.5));
        loss->SetAttribute("ReferenceLoss",     DoubleValue(40.0));
        loss->SetAttribute("ReferenceDistance", DoubleValue(1.0));
        ch5->AddPropagationLossModel(loss);
        ch5->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());
    }

    Ptr<MultiModelSpectrumChannel> ch6;
    if (isMlo)
    {
        ch6 = CreateObject<MultiModelSpectrumChannel>();
        Ptr<LogDistancePropagationLossModel> loss6 = CreateObject<LogDistancePropagationLossModel>();
        loss6->SetAttribute("Exponent",          DoubleValue(3.5));
        loss6->SetAttribute("ReferenceLoss",     DoubleValue(40.0));
        loss6->SetAttribute("ReferenceDistance", DoubleValue(1.0));
        ch6->AddPropagationLossModel(loss6);
        ch6->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());
    }

    // Lambda: add one (channel, frequency-range, channelSettings) to a PHY helper link
    auto cfgLink = [](SpectrumWifiPhyHelper& phy,
                      uint32_t linkId,
                      Ptr<MultiModelSpectrumChannel> ch,
                      const FrequencyRange& range,
                      const char* chanSettings)
    {
        phy.AddChannel(ch, range);
        phy.Set(linkId, "ChannelSettings",             StringValue(chanSettings));
        phy.Set(linkId, "Antennas",                    UintegerValue(2));
        phy.Set(linkId, "MaxSupportedTxSpatialStreams", UintegerValue(2));
        phy.Set(linkId, "MaxSupportedRxSpatialStreams", UintegerValue(2));
    };

    // -----------------------------------------------------------------------
    //  PHY helpers
    // -----------------------------------------------------------------------
    SpectrumWifiPhyHelper phyMain(nMainLinks);
    phyMain.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
    if (isMlo)
        phyMain.SetPcapCaptureType(WifiPhyHelper::PcapCaptureType::PCAP_PER_LINK);
    cfgLink(phyMain, 0, ch5, WIFI_SPECTRUM_5_GHZ, "{42, 80, BAND_5GHZ, 0}");
    if (isMlo)
    {
        cfgLink(phyMain, 1, ch6,  WIFI_SPECTRUM_6_GHZ, "{7, 80, BAND_6GHZ, 0}");
    }

    // OBSS0: single link, 5 GHz ch42 (same channel as main link 0)
    SpectrumWifiPhyHelper phyObss0(1);
    phyObss0.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
    cfgLink(phyObss0, 0, ch5, WIFI_SPECTRUM_5_GHZ, "{42, 80, BAND_5GHZ, 0}");

    // OBSS1: single link, 6 GHz ch7  (same channel as main link 1; STR/EMLSR only)
    SpectrumWifiPhyHelper phyObss1(1); // configured below only when isMlo
    if (isMlo)
    {
        phyObss1.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
        cfgLink(phyObss1, 0, ch6, WIFI_SPECTRUM_6_GHZ, "{7, 80, BAND_6GHZ, 0}");
    }

    // -----------------------------------------------------------------------
    //  WiFi helpers
    // -----------------------------------------------------------------------
    const char* DATA_MODE = "EhtMcs13";
    const char* CTL_MODE  = "EhtMcs0"; // valid in both 5 & 6 GHz (HE-only 6 GHz)

    // Main BSS (nMainLinks RSMs)
    WifiHelper wifiMain;
    wifiMain.SetStandard(WIFI_STANDARD_80211be);
    wifiMain.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
    wifiMain.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                     "DataMode",    StringValue(DATA_MODE),
                                     "ControlMode", StringValue(CTL_MODE));
    if (mloMode == "EMLSR")
    {
        wifiMain.ConfigEhtOptions("EmlsrActivated",      BooleanValue(true));
        wifiMain.ConfigEhtOptions("TransitionTimeout",   TimeValue(MicroSeconds(1024)));
        // ns-3 default MsdMaxNTxops = 1 caps EMLSR to a single TXOP per
        // 5.5 ms MediumSyncDelay window, which throttles EMLSR throughput
        // severely in steady-state traffic. Match the 4.1.1 baseline.
        wifiMain.ConfigEhtOptions("MediumSyncDuration",  TimeValue(MicroSeconds(3200)));
        wifiMain.ConfigEhtOptions("MsdOfdmEdThreshold",  IntegerValue(-72));
        wifiMain.ConfigEhtOptions("MsdMaxNTxops",        UintegerValue(0));   // unlimited
    }

    // OBSS helpers (always single-link, same MCS)
    WifiHelper wifiObss;
    wifiObss.SetStandard(WIFI_STANDARD_80211be);
    wifiObss.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
    // Keep default station manager here to avoid overload ambiguity in this build setup.

    WifiMacHelper wifiMac;
    // No multi-user scheduler: every BSS in this scenario has exactly 1 STA,
    // so DL OFDMA cannot be scheduled.  The RrMultiUserScheduler was the
    // source of intermittent SIGABRT crashes (WifiTxVector NS_ASSERT in
    // GetMode() when a STA reassociates under heavy co-channel load).
    // AMPDU aggregation is controlled independently via BE_MaxAmpduSize /
    // MpduBufferSize and is unaffected by removing the scheduler.

    auto setApMac = [&](const char* ssid) {
        wifiMac.SetType("ns3::ApWifiMac",
                        "Ssid",            SsidValue(Ssid(ssid)),
                        "BE_MaxAmpduSize", UintegerValue(6500631),
                        "MpduBufferSize",  UintegerValue(nMpdus));
    };
    auto setStaMac = [&](const char* ssid) {
        wifiMac.SetType("ns3::StaWifiMac",
                        "Ssid",            SsidValue(Ssid(ssid)),
                        "ActiveProbing",   BooleanValue(false),
                        "BE_MaxAmpduSize", UintegerValue(6500631),
                        "MpduBufferSize",  UintegerValue(nMpdus));
    };

    // -----------------------------------------------------------------------
    //  Install devices
    //  Order: AP installs first, then all OBSS STAs, then EMLSR manager,
    //  then main BSS STA — so EMLSR manager does not leak into OBSS STAs.
    // -----------------------------------------------------------------------
    setApMac("main");
    NetDeviceContainer mainApDev = wifiMain.Install(phyMain, wifiMac, mainAp);

    // OBSS0: N independent BSSs, each "obss0-K" SSID
    std::vector<NetDeviceContainer> obss0ApDevs(nObss);
    std::vector<NetDeviceContainer> obss0StaDevs(nObss);
    for (uint32_t k = 0; k < nObss; ++k)
    {
        std::string ssid = "obss0-" + std::to_string(k);
        setApMac(ssid.c_str());
        obss0ApDevs[k] = wifiObss.Install(phyObss0, wifiMac, obss0Aps.Get(k));
        setStaMac(ssid.c_str());
        obss0StaDevs[k] = wifiObss.Install(phyObss0, wifiMac, obss0Stas.Get(k));
    }

    // OBSS1: N independent BSSs on 6 GHz (STR/EMLSR only)
    std::vector<NetDeviceContainer> obss1ApDevs(nObss);
    std::vector<NetDeviceContainer> obss1StaDevs(nObss);
    if (isMlo)
    {
        for (uint32_t k = 0; k < nObss; ++k)
        {
            std::string ssid = "obss1-" + std::to_string(k);
            setApMac(ssid.c_str());
            obss1ApDevs[k] = wifiObss.Install(phyObss1, wifiMac, obss1Aps.Get(k));
            setStaMac(ssid.c_str());
            obss1StaDevs[k] = wifiObss.Install(phyObss1, wifiMac, obss1Stas.Get(k));
        }
    }

    if (mloMode == "EMLSR")
    {
        wifiMac.SetEmlsrManager("ns3::DefaultEmlsrManager",
                                // Exclude the 2.4 GHz link (link 2): its 40-MHz max width
// can't satisfy AuxPhyChannelWidth=80, which corrupts the
// AuxPhy selection logic and collapses EMLSR throughput.
"EmlsrLinkSet",         StringValue("0,1"),
                                "SwitchAuxPhy",         BooleanValue(true),
                                "AuxPhyChannelWidth",   UintegerValue(80),
                                "AuxPhyMaxModClass",    StringValue("EHT"),
                                "AuxPhyTxCapable",      BooleanValue(true),
                                "PutAuxPhyToSleep",     BooleanValue(false));
    }

    setStaMac("main");
    NetDeviceContainer mainStaDev = wifiMain.Install(phyMain, wifiMac, mainSta);

    // STR link balancing: ns-3's default link selector preferentially fills
    // link 0, leaving link 1 under-utilised, so STR throughput sits at ~1.5x
    // SLO instead of the expected ~2x. Map all TIDs to BOTH links on both AP
    // and STA so the QosTxop scheduler distributes packets evenly.
    if (mloMode == "STR")
    {
        std::map<std::list<uint8_t>, std::list<uint8_t>> evenMap = {
            {{0, 1, 2, 3, 4, 5, 6, 7}, {0, 1}}};
        for (uint32_t i = 0; i < mainApDev.GetN(); ++i)
        {
            auto d = DynamicCast<WifiNetDevice>(mainApDev.Get(i));
            d->GetEhtConfiguration()->SetTidLinkMapping(WifiDirection::DOWNLINK, evenMap);
            d->GetEhtConfiguration()->SetTidLinkMapping(WifiDirection::UPLINK,   evenMap);
        }
        for (uint32_t i = 0; i < mainStaDev.GetN(); ++i)
        {
            auto d = DynamicCast<WifiNetDevice>(mainStaDev.Get(i));
            d->GetEhtConfiguration()->SetTidLinkMapping(WifiDirection::DOWNLINK, evenMap);
            d->GetEhtConfiguration()->SetTidLinkMapping(WifiDirection::UPLINK,   evenMap);
        }
    }

    // -----------------------------------------------------------------------
    //  Mobility
    //  All APs are within mutual CCA range (20 m separation).
    //  All STAs are within 5 m of their AP (r = 5 per Table 3.5).
    // -----------------------------------------------------------------------
    MobilityHelper mobility;
    Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator>();

    // Main BSS
    pos->Add(Vector(0.0, 0.0, 0.0)); // Main AP
    pos->Add(Vector(4.0, 0.0, 0.0)); // Main STA (4 m)

    // OBSS0: N truly overlapping BSSs on 5 GHz ch42.
    // All APs clustered within 4 m of the main BSS so every BSS is well above
    // the 80 MHz CCA threshold (~-72 dBm) — i.e. no hidden-terminal regime.
    // APs on a small circle of radius 3 m around (5, 5); STAs 1 m further out.
    for (uint32_t k = 0; k < nObss; ++k)
    {
        double theta = 2.0 * M_PI * static_cast<double>(k) / 10.0;
        double apX   = 5.0 + 3.0 * std::cos(theta);
        double apY   = 5.0 + 3.0 * std::sin(theta);
        double staX  = 5.0 + 4.0 * std::cos(theta);
        double staY  = 5.0 + 4.0 * std::sin(theta);
        pos->Add(Vector(apX,  apY,  0.0)); // OBSS0 AP k
        pos->Add(Vector(staX, staY, 0.0)); // OBSS0 STA k
    }

    // OBSS1: same cluster but offset on +y to keep node addressing distinct.
    if (isMlo)
    {
        for (uint32_t k = 0; k < nObss; ++k)
        {
            double theta = 2.0 * M_PI * static_cast<double>(k) / 10.0;
            double apX   = 5.0 + 3.0 * std::cos(theta);
            double apY   = 5.0 + 3.0 * std::sin(theta);
            double staX  = 5.0 + 4.0 * std::cos(theta);
            double staY  = 5.0 + 4.0 * std::sin(theta);
            pos->Add(Vector(apX,  apY,  0.0));
            pos->Add(Vector(staX, staY, 0.0));
        }
    }

    mobility.SetPositionAllocator(pos);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(mainAp);
    mobility.Install(mainSta);
    if (nObss > 0)
    {
        // Install interleaved: AP0, STA0, AP1, STA1, ... to match pos order
        for (uint32_t k = 0; k < nObss; ++k)
        {
            mobility.Install(obss0Aps.Get(k));
            mobility.Install(obss0Stas.Get(k));
        }
    }
    if (isMlo && nObss > 0)
    {
        for (uint32_t k = 0; k < nObss; ++k)
        {
            mobility.Install(obss1Aps.Get(k));
            mobility.Install(obss1Stas.Get(k));
        }
    }

    // -----------------------------------------------------------------------
    //  Internet stack + IP (separate /24 per BSS)
    // -----------------------------------------------------------------------
    InternetStackHelper stack;
    stack.Install(mainAp);
    stack.Install(mainSta);
    if (nObss > 0)
    {
        stack.Install(obss0Aps);
        stack.Install(obss0Stas);
    }
    if (isMlo && nObss > 0)
    {
        stack.Install(obss1Aps);
        stack.Install(obss1Stas);
    }

    Ipv4AddressHelper addr;

    addr.SetBase("10.1.0.0", "255.255.255.0");
    Ipv4InterfaceContainer mainApIf  = addr.Assign(mainApDev);
    Ipv4InterfaceContainer mainStaIf = addr.Assign(mainStaDev);

    // OBSS0: /24 per BSS starting at 10.2.0.0
    std::vector<Ipv4InterfaceContainer> obss0StaIfs(nObss);
    for (uint32_t k = 0; k < nObss; ++k)
    {
        std::ostringstream base;
        base << "10." << (2 + k) << ".0.0";
        addr.SetBase(base.str().c_str(), "255.255.255.0");
        addr.Assign(obss0ApDevs[k]);
        obss0StaIfs[k] = addr.Assign(obss0StaDevs[k]);
    }

    // OBSS1: /24 per BSS starting at 10.102.0.0
    std::vector<Ipv4InterfaceContainer> obss1StaIfs(nObss);
    if (isMlo)
    {
        for (uint32_t k = 0; k < nObss; ++k)
        {
            std::ostringstream base;
            base << "10." << (102 + k) << ".0.0";
            addr.SetBase(base.str().c_str(), "255.255.255.0");
            addr.Assign(obss1ApDevs[k]);
            obss1StaIfs[k] = addr.Assign(obss1StaDevs[k]);
        }
    }

    // -----------------------------------------------------------------------
    //  Traffic
    //   Main BSS: full-buffer UDP in one direction (DL or UL per arg).
    //   OBSS BSSs: bursty DL — Chen et al. 2022 Fig. 5 model: each OBSS sends
    //              150 kB every ~8.3 ms (≈ 10 % of MCS-13 / 2-SS / 80 MHz
    //              transmission rate). Implemented as OnOffApplication with
    //              On=0.8 ms @ 1500 Mb/s, Off=7.5 ms (cycle 8.3 ms).
    // -----------------------------------------------------------------------
    double   tStart = 1.0;
    double   tStop  = simTime + 1.0;
    uint16_t port   = 5000;

    // Saturated UDP flow (main BSS only)
    auto addSatFlow = [&](Ptr<Node> txNode, Ptr<Node> rxNode, Ipv4Address rxAddr) {
        UdpServerHelper server(port);
        auto sApp = server.Install(rxNode);
        sApp.Start(Seconds(tStart));
        sApp.Stop(Seconds(tStop));

        UdpClientHelper client(rxAddr, port);
        client.SetAttribute("MaxPackets", UintegerValue(0xFFFFFFFF));
        client.SetAttribute("Interval",   TimeValue(MicroSeconds(1)));
        client.SetAttribute("PacketSize", UintegerValue(payloadSize));
        auto cApp = client.Install(txNode);
        cApp.Start(Seconds(tStart));
        cApp.Stop(Seconds(tStop));
        ++port;
    };

    // Bursty DL flow (OBSS interference generators).
    // Burst phase is randomised within the 8.3 ms cycle so that concurrent
    // OBSS bursts do not all collide at t=tStart (paper says bursts arrive
    // "approximately every 8.3 ms").
    Ptr<UniformRandomVariable> burstPhase = CreateObject<UniformRandomVariable>();
    burstPhase->SetAttribute("Min", DoubleValue(0.0));
    burstPhase->SetAttribute("Max", DoubleValue(0.0083));
    auto addBurstyFlow = [&](Ptr<Node> apNode, Ptr<Node> staNode, Ipv4Address staAddr) {
        PacketSinkHelper sink("ns3::UdpSocketFactory",
                              InetSocketAddress(Ipv4Address::GetAny(), port));
        auto sApp = sink.Install(staNode);
        sApp.Start(Seconds(tStart));
        sApp.Stop(Seconds(tStop));

        OnOffHelper onoff("ns3::UdpSocketFactory",
                          InetSocketAddress(staAddr, port));
        onoff.SetAttribute("DataRate",   DataRateValue(DataRate("1500Mbps")));
        onoff.SetAttribute("PacketSize", UintegerValue(payloadSize));
        onoff.SetAttribute("OnTime",
            StringValue("ns3::ConstantRandomVariable[Constant=0.0008]"));   // 0.8 ms
        onoff.SetAttribute("OffTime",
            StringValue("ns3::ConstantRandomVariable[Constant=0.0075]"));   // 7.5 ms
        auto cApp = onoff.Install(apNode);
        cApp.Start(Seconds(tStart + burstPhase->GetValue()));
        cApp.Stop(Seconds(tStop));
        ++port;
    };

    // Main BSS: single direction
    if (direction == "DL")
        addSatFlow(mainAp.Get(0),  mainSta.Get(0), mainStaIf.GetAddress(0));
    else
        addSatFlow(mainSta.Get(0), mainAp.Get(0),  mainApIf.GetAddress(0));

    // OBSS0: bursty DL per independent BSS
    for (uint32_t k = 0; k < nObss; ++k)
        addBurstyFlow(obss0Aps.Get(k), obss0Stas.Get(k), obss0StaIfs[k].GetAddress(0));

    // OBSS1: bursty DL per independent BSS (STR/EMLSR only)
    if (isMlo)
        for (uint32_t k = 0; k < nObss; ++k)
            addBurstyFlow(obss1Aps.Get(k), obss1Stas.Get(k), obss1StaIfs[k].GetAddress(0));

    // -----------------------------------------------------------------------
    //  MacRx traces — DL on main STA, UL on main AP
    // -----------------------------------------------------------------------
    {
        uint32_t staId = mainSta.Get(0)->GetId();
        Config::Connect("/NodeList/" + std::to_string(staId) +
                            "/DeviceList/*/$ns3::WifiNetDevice/Mac/MacRx",
                        MakeCallback(&MacRxDl));

        uint32_t apId = mainAp.Get(0)->GetId();
        Config::Connect("/NodeList/" + std::to_string(apId) +
                            "/DeviceList/*/$ns3::WifiNetDevice/Mac/MacRx",
                        MakeCallback(&MacRxUl));
    }

    Simulator::Stop(Seconds(tStop + 1.0));
    Simulator::Run();

    auto toMbps = [&](uint64_t bytes) {
        return (simTime > 0.0) ? (bytes * 8.0) / (simTime * 1e6) : 0.0;
    };
    std::cout << "\nMainBSS DL MacRx: " << toMbps(g_dlBytes) << " Mb/s\n";
    std::cout << "MainBSS UL MacRx: "   << toMbps(g_ulBytes) << " Mb/s\n";

    Simulator::Destroy();
    return 0;
}

