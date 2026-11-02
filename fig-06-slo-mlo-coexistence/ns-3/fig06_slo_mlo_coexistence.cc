/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/multi-model-spectrum-channel.h"
#include "ns3/wifi-phy-band.h"

#include <iostream>
#include <vector>
#include <string>
#include <cstdint>
#include <cmath>
#include <sstream>
#include <algorithm>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("WifiMloCoexistence");

// ============================================================================
//  L3 throughput (FlowMonitor)
// ============================================================================

static double
GetFlowThroughputMbps(Ptr<FlowMonitor> flowmon)
{
    double totalThroughput = 0.0;

    auto stats = flowmon->GetFlowStats();
    for (auto const& p : stats)
    {
        const FlowMonitor::FlowStats& st = p.second;
        if (st.rxPackets == 0)
        {
            continue;
        }

        double tFirst = st.timeFirstRxPacket.GetSeconds();
        double tLast = st.timeLastRxPacket.GetSeconds();
        double duration = tLast - tFirst;
        if (duration <= 0.0)
        {
            continue;
        }

        double thr = (st.rxBytes * 8.0) / (duration * 1e6); // Mb/s
        totalThroughput += thr;
    }

    return totalThroughput;
}

// ============================================================================
//  MAC-level: MacRx (bits received by STA from the protocol stack)
// ============================================================================

// Per-BSS MacRx byte counters: [0]=BSS0 (ch42 SLO), [1]=BSS1 (ch106 SLO), [2]=BSS2 (STR)
static uint64_t g_macRxBytes[3] = {0, 0, 0};

// Per-link PhyTxBegin packet counters for BSS2's AP: [0]=link 0 (ch42), [1]=link 1 (ch106)
static uint64_t g_ap2PhyTx[2]   = {0, 0};
static uint64_t g_ap2PhyTxB[2]  = {0, 0};   // bytes
static uint64_t g_sta2PhyTx[2]  = {0, 0};

static void
MacRxTrace(uint32_t bssIdx, std::string /*context*/, Ptr<const Packet> pkt)
{
    g_macRxBytes[bssIdx] += pkt->GetSize();
}

static void
Ap2PhyTxTrace(uint32_t linkId, std::string /*ctx*/, Ptr<const Packet> pkt, double /*txPowerW*/)
{
    if (linkId < 2) { g_ap2PhyTx[linkId]++; g_ap2PhyTxB[linkId] += pkt->GetSize(); }
}

static void
Sta2PhyTxTrace(uint32_t linkId, std::string /*ctx*/, Ptr<const Packet> pkt, double /*txPowerW*/)
{
    if (linkId < 2) { g_sta2PhyTx[linkId]++; }
}

// ============================================================================

int
main(int argc, char* argv[])
{
    // -----------------------------------------------------------------------
    //  Parameters
    // -----------------------------------------------------------------------
    double      simTime     = 10.0;   // [s]
    uint32_t    payloadSize = 1500;   // [B]
    uint32_t    nMpdus      = 1024;
    std::string mloMode     = "STR";  // BSS2 mode: STR or EMLSR
    // Default: fully saturated downlink in all BSSs (0 = saturated).
    double      bss0LoadMbps = 0.0;
    double      bss1LoadMbps = 0.0;
    double      bss2LoadMbps = 0.0;

    CommandLine cmd(__FILE__);
    cmd.AddValue("simTime",     "Simulation time [s]",         simTime);
    cmd.AddValue("payloadSize", "UDP payload size [B]",         payloadSize);
    cmd.AddValue("nMpdus",      "Max MPDUs per A-MPDU",        nMpdus);
    cmd.AddValue("mloMode",     "BSS2 MLO mode: STR or EMLSR", mloMode);
    cmd.AddValue("bss0LoadMbps", "BSS0 offered load [Mb/s], 0=saturated", bss0LoadMbps);
    cmd.AddValue("bss1LoadMbps", "BSS1 offered load [Mb/s], 0=saturated", bss1LoadMbps);
    cmd.AddValue("bss2LoadMbps", "BSS2 offered load [Mb/s], 0=saturated", bss2LoadMbps);
    cmd.Parse(argc, argv);

    if (mloMode != "STR" && mloMode != "EMLSR")
    {
        NS_ABORT_MSG("mloMode must be STR or EMLSR");
    }

    std::cout << "=== Coexistence Scenario ===\n"
              << "BSS0: SLO   channel 42  (80 MHz)\n"
              << "BSS1: SLO   channel 106 (80 MHz)\n"
              << "BSS2: " << mloMode << "  channels 42 + 106 (80 MHz each)\n"
              << "Offered load [Mb/s]: BSS0=" << bss0LoadMbps
              << " BSS1=" << bss1LoadMbps
              << " BSS2=" << bss2LoadMbps << "\n\n";

    RngSeedManager::SetSeed(1);

    // Paper alignment (Carrascosa 2023 Table I):
    //  - RTS/CTS used for every (A-)MPDU exchange
    //  - AP transmission buffer = 4096 packets
    Config::SetDefault("ns3::WifiRemoteStationManager::RtsCtsThreshold",
                       UintegerValue(0));
    Config::SetDefault("ns3::WifiMacQueue::MaxSize",
                       QueueSizeValue(QueueSize("4096p")));

    NodeContainer apNodes;  apNodes.Create(3);
    NodeContainer staNodes; staNodes.Create(3);

    // -----------------------------------------------------------------------
    //  Spectrum channels — ONE per frequency (ch42 and ch106) so each
    //  frequency has its own MultiModelSpectrumChannel object with its own
    //  event scheduler. Co-channel nodes (BSS0 + BSS2's link 0 on ch42;
    //  BSS1 + BSS2's link 1 on ch106) share the same channel object so they
    //  see each other for CSMA/CA contention. Cross-channel pairs are on
    //  different objects (no spurious serialisation of BSS2's two PHYs).
    //  This mirrors 4.1.1's setup where each link gets its own channel
    //  object — required for STR to actually parallelise the two PHYs.
    // -----------------------------------------------------------------------
    auto makeChannel = []() {
        Ptr<MultiModelSpectrumChannel> ch = CreateObject<MultiModelSpectrumChannel>();
        Ptr<LogDistancePropagationLossModel> loss = CreateObject<LogDistancePropagationLossModel>();
        loss->SetAttribute("Exponent",          DoubleValue(3.5));
        loss->SetAttribute("ReferenceLoss",     DoubleValue(40.0));
        loss->SetAttribute("ReferenceDistance", DoubleValue(1.0));
        ch->AddPropagationLossModel(loss);
        ch->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());
        return ch;
    };
    Ptr<MultiModelSpectrumChannel> specCh42  = makeChannel();
    Ptr<MultiModelSpectrumChannel> specCh106 = makeChannel();

    // Custom FrequencyRange objects so phy2's two links register two DIFFERENT
    // SpectrumChannel objects (one per centre freq). With the default
    // WIFI_SPECTRUM_5_GHZ range, the second AddChannel() call OVERWRITES the
    // first → BSS2 link 0 ends up on the wrong SpectrumChannel and BSS0 sees
    // no OBSS contention from BSS2.
    const FrequencyRange ch42_range  = {MHz_u{5170}, MHz_u{5250}};
    const FrequencyRange ch106_range = {MHz_u{5490}, MHz_u{5570}};

    auto addLink = [&](SpectrumWifiPhyHelper& phy, uint32_t linkId,
                       Ptr<MultiModelSpectrumChannel> ch, uint32_t chanNum,
                       const FrequencyRange& range)
    {
        phy.AddChannel(ch, range);
        std::ostringstream oss;
        oss << "{" << chanNum << ", 80, BAND_5GHZ, 0}";
        phy.Set(linkId, "ChannelSettings",             StringValue(oss.str()));
        phy.Set(linkId, "Antennas",                    UintegerValue(2));
        phy.Set(linkId, "MaxSupportedTxSpatialStreams", UintegerValue(2));
        phy.Set(linkId, "MaxSupportedRxSpatialStreams", UintegerValue(2));
        // Carrascosa 2023 Table I: 20 dBm transmit power
        phy.Set(linkId, "TxPowerStart", DoubleValue(20.0));
        phy.Set(linkId, "TxPowerEnd",   DoubleValue(20.0));
    };

    // BSS0: single link on ch42 → shares ch42 channel with BSS2's link 0
    SpectrumWifiPhyHelper phy0(1);
    phy0.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
    addLink(phy0, 0, specCh42, 42, ch42_range);

    // BSS1: single link on ch106 → shares ch106 channel with BSS2's link 1
    SpectrumWifiPhyHelper phy1(1);
    phy1.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
    addLink(phy1, 0, specCh106, 106, ch106_range);

    // BSS2: two links on DIFFERENT channel objects (ch42 + ch106) using
    // distinct frequency ranges so both registrations survive.
    SpectrumWifiPhyHelper phy2(2);
    phy2.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
    phy2.SetPcapCaptureType(WifiPhyHelper::PcapCaptureType::PCAP_PER_LINK);
    addLink(phy2, 0, specCh42,  42,  ch42_range);
    addLink(phy2, 1, specCh106, 106, ch106_range);

    // -----------------------------------------------------------------------
    //  WiFi helpers — one per link-count to keep RSM count == link count.
    // -----------------------------------------------------------------------

    // BSS0 & BSS1: single-link, 1 RSM
    WifiHelper wifi01;
    wifi01.SetStandard(WIFI_STANDARD_80211be);
    wifi01.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
    wifi01.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                   "DataMode",    StringValue("EhtMcs8"),
                                   "ControlMode", StringValue("OfdmRate24Mbps"));

    // BSS2: two-link (STR or EMLSR), 2 RSMs
    WifiHelper wifi2;
    wifi2.SetStandard(WIFI_STANDARD_80211be);
    wifi2.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
    for (uint8_t l = 0; l < 2; ++l)
    {
        wifi2.SetRemoteStationManager(l, "ns3::ConstantRateWifiManager",
                                      "DataMode",    StringValue("EhtMcs8"),
                                      "ControlMode", StringValue("OfdmRate24Mbps"));
    }
    if (mloMode == "EMLSR")
    {
        wifi2.ConfigEhtOptions("EmlsrActivated",     BooleanValue(true));
        wifi2.ConfigEhtOptions("TransitionTimeout",  TimeValue(MicroSeconds(1024)));
        // ns-3 default MsdMaxNTxops = 1 caps EMLSR to *one* TXOP during the
        // 5.5 ms MediumSyncDelay window, which throttles steady-state EMLSR
        // throughput severely. Disable that cap and align the other MSD
        // parameters with the 4.1.1 baseline.
        wifi2.ConfigEhtOptions("MediumSyncDuration", TimeValue(MicroSeconds(0)));     // disable MSD timer
        wifi2.ConfigEhtOptions("MsdOfdmEdThreshold", IntegerValue(-72));
        wifi2.ConfigEhtOptions("MsdMaxNTxops",       UintegerValue(0));   // unlimited attempts
    }

    WifiMacHelper wifiMac;
    // Keep SU behavior neutral for 1 STA per BSS and avoid MU scheduler side-effects.

    // Convenience lambdas to set AP / STA MAC type
    auto setApMac = [&](const char* ssidStr) {
        wifiMac.SetType("ns3::ApWifiMac",
                        "Ssid",            SsidValue(Ssid(ssidStr)),
                        "BE_MaxAmpduSize", UintegerValue(6500631),
                        "MpduBufferSize",  UintegerValue(nMpdus));
    };
    auto setStaMac = [&](const char* ssidStr) {
        wifiMac.SetType("ns3::StaWifiMac",
                        "Ssid",            SsidValue(Ssid(ssidStr)),
                        "ActiveProbing",   BooleanValue(false),
                        "BE_MaxAmpduSize", UintegerValue(6500631),
                        "MpduBufferSize",  UintegerValue(nMpdus));
    };

    // -----------------------------------------------------------------------
    //  Install devices — each BSS gets its own SSID
    // -----------------------------------------------------------------------

    // BSS0
    setApMac("bss-0");
    NetDeviceContainer apDev0  = wifi01.Install(phy0, wifiMac, apNodes.Get(0));
    setStaMac("bss-0");
    NetDeviceContainer staDev0 = wifi01.Install(phy0, wifiMac, staNodes.Get(0));

    // BSS1
    setApMac("bss-1");
    NetDeviceContainer apDev1  = wifi01.Install(phy1, wifiMac, apNodes.Get(1));
    setStaMac("bss-1");
    NetDeviceContainer staDev1 = wifi01.Install(phy1, wifiMac, staNodes.Get(1));

    // BSS2 – STR or EMLSR (EMLSR options already set on wifi2 above)
    setApMac("bss-2");
    NetDeviceContainer apDev2  = wifi2.Install(phy2, wifiMac, apNodes.Get(2));
    if (mloMode == "EMLSR")
    {
        wifiMac.SetEmlsrManager("ns3::AdvancedEmlsrManager",
                                "EmlsrLinkSet",         StringValue("0,1"),
                                "SwitchAuxPhy",         BooleanValue(true),
                                "PutAuxPhyToSleep",     BooleanValue(false),
                                "AuxPhyChannelWidth",   UintegerValue(80),
                                "AuxPhyMaxModClass",    StringValue("EHT"),
                                "AuxPhyTxCapable",      BooleanValue(true));
    }
    setStaMac("bss-2");
    NetDeviceContainer staDev2 = wifi2.Install(phy2, wifiMac, staNodes.Get(2));

    // STR link balancing: ns-3's default link selector preferentially uses
    // link 1 (ch106) for downlink, leaving link 0 (ch42) under-utilised and
    // producing asymmetric OBSS contention. Map all TIDs to BOTH links on
    // both AP and STA so that the BlockAck/QosTxop scheduler distributes
    // packets across the two links evenly.
    if (mloMode == "STR")
    {
        std::map<std::list<uint8_t>, std::list<uint8_t>> evenMap = {
            {{0,1,2,3,4,5,6,7}, {0,1}}};
        for (uint32_t i = 0; i < apDev2.GetN(); ++i)
        {
            Ptr<WifiNetDevice> d = DynamicCast<WifiNetDevice>(apDev2.Get(i));
            d->GetEhtConfiguration()->SetTidLinkMapping(WifiDirection::DOWNLINK, evenMap);
            d->GetEhtConfiguration()->SetTidLinkMapping(WifiDirection::UPLINK,   evenMap);
        }
        for (uint32_t i = 0; i < staDev2.GetN(); ++i)
        {
            Ptr<WifiNetDevice> d = DynamicCast<WifiNetDevice>(staDev2.Get(i));
            d->GetEhtConfiguration()->SetTidLinkMapping(WifiDirection::DOWNLINK, evenMap);
            d->GetEhtConfiguration()->SetTidLinkMapping(WifiDirection::UPLINK,   evenMap);
        }
    }

    // -----------------------------------------------------------------------
    //  Mobility – equilateral triangle (10 m side), each STA 5 m east of its AP.
    //  AP2 (BSS2) sits at the apex, equidistant from AP0 and AP1,
    //  maximising co-channel exposure on both links.
    //
    //        AP2 (5, 8.66)  STA2 (10, 8.66)
    //       /           \
    //   AP0 (0,0)      AP1 (10,0)
    //   STA0(5,0)      STA1(15,0)
    // -----------------------------------------------------------------------
    MobilityHelper mobility;
    Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator>();
    // APs
    pos->Add(Vector( 0.0,  0.0,  0.0)); // AP0  (SLO  ch42)
    pos->Add(Vector(10.0,  0.0,  0.0)); // AP1  (SLO  ch106)
    pos->Add(Vector( 5.0,  8.66, 0.0)); // AP2  (STR/EMLSR)
    // STAs – 5 m east of their AP
    pos->Add(Vector( 5.0,  0.0,  0.0)); // STA0
    pos->Add(Vector(15.0,  0.0,  0.0)); // STA1
    pos->Add(Vector(10.0,  8.66, 0.0)); // STA2
    mobility.SetPositionAllocator(pos);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(apNodes);
    mobility.Install(staNodes);

    // -----------------------------------------------------------------------
    //  Internet stack + IP addressing (separate /24 per BSS)
    // -----------------------------------------------------------------------
    InternetStackHelper stack;
    stack.Install(apNodes);
    stack.Install(staNodes);

    Ipv4AddressHelper addr;

    addr.SetBase("10.1.0.0", "255.255.255.0");
    Ipv4InterfaceContainer apIf0  = addr.Assign(apDev0);
    Ipv4InterfaceContainer staIf0 = addr.Assign(staDev0);

    addr.SetBase("10.2.0.0", "255.255.255.0");
    Ipv4InterfaceContainer apIf1  = addr.Assign(apDev1);
    Ipv4InterfaceContainer staIf1 = addr.Assign(staDev1);

    addr.SetBase("10.3.0.0", "255.255.255.0");
    Ipv4InterfaceContainer apIf2  = addr.Assign(apDev2);
    Ipv4InterfaceContainer staIf2 = addr.Assign(staDev2);

    // No global routing needed: every flow is single-hop within its own /24.
    // Subnet routes are installed automatically by Ipv4AddressHelper::Assign().

    // -----------------------------------------------------------------------
    //  Traffic: downlink UDP AP -> STA (paced if load > 0, saturated if load == 0).
    //  One flow per BSS keeps link usage emergent from MLO/EMLSR itself.
    // -----------------------------------------------------------------------
    double   tStart = 1.0;
    double   tStop  = simTime + 1.0;
    uint16_t port   = 5000;

    auto addFlow = [&](Ptr<Node> apNode, Ptr<Node> staNode, Ipv4Address staAddr, double loadMbps) {
        UdpServerHelper server(port);
        auto sApp = server.Install(staNode);
        sApp.Start(Seconds(tStart));
        sApp.Stop(Seconds(tStop));

        UdpClientHelper client(staAddr, port);
        client.SetAttribute("MaxPackets", UintegerValue(0xFFFFFFFF));
        if (loadMbps <= 0.0)
        {
            client.SetAttribute("Interval", TimeValue(MicroSeconds(1)));
        }
        else
        {
            const double pktBits = static_cast<double>(payloadSize) * 8.0;
            const double intervalUs = pktBits / loadMbps;
            client.SetAttribute("Interval", TimeValue(MicroSeconds(std::max(1.0, intervalUs))));
        }
        client.SetAttribute("PacketSize", UintegerValue(payloadSize));
        auto cApp = client.Install(apNode);
        cApp.Start(Seconds(tStart));
        cApp.Stop(Seconds(tStop));
        ++port;
    };

    addFlow(apNodes.Get(0), staNodes.Get(0), staIf0.GetAddress(0), bss0LoadMbps); // BSS0
    addFlow(apNodes.Get(1), staNodes.Get(1), staIf1.GetAddress(0), bss1LoadMbps); // BSS1
    addFlow(apNodes.Get(2), staNodes.Get(2), staIf2.GetAddress(0), bss2LoadMbps); // BSS2

    // -----------------------------------------------------------------------
    //  FlowMonitor + per-BSS MacRx traces
    // -----------------------------------------------------------------------
    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor> flowmon = fmHelper.InstallAll();

    for (uint32_t i = 0; i < 3; ++i)
    {
        uint32_t id = staNodes.Get(i)->GetId();
        Config::Connect("/NodeList/" + std::to_string(id) +
                        "/DeviceList/*/$ns3::WifiNetDevice/Mac/MacRx",
                        MakeBoundCallback(&MacRxTrace, i));
    }

    // Per-link PhyTxBegin diagnostic for BSS2 AP and STA
    uint32_t ap2Id  = apNodes.Get(2)->GetId();
    uint32_t sta2Id = staNodes.Get(2)->GetId();
    for (uint32_t link = 0; link < 2; ++link)
    {
        Config::Connect("/NodeList/" + std::to_string(ap2Id) +
                        "/DeviceList/*/$ns3::WifiNetDevice/Phys/" +
                        std::to_string(link) + "/PhyTxBegin",
                        MakeBoundCallback(&Ap2PhyTxTrace, link));
        Config::Connect("/NodeList/" + std::to_string(sta2Id) +
                        "/DeviceList/*/$ns3::WifiNetDevice/Phys/" +
                        std::to_string(link) + "/PhyTxBegin",
                        MakeBoundCallback(&Sta2PhyTxTrace, link));
    }

    Simulator::Stop(Seconds(tStop + 1.0));
    Simulator::Run();

    // -----------------------------------------------------------------------
    //  Results
    // -----------------------------------------------------------------------
    auto mbps = [&](uint32_t i) {
        return (simTime > 0.0) ? (g_macRxBytes[i] * 8.0) / (simTime * 1e6) : 0.0;
    };

    std::cout << "\n=== Throughput results ===\n";
    std::cout << "MacRx BSS0 (SLO  ch42):       " << mbps(0) << " Mb/s\n";
    std::cout << "MacRx BSS1 (SLO  ch106):      " << mbps(1) << " Mb/s\n";
    std::cout << "MacRx BSS2 (" << mloMode << " ch42+ch106): " << mbps(2) << " Mb/s\n";
    std::cout << "MacRx total:                  " << mbps(0)+mbps(1)+mbps(2) << " Mb/s\n";
    std::cout << "L3 total (FlowMonitor):       "
              << GetFlowThroughputMbps(flowmon) << " Mb/s\n";
    std::cout << "\n=== BSS2 per-link PHY TX (diagnostic) ===\n";
    std::cout << "AP2  PhyTx link0 (ch42):  " << g_ap2PhyTx[0]
              << " pkts, " << (g_ap2PhyTxB[0]*8.0/(simTime*1e6)) << " Mb/s on air\n";
    std::cout << "AP2  PhyTx link1 (ch106): " << g_ap2PhyTx[1]
              << " pkts, " << (g_ap2PhyTxB[1]*8.0/(simTime*1e6)) << " Mb/s on air\n";
    std::cout << "STA2 PhyTx link0 (ch42):  " << g_sta2PhyTx[0] << " pkts (ACKs/BAs)\n";
    std::cout << "STA2 PhyTx link1 (ch106): " << g_sta2PhyTx[1] << " pkts (ACKs/BAs)\n";

    Simulator::Destroy();
    return 0;
}
