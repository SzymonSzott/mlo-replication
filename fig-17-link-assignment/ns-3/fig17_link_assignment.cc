/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Latency Scenario #4B  — END-TO-END (FlowMonitor) variant
 * (Section 3.2.10 / Scenario 4.2.5)
 *
 * Copy of wifi-mlo-latency-scenario4b.cc with FlowMonitor end-to-end UDP
 * delay (IP-layer Tx → IP-layer Rx) instead of channel-access delay
 * (MacTx → PhyTxBegin).  Same output line schema.
 *
 * Building on Scenario #4A, this scenario explores additional channel
 * configurations and their potential to reduce latency in a contended
 * 4-BSS environment.
 *
 * Topology: 4 BSSs (A–D), each with 1 AP + 1 STA, all in mutual range.
 * Five 80 MHz channels available (Table 3.11):
 *   ch0: 5 GHz  ch42   ("channel 1"   in 5 GHz)
 *   ch1: 5 GHz  ch106  ("channel 100" in 5 GHz)
 *   ch2: 6 GHz  ch7    ("channel 1"   in 6 GHz)
 *   ch3: 6 GHz  ch23   ("channel 100" in 6 GHz)
 *   ch4: 6 GHz  ch39   ("channel 200" in 6 GHz)
 *
 * Modes (--strMode):
 *   SLO    – 1 link/BSS, exclusive channel: A→ch0, B→ch1, C→ch2, D→ch3
 *   STR2   – 2 links/BSS, cross-pair sharing per Fig. 3.5b (same as Scenario #4A):
 *              A=B={ch0,ch1}, C=D={ch2,ch3}  (two independent pairs, paper Fig. 3.5b)
 *   EMLSR2 – same topology as STR2 with EMLSR activated
 *   STR1P1 – 2 links/BSS: ch4 shared by all + 1 exclusive per BSS (Fig. 3.6a):
 *              A={ch4,ch0}, B={ch4,ch1}, C={ch4,ch2}, D={ch4,ch3}
 *   STR5   – 5 links/BSS, all 4 BSSs share all 5 channels (Fig. 3.6b)
 *
 * PHY:    EHT 802.11be, MCS 8, 2 SS, GI 800 ns, 80 MHz per link
 * A-MPDU: nMpdus = 1024 (per Table 3.11)
 * Traffic: Poisson arrivals (Exp inter-packet times), constant 1500-byte packets,
 * AP–STA distance: 5 m; all BSSs co-located for full mutual interference range
 *
 * Output lines parsed by runner:
 *   "Mean DL Latency: X.XXX ms"
 *   "DL Latency p50: X.XXX ms"
 *   "DL Latency p95: X.XXX ms"
 *   "DL Latency p99: X.XXX ms"
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

NS_LOG_COMPONENT_DEFINE("WifiMloLatencyScenario4bE2e");

// === End-to-end (FlowMonitor) delay helpers ===
namespace
{
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

int
main(int argc, char* argv[])
{
    // -----------------------------------------------------------------------
    //  Parameters
    // -----------------------------------------------------------------------
    double      simTime      = 10.0;
    uint32_t    payloadSize  = 1500;
    uint32_t    nMpdus       = 1024;
    double      offeredLoad  = 500.0;   // Mb/s total (split equally among 4 BSSs)
    std::string strMode      = "STR2";  // SLO | STR2 | EMLSR2 | STR1P1 | STR5
    double      startupGuard = 2.0;     // seconds of idle before traffic starts
    double      drainTime = 1.0;
    uint32_t    maxMacQueuePackets = 4096;
    double      maxMacQueueDelayMs = 500.0;

    CommandLine cmd(__FILE__);
    cmd.AddValue("simTime",      "Simulation time [s]",                    simTime);
    cmd.AddValue("payloadSize",  "UDP payload size [B]",                   payloadSize);
    cmd.AddValue("nMpdus",       "Max MPDUs per A-MPDU",                   nMpdus);
    cmd.AddValue("offeredLoad",  "Total DL offered load [Mb/s]",           offeredLoad);
    cmd.AddValue("strMode",      "Mode: SLO|STR2|EMLSR2|STR1P1|STR5",     strMode);
    cmd.AddValue("startupGuard", "Idle time before traffic [s]",           startupGuard);
    cmd.AddValue("drainTime", "Receiver-only drain interval [s]", drainTime);
    cmd.AddValue("maxMacQueuePackets", "Max Wi-Fi MAC queue size [packets]", maxMacQueuePackets);
    cmd.AddValue("maxMacQueueDelayMs", "Max Wi-Fi MAC queue residence time [ms]", maxMacQueueDelayMs);
    cmd.Parse(argc, argv);

    if (strMode != "SLO" && strMode != "STR2" && strMode != "EMLSR2" &&
        strMode != "STR1P1" && strMode != "STR5")
        NS_ABORT_MSG("strMode must be one of: SLO STR2 EMLSR2 STR1P1 STR5");
    if (offeredLoad <= 0.0)
        NS_ABORT_MSG("offeredLoad must be > 0");
    NS_ABORT_MSG_IF(startupGuard < 0.0 || drainTime < 0.0,
                    "startupGuard and drainTime must be non-negative");
    NS_ABORT_MSG_IF(maxMacQueuePackets == 0 || maxMacQueueDelayMs <= 0.0,
                    "Wi-Fi MAC queue limits must be positive");

    // Derive numLinks from mode
    uint32_t numLinks = 1;
    if (strMode == "STR2" || strMode == "EMLSR2" || strMode == "STR1P1")
        numLinks = 2;
    else if (strMode == "STR5")
        numLinks = 5;

    const bool emlsr = (strMode == "EMLSR2");

    RngSeedManager::SetSeed(1); // run index controlled by --RngRun

    // 100 µs bin width for FlowMonitor delayHistogram.
    Config::SetDefault("ns3::FlowMonitor::DelayBinWidth", DoubleValue(0.0001));

    std::cout << "=== Latency Scenario #4B ===\n"
              << "strMode: " << strMode
              << "  numLinks: " << numLinks
              << "  offeredLoad: " << offeredLoad << " Mb/s\n\n";

    // -----------------------------------------------------------------------
    //  Five physical spectrum channels (Table 3.11):
    //    ch0: 5 GHz ch42   ("channel 1"   in 5 GHz, 80 MHz)
    //    ch1: 5 GHz ch106  ("channel 100" in 5 GHz, 80 MHz)
    //    ch2: 6 GHz ch7    ("channel 1"   in 6 GHz, 80 MHz)
    //    ch3: 6 GHz ch23   ("channel 100" in 6 GHz, 80 MHz)
    //    ch4: 6 GHz ch39   ("channel 200" in 6 GHz, 80 MHz)
    // -----------------------------------------------------------------------
    struct PhyChan
    {
        uint32_t       chanNum;
        FrequencyRange range;
        const char*    band;   // for ChannelSettings string
    };

    // clang-format off
    const PhyChan phyChans[5] = {
        {  42, WIFI_SPECTRUM_5_GHZ, "BAND_5GHZ" },  // ch0: 5 GHz "channel 1"
        { 106, WIFI_SPECTRUM_5_GHZ, "BAND_5GHZ" },  // ch1: 5 GHz "channel 100"
        {   7, WIFI_SPECTRUM_6_GHZ, "BAND_6GHZ" },  // ch2: 6 GHz "channel 1"
        {  23, WIFI_SPECTRUM_6_GHZ, "BAND_6GHZ" },  // ch3: 6 GHz "channel 100"
        {  39, WIFI_SPECTRUM_6_GHZ, "BAND_6GHZ" },  // ch4: 6 GHz "channel 200"
    };
    // clang-format on

    // BSSs sharing the same specMedia object will hear each other and contend.
    std::vector<Ptr<MultiModelSpectrumChannel>> specMedia(5);
    for (int i = 0; i < 5; ++i)
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
    //  Per-BSS link assignments: bssLinkChanIdx[bss][link_slot] = ch index
    //
    //  SLO:           A→ch0, B→ch1, C→ch2, D→ch3
    //  STR2 / EMLSR2: cross-pair sharing per Fig. 3.5b:
    //                   A={ch0,ch1}, B={ch2,ch3}, C={ch0,ch2}, D={ch1,ch3}
    //                   (ch0: A+C, ch1: A+D, ch2: B+C, ch3: B+D)
    //  STR1P1:        ch4 shared by all + exclusive ch per BSS (Fig. 3.6a):
    //                   A={ch4,ch0}, B={ch4,ch1}, C={ch4,ch2}, D={ch4,ch3}
    //  STR5:          A=B=C=D={ch0,ch1,ch2,ch3,ch4}
    // -----------------------------------------------------------------------
    // clang-format off
    int bssLinkChanIdx[4][5] = {};

    if (strMode == "SLO")
    {
        bssLinkChanIdx[0][0] = 0; // A → ch0 (5 GHz "channel 1")
        bssLinkChanIdx[1][0] = 1; // B → ch1 (5 GHz "channel 100")
        bssLinkChanIdx[2][0] = 2; // C → ch2 (6 GHz "channel 1")
        bssLinkChanIdx[3][0] = 3; // D → ch3 (6 GHz "channel 100")
    }
    else if (strMode == "STR2" || strMode == "EMLSR2")
    {
        // Fig. 3.5b: two independent contending pairs
        bssLinkChanIdx[0][0] = 0; bssLinkChanIdx[0][1] = 1; // A = {ch0, ch1}
        bssLinkChanIdx[1][0] = 0; bssLinkChanIdx[1][1] = 1; // B = {ch0, ch1}
        bssLinkChanIdx[2][0] = 2; bssLinkChanIdx[2][1] = 3; // C = {ch2, ch3}
        bssLinkChanIdx[3][0] = 2; bssLinkChanIdx[3][1] = 3; // D = {ch2, ch3}
    }
    else if (strMode == "STR1P1")
    {
        // ch4 shared by all 4 BSSs; exclusive channels ch0–ch3 for A–D
        bssLinkChanIdx[0][0] = 4; bssLinkChanIdx[0][1] = 0; // A = {ch4, ch0}
        bssLinkChanIdx[1][0] = 4; bssLinkChanIdx[1][1] = 1; // B = {ch4, ch1}
        bssLinkChanIdx[2][0] = 4; bssLinkChanIdx[2][1] = 2; // C = {ch4, ch2}
        bssLinkChanIdx[3][0] = 4; bssLinkChanIdx[3][1] = 3; // D = {ch4, ch3}
    }
    else // STR5
    {
        for (int bss = 0; bss < 4; ++bss)
            for (int l = 0; l < 5; ++l)
                bssLinkChanIdx[bss][l] = l;
    }
    // clang-format on

    // -----------------------------------------------------------------------
    //  Nodes
    // -----------------------------------------------------------------------
    NodeContainer apNodes;
    apNodes.Create(4);
    NodeContainer staNodes;
    staNodes.Create(4);

    // -----------------------------------------------------------------------
    //  WiFi (EHT 802.11be, MCS 8, 2 SS, GI 800 ns)
    // -----------------------------------------------------------------------
    std::ostringstream queueSize;
    queueSize << maxMacQueuePackets << "p";
    Config::SetDefault("ns3::WifiMacQueue::MaxSize", QueueSizeValue(QueueSize(queueSize.str())));
    Config::SetDefault("ns3::WifiMacQueue::MaxDelay", TimeValue(MilliSeconds(maxMacQueueDelayMs)));
    Config::SetDefault("ns3::ArpCache::PendingQueueSize", UintegerValue(maxMacQueuePackets));

    // 256-QAM 3/4 (thesis Table 3.11 "MCS 8") -> EHT MCS 8 in 802.11be indexing.
    const char* DATA_MODE = "EhtMcs8";
    const char* CTL_MODE  = "EhtMcs0";

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211be);
    wifi.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
    if (emlsr)
    {
        wifi.ConfigEhtOptions("EmlsrActivated",   BooleanValue(true));
        wifi.ConfigEhtOptions("TransitionTimeout", TimeValue(MicroSeconds(1024)));
    }
    for (uint32_t l = 0; l < numLinks; ++l)
        wifi.SetRemoteStationManager(l, "ns3::ConstantRateWifiManager",
                                     "DataMode",    StringValue(DATA_MODE),
                                     "ControlMode", StringValue(CTL_MODE));

    // -----------------------------------------------------------------------
    //  Install one PHY+MAC per BSS; BSSs sharing a specMedia object will
    //  hear each other and contend.
    // -----------------------------------------------------------------------
    NetDeviceContainer apDevices;
    NetDeviceContainer staDevices;
    const std::string bssNames[] = {"A", "B", "C", "D"};

    for (uint32_t bss = 0; bss < 4; ++bss)
    {
        SpectrumWifiPhyHelper phy(numLinks);
        phy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
        if (numLinks > 1)
            phy.SetPcapCaptureType(WifiPhyHelper::PcapCaptureType::PCAP_PER_LINK);

        for (uint32_t l = 0; l < numLinks; ++l)
        {
            int ci = bssLinkChanIdx[bss][l];
            std::ostringstream chSet;
            chSet << "{" << phyChans[ci].chanNum << ", 80, " << phyChans[ci].band << ", 0}";
            phy.AddChannel(specMedia[ci], phyChans[ci].range);
            phy.Set(l, "ChannelSettings",             StringValue(chSet.str()));
            phy.Set(l, "Antennas",                    UintegerValue(2));
            phy.Set(l, "MaxSupportedTxSpatialStreams", UintegerValue(2));
            phy.Set(l, "MaxSupportedRxSpatialStreams", UintegerValue(2));
        }

        WifiMacHelper mac;
        Ssid ssid(bssNames[bss]);

        mac.SetType("ns3::ApWifiMac",
                    "Ssid",            SsidValue(ssid),
                    "BE_MaxAmpduSize", UintegerValue(nMpdus * (payloadSize + 60)),
                    "MpduBufferSize",  UintegerValue(nMpdus));
        apDevices.Add(wifi.Install(phy, mac, apNodes.Get(bss)));

        mac.SetType("ns3::StaWifiMac",
                    "Ssid",            SsidValue(ssid),
                    "ActiveProbing",   BooleanValue(false),
                    "BE_MaxAmpduSize", UintegerValue(nMpdus * (payloadSize + 60)),
                    "MpduBufferSize",  UintegerValue(nMpdus));
        staDevices.Add(wifi.Install(phy, mac, staNodes.Get(bss)));
    }

    // -----------------------------------------------------------------------
    //  Mobility: 4 BSSs in mutual interference range but spatially separated.
    //  APs at corners of a 10x10 m apartment, STAs 3 m from their own AP.
    //  Co-locating all 4 APs at one point would inflate inter-BSS contention
    //  to artificial extremes; the apartment layout matches the residential
    //  TGax-style placement used in Carrascosa-Zamakois et al. PIMRC 2023.
    // -----------------------------------------------------------------------
    const Vector apPositions[4]  = {
        Vector( 0.0,  0.0, 0.0),
        Vector(10.0,  0.0, 0.0),
        Vector( 0.0, 10.0, 0.0),
        Vector(10.0, 10.0, 0.0),
    };
    const Vector staPositions[4] = {
        Vector( 3.0,  0.0, 0.0),
        Vector( 7.0,  0.0, 0.0),
        Vector( 3.0, 10.0, 0.0),
        Vector( 7.0, 10.0, 0.0),
    };

    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");

    for (uint32_t bss = 0; bss < 4; ++bss)
    {
        Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator>();
        pos->Add(apPositions[bss]);
        pos->Add(staPositions[bss]);
        mobility.SetPositionAllocator(pos);
        mobility.Install(apNodes.Get(bss));
        mobility.Install(staNodes.Get(bss));
    }

    // -----------------------------------------------------------------------
    //  Internet stack + IP (one /24 subnet per BSS)
    // -----------------------------------------------------------------------
    InternetStackHelper stack;
    stack.Install(apNodes);
    stack.Install(staNodes);

    Ipv4AddressHelper addr;
    addr.SetBase("10.1.0.0", "255.255.255.0");

    std::vector<Ipv4Address> staAddrs(4);
    for (uint32_t bss = 0; bss < 4; ++bss)
    {
        Ipv4InterfaceContainer apIf = addr.Assign(apDevices.Get(bss));
        Ipv4InterfaceContainer staIf = addr.Assign(staDevices.Get(bss));
        PopulateE2eArp(apIf, staIf);
        WarmUpE2e(apIf, staIf, startupGuard);
        staAddrs[bss] = staIf.GetAddress(0);
        addr.NewNetwork();
    }

    // -----------------------------------------------------------------------
    //  Traffic: Poisson On-Off DL (AP → STA per BSS)
    //  Exponential inter-packet times, constant 1500-byte packets (paper §III).
    //  Modelled with OnOff: high DataRate → one packet per on-cycle;
    //  OffTime = Exp(mean_inter_arrival - onTime).
    // -----------------------------------------------------------------------
    const double tStart      = startupGuard;
    const double tStop       = simTime + startupGuard;
    const double perBssLoad  = offeredLoad / 4.0; // Mb/s per BSS

    const double appDataRateBps = 5.0e9;
    const double appPktBits     = payloadSize * 8.0;
    const double onTimeSec      = appPktBits / appDataRateBps;
    const double iatSec         = appPktBits / (perBssLoad * 1e6);
    const double offMeanSec     = iatSec - onTimeSec;
    NS_ABORT_IF(offMeanSec <= 0.0);

    std::ostringstream drStr, onTimeStr, offTimeStr;
    drStr      << static_cast<uint64_t>(appDataRateBps) << "bps";
    onTimeStr  << "ns3::ConstantRandomVariable[Constant=" << onTimeSec << "]";
    offTimeStr << "ns3::ExponentialRandomVariable[Mean=" << offMeanSec << "]";

    for (uint32_t bss = 0; bss < 4; ++bss)
    {
        OnOffHelper onoff("ns3::UdpSocketFactory",
                          InetSocketAddress(staAddrs[bss], 5000));
        onoff.SetAttribute("DataRate",   StringValue(drStr.str()));
        onoff.SetAttribute("PacketSize", UintegerValue(payloadSize));
        onoff.SetAttribute("OnTime",  StringValue(onTimeStr.str()));
        onoff.SetAttribute("OffTime", StringValue(offTimeStr.str()));

        ApplicationContainer cApp = onoff.Install(apNodes.Get(bss));
        cApp.Start(Seconds(tStart));
        cApp.Stop(Seconds(tStop));

        UdpServerHelper server(5000);
        ApplicationContainer sApp = server.Install(staNodes.Get(bss));
        sApp.Start(Seconds(tStart));
        sApp.Stop(Seconds(tStop + drainTime));
    }

    // -----------------------------------------------------------------------
    //  FlowMonitor — end-to-end UDP delay + throughput
    // -----------------------------------------------------------------------
    FlowMonitorHelper flowHelper;
    Ptr<FlowMonitor> flowMon = flowHelper.InstallAll();

    Simulator::Stop(Seconds(tStop + drainTime));
    Simulator::Run();

    // -----------------------------------------------------------------------
    //  Results — aggregate over all 4 DL flows (one per BSS; UDP port 5000)
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
        if (t.destinationPort != 5000)
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
    const double p50LatencyMs  = PercentileFromBins(binCounts, kBinWidth, 50.0) * 1000.0;
    const double p95LatencyMs  = PercentileFromBins(binCounts, kBinWidth, 95.0) * 1000.0;
    const double p99LatencyMs  = PercentileFromBins(binCounts, kBinWidth, 99.0) * 1000.0;
    const double meanThrMbps =
        (simTime > 0.0) ? (totalRxBytes * 8.0) / (simTime * 1e6) : 0.0;

    std::cout << "\nMean DL Latency: "   << meanLatencyMs << " ms\n";
    std::cout << "DL Latency p50: "     << p50LatencyMs  << " ms\n";
    std::cout << "DL Latency p95: "     << p95LatencyMs  << " ms\n";
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
