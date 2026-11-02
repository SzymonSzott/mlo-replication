/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Latency Scenario #3  — END-TO-END (FlowMonitor) variant
 * (based on [15] – Kozioł et al., accepted work)
 *
 * Copy of wifi-mlo-latency-scenario3.cc with FlowMonitor end-to-end UDP delay
 * (IP-layer Tx → IP-layer Rx) instead of channel-access delay
 * (MacTx → PhyTxBegin).  Same output line schema.
 *
 * Purpose: evaluate how varying link configurations and MLO modes (STR/EMLSR)
 * affect DL latency.
 *
 * Single BSS: 1 AP + 1 STA, 1-3 links in STR or EMLSR mode.
 *   numLinks = 1 (SLO):  5 GHz "channel 1"   80 MHz
 *   numLinks = 2 (STR):  5 GHz "channel 1" + "channel 100", both 80 MHz
 *   numLinks = 3 (STR):  5 GHz "channel 1" + "channel 100" + "channel 136", all 80 MHz
 *   (per Table 3.9, test with STR and EMLSR modes)
 *
 * PHY:    EHT 802.11be, MCS 8, 2 SS, GI 800 ns, 80 MHz per link
 * A-MPDU: nMpdus = 1024
 * Traffic: constant-rate On-Off DL (AP → STA), fixed offered load swept via runner
 * AP–STA distance: 5 m
 * MLO mode: STR (default) or EMLSR (if emlsr=true)
 *
 * Output lines parsed by runner:
 *   "Mean DL Latency: X.XXX ms"
 *   "DL Latency p1: X.XXX ms"
 *   "Mean DL Throughput: X.XX Mb/s"
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

NS_LOG_COMPONENT_DEFINE("WifiMloLatencyScenario3E2e");

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

// ------} // namespace

// ---------------------------------------------------------------------------

int
main(int argc, char* argv[])
{
    // -----------------------------------------------------------------------
    //  Parameters
    // -----------------------------------------------------------------------
    double      simTime      = 10.0;
    uint32_t    payloadSize  = 1500;
    uint32_t    nMpdus       = 1024;
    uint32_t    numLinks     = 2;     // 1, 2, or 3
    double      offeredLoad  = 500.0; // Mb/s; swept by runner
    bool        emlsr        = false; // false = STR, true = EMLSR
    double      startupGuard = 2.0;
    double      drainTime = 1.0;
    uint32_t    maxMacQueuePackets = 4096;
    double      maxMacQueueDelayMs = 500.0;

    CommandLine cmd(__FILE__);
    cmd.AddValue("simTime",     "Simulation time [s]",                   simTime);
    cmd.AddValue("payloadSize", "UDP payload size [B]",                  payloadSize);
    cmd.AddValue("nMpdus",      "Max MPDUs per A-MPDU",                  nMpdus);
    cmd.AddValue("numLinks",    "Number of links (1, 2, or 3)",          numLinks);
    cmd.AddValue("offeredLoad", "DL offered load [Mb/s]",                offeredLoad);
    cmd.AddValue("emlsr",       "Enable EMLSR mode (false=STR, true=EMLSR)", emlsr);
    cmd.AddValue("startupGuard", "Idle time before traffic [s]", startupGuard);
    cmd.AddValue("drainTime", "Receiver-only drain interval [s]", drainTime);
    cmd.AddValue("maxMacQueuePackets", "Max Wi-Fi MAC queue size [packets]", maxMacQueuePackets);
    cmd.AddValue("maxMacQueueDelayMs", "Max Wi-Fi MAC queue residence time [ms]", maxMacQueueDelayMs);
    cmd.Parse(argc, argv);

    if (numLinks < 1 || numLinks > 3)
        NS_ABORT_MSG("numLinks must be 1, 2, or 3");
    if (offeredLoad <= 0.0)
        NS_ABORT_MSG("offeredLoad must be > 0");
    NS_ABORT_MSG_IF(startupGuard < 0.0 || drainTime < 0.0,
                    "startupGuard and drainTime must be non-negative");
    NS_ABORT_MSG_IF(maxMacQueuePackets == 0 || maxMacQueueDelayMs <= 0.0,
                    "Wi-Fi MAC queue limits must be positive");

    std::ostringstream queueSize;
    queueSize << maxMacQueuePackets << "p";
    Config::SetDefault("ns3::WifiMacQueue::MaxSize", QueueSizeValue(QueueSize(queueSize.str())));
    Config::SetDefault("ns3::WifiMacQueue::MaxDelay", TimeValue(MilliSeconds(maxMacQueueDelayMs)));
    Config::SetDefault("ns3::ArpCache::PendingQueueSize", UintegerValue(maxMacQueuePackets));

    RngSeedManager::SetSeed(1); // run number controlled by --RngRun

    // 100 µs bin width gives good resolution for the [0, ~1 s] delay range
    // used by FlowMonitor's delayHistogram.
    Config::SetDefault("ns3::FlowMonitor::DelayBinWidth", DoubleValue(0.0001));

    const char* mloMode = emlsr ? "EMLSR" : "STR";
    std::cout << "=== Latency Scenario #3 ===\n"
              << "numLinks: " << numLinks
              << "  MLO mode: " << mloMode
              << "  offeredLoad: " << offeredLoad << " Mb/s\n\n";

    // -----------------------------------------------------------------------
    //  Channel plan (5 GHz only, 80 MHz per link):
    //  ns-3 ChannelSettings expects valid 80 MHz center channels in 5 GHz.
    //    Link 0: center ch42  (maps to "channel 1" block)
    //    Link 1: center ch106 (maps to "channel 100" block)  -- numLinks >= 2
    //    Link 2: center ch138 (maps to "channel 136" block)  -- numLinks == 3
    // -----------------------------------------------------------------------
    struct LinkCfg
    {
        uint32_t chanNum;
        uint32_t width; // MHz
        const char* band;
    };

    const LinkCfg linkCfgs[3] = {
        {42,  80, "BAND_5GHZ"},
        {106, 80, "BAND_5GHZ"},
        {138, 80, "BAND_5GHZ"},
    };

    // -----------------------------------------------------------------------
    //  Nodes
    // -----------------------------------------------------------------------
    NodeContainer apNode;  apNode.Create(1);
    NodeContainer staNode; staNode.Create(1);

    // -----------------------------------------------------------------------
    //  Spectrum channels — one independent object per link
    // -----------------------------------------------------------------------
    std::vector<Ptr<MultiModelSpectrumChannel>> specChans(numLinks);
    for (uint32_t l = 0; l < numLinks; ++l)
    {
        specChans[l] = CreateObject<MultiModelSpectrumChannel>();
        // Residential indoor propagation per thesis Table 3.1 (LogDistance gamma=3.5, refLoss=40 dB @ 1 m).
        Ptr<LogDistancePropagationLossModel> loss = CreateObject<LogDistancePropagationLossModel>();
        loss->SetAttribute("Exponent",          DoubleValue(3.5));
        loss->SetAttribute("ReferenceDistance", DoubleValue(1.0));
        loss->SetAttribute("ReferenceLoss",     DoubleValue(40.0));
        specChans[l]->AddPropagationLossModel(loss);
    }

    // -----------------------------------------------------------------------
    //  PHY helper
    // -----------------------------------------------------------------------
    SpectrumWifiPhyHelper phy(numLinks);
    phy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
    if (numLinks > 1)
        phy.SetPcapCaptureType(WifiPhyHelper::PcapCaptureType::PCAP_PER_LINK);

    for (uint32_t l = 0; l < numLinks; ++l)
    {
        const LinkCfg& cfg = linkCfgs[l];
        std::ostringstream oss;
        oss << "{" << cfg.chanNum << ", " << cfg.width << ", " << cfg.band << ", 0}";
        phy.AddChannel(specChans[l], WIFI_SPECTRUM_5_GHZ);
        phy.Set(l, "ChannelSettings",             StringValue(oss.str()));
        phy.Set(l, "Antennas",                    UintegerValue(2));
        phy.Set(l, "MaxSupportedTxSpatialStreams", UintegerValue(2));
        phy.Set(l, "MaxSupportedRxSpatialStreams", UintegerValue(2));
    }

    // -----------------------------------------------------------------------
    //  WiFi (EHT 802.11be, MCS 8)
    // -----------------------------------------------------------------------
    // 256-QAM 3/4 (thesis Table 3.9 "MCS 8") -> EHT MCS 8 in 802.11be indexing.
    const char* DATA_MODE = "EhtMcs8";
    const char* CTL_MODE  = "EhtMcs0";

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211be);
    wifi.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
    if (emlsr)
    {
        wifi.ConfigEhtOptions("EmlsrActivated",     BooleanValue(true));
        wifi.ConfigEhtOptions("TransitionTimeout",  TimeValue(MicroSeconds(1024)));
        wifi.ConfigEhtOptions("MediumSyncDuration", TimeValue(MicroSeconds(0)));
        wifi.ConfigEhtOptions("MsdOfdmEdThreshold", IntegerValue(-72));
        wifi.ConfigEhtOptions("MsdMaxNTxops",       UintegerValue(0));
    }
    for (uint32_t l = 0; l < numLinks; ++l)
        wifi.SetRemoteStationManager(l, "ns3::ConstantRateWifiManager",
                                     "DataMode",    StringValue(DATA_MODE),
                                     "ControlMode", StringValue(CTL_MODE));

    // -----------------------------------------------------------------------
    //  MAC helper  (no MU scheduler — single STA, SU transmission only)
    // -----------------------------------------------------------------------
    WifiMacHelper wifiMac;
    Ssid ssid = Ssid("mlo-lat3");

    wifiMac.SetType("ns3::ApWifiMac",
                    "Ssid",            SsidValue(ssid),
                    "BE_MaxAmpduSize", UintegerValue(nMpdus * (payloadSize + 60)),
                    "MpduBufferSize",  UintegerValue(nMpdus));
    NetDeviceContainer apDev = wifi.Install(phy, wifiMac, apNode);

    // EMLSR manager must be installed on the *non-AP* MLD before its devices
    // are created; without it, EmlsrActivated only advertises support and the
    // device behaves identically to STR. The EmlsrLinkSet enumerates which
    // setup links should run EMLSR (paper [15] uses all of them).
    if (emlsr && numLinks > 1)
    {
        std::ostringstream linkSet;
        for (uint32_t l = 0; l < numLinks; ++l)
        {
            if (l > 0) linkSet << ",";
            linkSet << l;
        }
        wifiMac.SetEmlsrManager("ns3::AdvancedEmlsrManager",
                                "EmlsrLinkSet",         StringValue(linkSet.str()),
                                "SwitchAuxPhy",         BooleanValue(true),
                                "PutAuxPhyToSleep",     BooleanValue(false),
                                "AuxPhyChannelWidth",   UintegerValue(80),
                                "AuxPhyMaxModClass",    StringValue("EHT"),
                                "AuxPhyTxCapable",      BooleanValue(true));
    }

    wifiMac.SetType("ns3::StaWifiMac",
                    "Ssid",            SsidValue(ssid),
                    "ActiveProbing",   BooleanValue(false),
                    "BE_MaxAmpduSize", UintegerValue(nMpdus * (payloadSize + 60)),
                    "MpduBufferSize",  UintegerValue(nMpdus));
    NetDeviceContainer staDev = wifi.Install(phy, wifiMac, staNode);

    // -----------------------------------------------------------------------
    //  Mobility: AP at origin, STA 5 m away
    // -----------------------------------------------------------------------
    MobilityHelper mobility;
    Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator>();
    pos->Add(Vector(0.0, 0.0, 0.0)); // AP
    pos->Add(Vector(5.0, 0.0, 0.0)); // STA -- 5 m link distance per thesis Table 3.9
    mobility.SetPositionAllocator(pos);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(apNode);
    mobility.Install(staNode);

    // -----------------------------------------------------------------------
    //  Internet stack + IP
    // -----------------------------------------------------------------------
    InternetStackHelper stack;
    stack.Install(apNode);
    stack.Install(staNode);

    Ipv4AddressHelper addr;
    addr.SetBase("10.1.0.0", "255.255.255.0");
    Ipv4InterfaceContainer apIf  = addr.Assign(apDev);
    Ipv4InterfaceContainer staIf = addr.Assign(staDev);
    PopulateE2eArp(apIf, staIf);
    WarmUpE2e(apIf, staIf, startupGuard);
    // Global route population queries NetDevice::GetChannel(), which is not
    // valid for multi-link WifiNetDevice. This scenario is a single subnet
    // (AP <-> STA), so static host routing setup is unnecessary.

    // -----------------------------------------------------------------------
    //  Traffic: constant-rate On-Off DL (AP → STA)
    //  OnTime = always on, OffTime = 0  →  steady offered load = offeredLoad Mb/s.
    // -----------------------------------------------------------------------
    const double tStart = startupGuard;
    const double tStop  = simTime + tStart;

    std::ostringstream drStr;
    drStr << static_cast<uint64_t>(offeredLoad * 1e6) << "bps";

    OnOffHelper onoff("ns3::UdpSocketFactory",
                      InetSocketAddress(staIf.GetAddress(0), 5000));
    onoff.SetAttribute("DataRate",   StringValue(drStr.str()));
    onoff.SetAttribute("PacketSize", UintegerValue(payloadSize));
    onoff.SetAttribute("OnTime",     StringValue("ns3::ConstantRandomVariable[Constant=1e9]"));
    onoff.SetAttribute("OffTime",    StringValue("ns3::ConstantRandomVariable[Constant=0]"));

    UdpServerHelper server(5000);
    ApplicationContainer sApp = server.Install(staNode.Get(0));
    sApp.Start(Seconds(tStart));
    sApp.Stop(Seconds(tStop + drainTime));

    ApplicationContainer cApp = onoff.Install(apNode.Get(0));
    cApp.Start(Seconds(tStart));
    cApp.Stop(Seconds(tStop));

    // -----------------------------------------------------------------------
    //  FlowMonitor — end-to-end UDP delay + throughput
    // -----------------------------------------------------------------------
    FlowMonitorHelper flowHelper;
    Ptr<FlowMonitor> flowMon = flowHelper.InstallAll();

    Simulator::Stop(Seconds(tStop + drainTime));
    Simulator::Run();

    // -----------------------------------------------------------------------
    //  Results — average over all UDP downlink flows (AP → STA)
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
        if (t.sourceAddress != apIf.GetAddress(0) || t.destinationPort == 49000)
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

    const double meanLatencyMs = (totalRxPackets > 0)
                                     ? (totalDelaySec / totalRxPackets) * 1000.0
                                     : 0.0;
    const double p1LatencyMs   = PercentileFromBins(binCounts, kBinWidth, 1.0)  * 1000.0;
    const double p95LatencyMs  = PercentileFromBins(binCounts, kBinWidth, 95.0) * 1000.0;
    const double p99LatencyMs  = PercentileFromBins(binCounts, kBinWidth, 99.0) * 1000.0;
    std::cout << "DL Latency p95: " << p95LatencyMs << " ms\n";
    const double meanThrMbps   = (simTime > 0.0)
                                 ? (totalRxBytes * 8.0) / (simTime * 1e6)
                                 : 0.0;

    std::cout << "\nMean DL Latency: "   << meanLatencyMs << " ms\n";
    std::cout << "DL Latency p1: "      << p1LatencyMs    << " ms\n";
    std::cout << "DL Latency p99: "     << p99LatencyMs  << " ms\n";
    std::cout << "Mean DL Throughput: " << meanThrMbps   << " Mb/s\n";
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
