/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Latency Scenario #5 - END-TO-END (FlowMonitor) variant
 *
 * Copy of wifi-mlo-latency-scenario5.cc measuring end-to-end UDP delay
 * (IP Tx -> IP Rx) instead of channel-access delay (MacTx -> PhyTxBegin).
 * Same output line schema.  (Section 3.2.11 / Scenario 4.2.6)
 *
 * Based on [17]: Carrascosa-Zamacois et al.,
 *   "Wi-Fi multi-link operation: An experimental study of latency and throughput",
 *   IEEE/ACM Transactions on Networking 32.1 (2023), pp. 308–322.
 *
 * Goal: Assess how different channel occupancy levels influence the latency
 * of MLO-STR vs SLO, under symmetric and asymmetric OBSS interference.
 *
 * ── Topology ────────────────────────────────────────────────────────────────
 *   Main BSS : 1 AP + 1 STA, DL
 *              SLO  → single link on ch0 (the less-congested channel)
 *              MLO-STR → 2 links: ch0 + ch1
 *   OBSS-1   : 1 AP + numObssStas1 STAs, single-link on ch0  (always present
 *              when numObssStas1 > 0; occupies the primary channel)
 *   OBSS-2   : 1 AP + numObssStas2 STAs, single-link on ch1  (created only
 *              when numLinks==2 and numObssStas2 > 0)
 *
 * ── Channel plan (5 GHz, 80 MHz each) ──────────────────────────────────────
 *   ch0 : 5 GHz centre channel 42  (~5210 MHz  →  "ch36"  region in thesis)
 *   ch1 : 5 GHz centre channel 106 (~5530 MHz  →  "ch100" region in thesis)
 *
 * ── PHY / MAC ───────────────────────────────────────────────────────────────
 *   Standard  : 802.11be (EHT)
 *   Data mode : EhtMcs11,  2 SS,  GI 800 ns,  80 MHz
 *   A-MPDU    : nMpdus (default 1024) — Table 3.12
 *
 * ── Traffic ─────────────────────────────────────────────────────────────────
 *   Main BSS  : DL OnOff at offeredLoad Mb/s (Poisson arrivals, port 5000)
 *   OBSS-k    : DL OnOff per OBSS STA at obssRatePerSta Mb/s (port 5001/5002)
 *                 Default obssRatePerSta = 111.4 Mb/s  ≈ 10 % of single-link
 *                 capacity (1114 Mb/s from baseline Table 4.1).
 *                 1 STA → 10 %,  4 STAs → 40 %,  7 STAs → 70 %  occupancy.
 *
 * ── Output lines parsed by run_all_rerun.py ─────────────────────────────────
 *   "Mean DL Latency: X.XXX ms"
 *   "DL Latency p50: X.XXX ms"
 *   "DL Latency p95: X.XXX ms"
 *   "DL Latency p99: X.XXX ms"
 *   "Mean DL Throughput: X.XX Mb/s"
 *
 * ── Example invocation ──────────────────────────────────────────────────────
 *   ./ns3 run "scratch/fig19_20_channel_occupancy \
 *       --numLinks=2 --numObssStas1=4 --numObssStas2=4 \
 *       --offeredLoad=371 --RngRun=1"
 */

#include "../_common/e2e-arp.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-helper.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-flow-classifier.h"
#include "ns3/mobility-module.h"
#include "ns3/multi-model-spectrum-channel.h"
#include "ns3/network-module.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/wifi-module.h"
#include "ns3/wifi-phy-band.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("WifiMloLatencyScenario5E2e");

// === End-to-end (FlowMonitor) delay helpers ===
namespace
{
// Linear-interpolation percentile from aggregated per-bin counts.
// `binCounts` are bin frequencies, all bins of width `binWidth` seconds.
// p in [0..100], returned value in seconds.
double
PercentileFromBins(const std::vector<uint64_t>& binCounts, double binWidth, double p)
{
    uint64_t total = 0;
    for (uint64_t c : binCounts)
        total += c;
    if (total == 0)
        return 0.0;

    const double target = (p / 100.0) * static_cast<double>(total);
    uint64_t cum = 0;
    for (size_t i = 0; i < binCounts.size(); ++i)
    {
        const uint64_t c = binCounts[i];
        if (c == 0)
            continue;
        if (static_cast<double>(cum + c) >= target)
        {
            const double frac = (target - static_cast<double>(cum)) /
                                static_cast<double>(c);
            return (static_cast<double>(i) + frac) * binWidth;
        }
        cum += c;
    }
    for (size_t i = binCounts.size(); i-- > 0;)
    {
        if (binCounts[i] > 0)
            return static_cast<double>(i + 1) * binWidth;
    }
    return 0.0;
}
} // namespace
// === end end-to-end delay helpers ===

// ---------------------------------------------------------------------------
//  Main
// ---------------------------------------------------------------------------
int
main(int argc, char* argv[])
{
    // -----------------------------------------------------------------------
    //  Parameters
    // -----------------------------------------------------------------------
    double   simTime        = 10.0;   // s
    uint32_t payloadSize    = 1500;   // bytes
    uint32_t nMpdus         = 1024;   // A-MPDU limit
    double   offeredLoad    = 370.0;  // Mb/s  (main BSS DL)
    uint32_t numLinks       = 2;      // 1 = SLO,  2 = MLO-STR
    uint32_t numObssStas1   = 1;      // OBSS-1 stations on ch0
    uint32_t numObssStas2   = 1;      // OBSS-2 stations on ch1 (MLO only)
    double   obssRatePerSta = 10.0;   // Mb/s per OBSS STA — 10 % of single-link
                                      // 20 MHz / MCS 9 / 2 SS goodput (~100 Mb/s)
                                      // matches Carrascosa TON [17] 20 MHz setup.
    double   startupGuard   = 2.0;    // s idle before traffic starts
    double   drainTime      = 1.0;
    uint32_t maxMacQueuePackets = 4096;
    double   maxMacQueueDelayMs = 500.0;

    CommandLine cmd(__FILE__);
    cmd.AddValue("simTime",        "Simulation time [s]",                       simTime);
    cmd.AddValue("payloadSize",    "UDP payload size [B]",                       payloadSize);
    cmd.AddValue("nMpdus",         "Max MPDUs per A-MPDU",                      nMpdus);
    cmd.AddValue("offeredLoad",    "Main-BSS DL offered load [Mb/s]",           offeredLoad);
    cmd.AddValue("numLinks",       "Main BSS links: 1=SLO, 2=MLO-STR",         numLinks);
    cmd.AddValue("numObssStas1",   "OBSS-1 STAs on ch0 (primary channel)",     numObssStas1);
    cmd.AddValue("numObssStas2",   "OBSS-2 STAs on ch1 (secondary; MLO only)", numObssStas2);
    cmd.AddValue("obssRatePerSta", "DL rate per OBSS STA [Mb/s]",              obssRatePerSta);
    cmd.AddValue("startupGuard",   "Idle before traffic starts [s]",           startupGuard);
    cmd.AddValue("drainTime",      "Receiver-only drain interval [s]",         drainTime);
    cmd.AddValue("maxMacQueuePackets", "Max Wi-Fi MAC queue size [packets]", maxMacQueuePackets);
    cmd.AddValue("maxMacQueueDelayMs", "Max Wi-Fi MAC queue residence time [ms]", maxMacQueueDelayMs);
    cmd.Parse(argc, argv);

    // 100 us bin width gives good resolution for the [0, ~1 s] delay
    // range used by FlowMonitor's delayHistogram.
    Config::SetDefault("ns3::FlowMonitor::DelayBinWidth", DoubleValue(0.0001));

    NS_ABORT_MSG_IF(numLinks != 1 && numLinks != 2,
                    "numLinks must be 1 (SLO) or 2 (MLO-STR)");
    NS_ABORT_MSG_IF(offeredLoad <= 0.0, "offeredLoad must be > 0");
    NS_ABORT_MSG_IF(startupGuard < 0.0 || drainTime < 0.0,
                    "startupGuard and drainTime must be non-negative");
    NS_ABORT_MSG_IF(maxMacQueuePackets == 0 || maxMacQueueDelayMs <= 0.0,
                    "Wi-Fi MAC queue limits must be positive");

    // For SLO, OBSS-2 (ch1) is irrelevant — force to 0 so no extra nodes are created.
    if (numLinks == 1)
        numObssStas2 = 0;

    RngSeedManager::SetSeed(1); // run index controlled by --RngRun

    std::cout << "=== Latency Scenario #5 ===\n"
              << "numLinks=" << numLinks
              << "  offeredLoad=" << offeredLoad << " Mb/s"
              << "  OBSS1=" << numObssStas1 << " stas"
              << "  OBSS2=" << numObssStas2 << " stas"
              << "  obssRate=" << obssRatePerSta << " Mb/s/sta\n\n";

    // -----------------------------------------------------------------------
    //  Two 5 GHz spectrum channels (80 MHz each)
    //    ch0: centre 42  (~5210 MHz)  → "channel 36"  in thesis (Table 3.12)
    //    ch1: centre 106 (~5530 MHz)  → "channel 100" in thesis
    // -----------------------------------------------------------------------
    struct PhyChan
    {
        uint32_t       chanNum; // ns-3 centre channel number
        FrequencyRange range;
        const char*    band;
    };
    // Thesis Table 3.12: channels 36 / 100 at a channel width of 80 MHz.
    // ns-3 identifies an 80 MHz channel by its CENTRE number, so the thesis's
    // "channel 36" / "channel 100" 80 MHz blocks are ch42 / ch106 -- the same
    // pair the 4.1.1 baseline uses. (Previously 36/100 at 20 MHz, following
    // Carrascosa's 20 MHz FCB-WACA testbed; that capped this scenario at
    // ~275 Mb/s instead of the baseline's ~1116 Mb/s -- an exact 4x.)
    const PhyChan phyChans[2] = {
        {  42, WIFI_SPECTRUM_5_GHZ, "BAND_5GHZ" }, // ch0 — thesis "channel 36"
        { 106, WIFI_SPECTRUM_5_GHZ, "BAND_5GHZ" }, // ch1 — thesis "channel 100"
    };
    constexpr uint16_t CHANNEL_WIDTH_MHZ = 80;     // thesis Table 3.12

    // Shared spectrum medium per channel: all nodes on the same medium contend.
    Ptr<MultiModelSpectrumChannel> specMedia[2];
    for (int i = 0; i < 2; ++i)
    {
        specMedia[i] = CreateObject<MultiModelSpectrumChannel>();
        // Residential indoor propagation per thesis Table 3.1 (LogDistance gamma=3.5, refLoss=40 dB @ 1 m).
        Ptr<LogDistancePropagationLossModel> loss = CreateObject<LogDistancePropagationLossModel>();
        loss->SetAttribute("Exponent",          DoubleValue(3.5));
        loss->SetAttribute("ReferenceDistance", DoubleValue(1.0));
        loss->SetAttribute("ReferenceLoss",     DoubleValue(40.0));
        specMedia[i]->AddPropagationLossModel(loss);
        specMedia[i]->SetPropagationDelayModel(
            CreateObject<ConstantSpeedPropagationDelayModel>());
    }

    // -----------------------------------------------------------------------
    //  MAC queue size
    // -----------------------------------------------------------------------
    std::ostringstream queueSize;
    queueSize << maxMacQueuePackets << "p";
    Config::SetDefault("ns3::WifiMacQueue::MaxSize", QueueSizeValue(QueueSize(queueSize.str())));
    Config::SetDefault("ns3::WifiMacQueue::MaxDelay", TimeValue(MilliSeconds(maxMacQueueDelayMs)));
    Config::SetDefault("ns3::ArpCache::PendingQueueSize", UintegerValue(maxMacQueuePackets));

    // -----------------------------------------------------------------------
    //  WiFi helpers — EHT 802.11be, MCS 11, 2 SS, GI 800 ns
    //  (Two helpers: one for the main BSS, one for OBSS single-link nodes)
    // -----------------------------------------------------------------------
    // Carrascosa TON [17] §II-A: "we employ a fixed modulation and coding
    // scheme on both interfaces — 256-QAM with coding rate 5/6 and 2 spatial
    // streams" → EHT MCS 9.
    // Thesis Table 3.12 / §3.2.11: "MCS 11 and the MPDU aggregation limit
    // of 1024". This file's own header also documents EhtMcs11; the previous
    // EhtMcs9 followed Carrascosa's testbed rather than the simulated scenario.
    const char* DATA_MODE = "EhtMcs11";
    const char* CTL_MODE  = "EhtMcs0";

    auto configWifi = [&](WifiHelper& wifi, uint32_t links) {
        wifi.SetStandard(WIFI_STANDARD_80211be);
        wifi.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
        for (uint32_t l = 0; l < links; ++l)
            wifi.SetRemoteStationManager(l,
                                         "ns3::ConstantRateWifiManager",
                                         "DataMode",    StringValue(DATA_MODE),
                                         "ControlMode", StringValue(CTL_MODE));
    };

    WifiHelper wifiMain, wifiObss;
    configWifi(wifiMain, numLinks);
    configWifi(wifiObss, 1);

    // -----------------------------------------------------------------------
    //  Main BSS nodes + WiFi installation
    // -----------------------------------------------------------------------
    NodeContainer mainApNode, mainStaNode;
    mainApNode.Create(1);
    mainStaNode.Create(1);

    NetDeviceContainer mainApDev, mainStaDev;
    {
        SpectrumWifiPhyHelper phy(numLinks);
        phy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
        if (numLinks > 1)
            phy.SetPcapCaptureType(WifiPhyHelper::PcapCaptureType::PCAP_PER_LINK);

        for (uint32_t l = 0; l < numLinks; ++l)
        {
            std::ostringstream chSet;
            chSet << "{" << phyChans[l].chanNum << ", " << CHANNEL_WIDTH_MHZ << ", " << phyChans[l].band << ", 0}";
            phy.AddChannel(specMedia[l], phyChans[l].range);
            phy.Set(l, "ChannelSettings",             StringValue(chSet.str()));
            phy.Set(l, "Antennas",                    UintegerValue(2));
            phy.Set(l, "MaxSupportedTxSpatialStreams", UintegerValue(2));
            phy.Set(l, "MaxSupportedRxSpatialStreams", UintegerValue(2));
        }

        Ssid mainSsid("MAIN");
        WifiMacHelper mac;

        mac.SetType("ns3::ApWifiMac",
                    "Ssid",            SsidValue(mainSsid),
                    "BE_MaxAmpduSize", UintegerValue(nMpdus * (payloadSize + 60)),
                    "MpduBufferSize",  UintegerValue(nMpdus));
        mainApDev = wifiMain.Install(phy, mac, mainApNode);

        mac.SetType("ns3::StaWifiMac",
                    "Ssid",            SsidValue(mainSsid),
                    "ActiveProbing",   BooleanValue(false),
                    "BE_MaxAmpduSize", UintegerValue(nMpdus * (payloadSize + 60)),
                    "MpduBufferSize",  UintegerValue(nMpdus));
        mainStaDev = wifiMain.Install(phy, mac, mainStaNode);
    }

    // -----------------------------------------------------------------------
    //  OBSS groups
    //  Each OBSS occupies one physical channel with a single-link BSS
    //  (1 AP + numObssStasN STAs, each STA receiving DL traffic).
    // -----------------------------------------------------------------------
    struct ObssGroup
    {
        uint32_t    numStas;
        int         chanIdx;
        uint16_t    port; // UdpServer port on OBSS STAs
        NodeContainer  ap;
        NodeContainer  stas;
        NetDeviceContainer apDev;
        NetDeviceContainer staDev;
        std::vector<Ipv4Address> staAddrs;
    };

    // To produce the airtime occupancy the Carrascosa TON [17] paper assumes,
    // each "OBSS station" needs to be an INDEPENDENT BSS (one AP + one STA),
    // not all 7 hanging off the same AP. With N independent OBSS APs all
    // contending via CSMA, the main BSS gets ~1/(N+1) of the airtime — which
    // matches the paper's per-occupancy SLO max much more closely.
    std::vector<ObssGroup> obssGroups;
    for (uint32_t i = 0; i < numObssStas1; ++i)
        obssGroups.push_back({1, 0,
                              static_cast<uint16_t>(5001 + i),
                              {}, {}, {}, {}, {}});
    for (uint32_t i = 0; i < numObssStas2; ++i)
        obssGroups.push_back({1, 1,
                              static_cast<uint16_t>(5100 + i),
                              {}, {}, {}, {}, {}});

    uint32_t obssIdx = 0;
    for (auto& og : obssGroups)
    {
        og.ap.Create(1);
        og.stas.Create(og.numStas);

        SpectrumWifiPhyHelper phy(1);
        phy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);

        int ci = og.chanIdx;
        std::ostringstream chSet;
        chSet << "{" << phyChans[ci].chanNum << ", " << CHANNEL_WIDTH_MHZ << ", " << phyChans[ci].band << ", 0}";
        phy.AddChannel(specMedia[ci], phyChans[ci].range);
        phy.Set(0, "ChannelSettings",             StringValue(chSet.str()));
        phy.Set(0, "Antennas",                    UintegerValue(2));
        phy.Set(0, "MaxSupportedTxSpatialStreams", UintegerValue(2));
        phy.Set(0, "MaxSupportedRxSpatialStreams", UintegerValue(2));

        std::ostringstream ssidStr;
        ssidStr << "OBSS" << obssIdx++;
        Ssid ssid(ssidStr.str());

        WifiMacHelper mac;
        mac.SetType("ns3::ApWifiMac",
                    "Ssid",            SsidValue(ssid),
                    "BE_MaxAmpduSize", UintegerValue(nMpdus * (payloadSize + 60)),
                    "MpduBufferSize",  UintegerValue(nMpdus));
        og.apDev = wifiObss.Install(phy, mac, og.ap);

        mac.SetType("ns3::StaWifiMac",
                    "Ssid",            SsidValue(ssid),
                    "ActiveProbing",   BooleanValue(true),
                    "BE_MaxAmpduSize", UintegerValue(nMpdus * (payloadSize + 60)),
                    "MpduBufferSize",  UintegerValue(nMpdus));
        og.staDev = wifiObss.Install(phy, mac, og.stas);
    }

    // -----------------------------------------------------------------------
    //  Mobility: all nodes co-located for mutual interference.
    //  AP at origin (0,0,0), STA 5 m away on x-axis.
    // -----------------------------------------------------------------------
    MobilityHelper mob;
    mob.SetMobilityModel("ns3::ConstantPositionMobilityModel");

    auto placeAt = [&](NodeContainer& nodes, Vector pos) {
        Ptr<ListPositionAllocator> alloc = CreateObject<ListPositionAllocator>();
        for (uint32_t i = 0; i < nodes.GetN(); ++i)
            alloc->Add(pos);
        mob.SetPositionAllocator(alloc);
        mob.Install(nodes);
    };

    placeAt(mainApNode,  Vector(0.0, 0.0, 0.0));
    placeAt(mainStaNode, Vector(5.0, 0.0, 0.0));
    for (auto& og : obssGroups)
    {
        placeAt(og.ap,   Vector(0.0, 0.0, 0.0));
        placeAt(og.stas, Vector(5.0, 0.0, 0.0));
    }

    // -----------------------------------------------------------------------
    //  Internet stack
    // -----------------------------------------------------------------------
    InternetStackHelper stack;
    stack.Install(mainApNode);
    stack.Install(mainStaNode);
    for (auto& og : obssGroups)
    {
        stack.Install(og.ap);
        stack.Install(og.stas);
    }

    // -----------------------------------------------------------------------
    //  IP addressing
    //  Main BSS : 10.1.0.0/24
    //  OBSS-1   : 10.2.0.0/24
    //  OBSS-2   : 10.3.0.0/24
    // -----------------------------------------------------------------------
    Ipv4AddressHelper addr;

    addr.SetBase("10.1.0.0", "255.255.255.0");
    Ipv4InterfaceContainer mainApIf = addr.Assign(mainApDev);
    Ipv4InterfaceContainer mainStaIf = addr.Assign(mainStaDev);
    PopulateE2eArp(mainApIf, mainStaIf);
    WarmUpE2e(mainApIf, mainStaIf, startupGuard, true);
    Ipv4Address mainStaAddr = mainStaIf.GetAddress(0);

    addr.NewNetwork();
    for (auto& og : obssGroups)
    {
        Ipv4InterfaceContainer apIf = addr.Assign(og.apDev);
        Ipv4InterfaceContainer staIf = addr.Assign(og.staDev);
        PopulateE2eArp(apIf, staIf);
        WarmUpE2e(apIf, staIf, startupGuard, true);
        for (uint32_t s = 0; s < og.numStas; ++s)
            og.staAddrs.push_back(staIf.GetAddress(s));
        addr.NewNetwork();
    }

    // -----------------------------------------------------------------------
    //  Traffic
    // -----------------------------------------------------------------------
    const double tStart        = startupGuard;
    const double tStop         = simTime + startupGuard;
    const double burstDataRate = 5.0e9;           // >> all offered loads → one pkt per on-period
    const double pktBits       = payloadSize * 8.0;
    const double pktOnSec      = pktBits / burstDataRate; // ~2.4 µs

    std::ostringstream drStr;
    drStr << static_cast<uint64_t>(burstDataRate) << "bps";

    // ── Main BSS DL (AP → STA, port 5000) ──────────────────────────────────
    {
        const double iatSec    = pktBits / (offeredLoad * 1e6);
        const double offMean   = iatSec - pktOnSec;
        NS_ABORT_MSG_IF(offMean <= 0.0,
                        "offeredLoad exceeds burst data rate — reduce load");

        std::ostringstream onStr, offStr;
        onStr  << "ns3::ConstantRandomVariable[Constant=" << pktOnSec  << "]";
        offStr << "ns3::ExponentialRandomVariable[Mean="  << offMean   << "]";

        OnOffHelper onoff("ns3::UdpSocketFactory",
                          InetSocketAddress(mainStaAddr, 5000));
        onoff.SetAttribute("DataRate",   StringValue(drStr.str()));
        onoff.SetAttribute("PacketSize", UintegerValue(payloadSize));
        onoff.SetAttribute("OnTime",     StringValue(onStr.str()));
        onoff.SetAttribute("OffTime",    StringValue(offStr.str()));

        ApplicationContainer apps = onoff.Install(mainApNode);
        apps.Start(Seconds(tStart));
        apps.Stop(Seconds(tStop));

        UdpServerHelper srv(5000);
        ApplicationContainer srvApps = srv.Install(mainStaNode);
        srvApps.Start(Seconds(tStart));
        srvApps.Stop(Seconds(tStop + drainTime));
    }

    // ── OBSS DL (each OBSS AP → its STAs, port 5001/5002) ──────────────────
    for (auto& og : obssGroups)
    {
        const double obssIat  = pktBits / (obssRatePerSta * 1e6);
        const double obssOff  = obssIat - pktOnSec;
        NS_ABORT_MSG_IF(obssOff <= 0.0,
                        "obssRatePerSta exceeds burst data rate");

        std::ostringstream onStr, offStr;
        onStr  << "ns3::ConstantRandomVariable[Constant=" << pktOnSec << "]";
        offStr << "ns3::ExponentialRandomVariable[Mean="  << obssOff  << "]";

        // One UdpServer per OBSS STA
        UdpServerHelper srv(og.port);
        ApplicationContainer srvApps = srv.Install(og.stas);
        srvApps.Start(Seconds(tStart));
        srvApps.Stop(Seconds(tStop + drainTime));

        // One OnOff flow from the OBSS AP to each of its STAs
        for (uint32_t s = 0; s < og.numStas; ++s)
        {
            OnOffHelper onoff("ns3::UdpSocketFactory",
                              InetSocketAddress(og.staAddrs[s], og.port));
            onoff.SetAttribute("DataRate",   StringValue(drStr.str()));
            onoff.SetAttribute("PacketSize", UintegerValue(payloadSize));
            onoff.SetAttribute("OnTime",     StringValue(onStr.str()));
            onoff.SetAttribute("OffTime",    StringValue(offStr.str()));

            ApplicationContainer apps = onoff.Install(og.ap);
            apps.Start(Seconds(tStart));
            apps.Stop(Seconds(tStop));
        }
    }

    // -----------------------------------------------------------------------
    //  FlowMonitor — end-to-end UDP delay + throughput
    // -----------------------------------------------------------------------
    FlowMonitorHelper flowHelper;
    Ptr<FlowMonitor> flowMon = flowHelper.InstallAll();

    // -----------------------------------------------------------------------
    //  Run
    // -----------------------------------------------------------------------
    Simulator::Stop(Seconds(tStop + drainTime));
    Simulator::Run();

    // -----------------------------------------------------------------------
    //  Results (main BSS only)
    // -----------------------------------------------------------------------
    flowMon->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(flowHelper.GetClassifier());
    auto stats = flowMon->GetFlowStats();

    double totalDelaySec = 0.0;
    uint64_t totalRxPackets = 0;
    uint64_t totalRxBytes = 0;
    uint64_t totalTxPackets = 0;
    uint64_t totalLostPackets = 0;
    constexpr double kBinWidth = 0.0001;
    std::vector<uint64_t> binCounts;
    for (auto& kv : stats)
    {
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(kv.first);
        // Main BSS downlink only (AP -> STA), matching the channel-access
        // variant's "main AP only" scope.
        if (t.sourceAddress != mainApIf.GetAddress(0) || t.destinationPort == 49000)
            continue;
        totalTxPackets += kv.second.txPackets;
        totalLostPackets += kv.second.lostPackets;
        totalDelaySec += kv.second.delaySum.GetSeconds();
        totalRxPackets += kv.second.rxPackets;
        totalRxBytes += kv.second.rxBytes;
        const Histogram& h = kv.second.delayHistogram;
        if (h.GetNBins() > binCounts.size())
            binCounts.resize(h.GetNBins(), 0);
        for (uint32_t i = 0; i < h.GetNBins(); ++i)
            binCounts[i] += h.GetBinCount(i);
    }

    const double meanLatMs = (totalRxPackets > 0)
                                 ? (totalDelaySec / totalRxPackets) * 1000.0
                                 : 0.0;
    const double p50Ms     = PercentileFromBins(binCounts, kBinWidth, 50.0) * 1000.0;
    const double p95Ms     = PercentileFromBins(binCounts, kBinWidth, 95.0) * 1000.0;
    const double p99Ms     = PercentileFromBins(binCounts, kBinWidth, 99.0) * 1000.0;
    const double meanThrMbps =
        (simTime > 0.0) ? totalRxBytes * 8.0 / (simTime * 1e6) : 0.0;

    std::cout << "\nMean DL Latency: "   << meanLatMs   << " ms\n";
    std::cout << "DL Latency p50: "     << p50Ms       << " ms\n";
    std::cout << "DL Latency p95: "     << p95Ms       << " ms\n";
    std::cout << "DL Latency p99: "     << p99Ms       << " ms\n";
    std::cout << "Mean DL Throughput: " << meanThrMbps << " Mb/s\n";
    std::cout << "DL Tx Packets: " << totalTxPackets << "\n";
    std::cout << "DL Rx Packets: " << totalRxPackets << "\n";
    std::cout << "DL Lost Packets: " << totalLostPackets << "\n";
    std::cout << "DL Unreceived Packets: "
              << (totalTxPackets >= totalRxPackets ? totalTxPackets - totalRxPackets : 0) << "\n";
    std::cout << "DL Packet Delivery Ratio: "
              << (totalTxPackets > 0 ? static_cast<double>(totalRxPackets) / totalTxPackets : 1.0)
              << "\n";

    Simulator::Destroy();
    return 0;
}
