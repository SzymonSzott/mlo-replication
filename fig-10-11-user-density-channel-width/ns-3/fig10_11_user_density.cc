/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Latency Scenario #1  — END-TO-END (FlowMonitor) variant
 * (based on [16] – Jeknic & Kocan, IEEE IT 2024)
 *
 * Copy of wifi-mlo-latency-scenario1.cc with FlowMonitor end-to-end UDP
 * delay (application send → application receive) instead of the
 * channel-access-delay metric (MacTx → PhyTxBegin). Same output format,
 * so the runner & plotter only need a different binary path / log dir.
 *
 * Analyses how different numbers of links and stations affect DL latency.
 *
 * Single BSS: 1 AP + nStas STAs
 *   1 link  → 5 GHz ch42  (80 MHz) or ch50  (160 MHz)
 *   2 links → 5 GHz ch42 + ch106 (80 MHz) or ch50 + ch114 (160 MHz)
 *   ("Channel 1" / "Channel 100" from the paper map to these center channels)
 *
 * PHY: EHT 802.11be, MCS 11, 2 SS, GI 800 ns
 * A-MPDU: 1024 MPDUs (required for proper 160 MHz operation in ns-3)
 * Traffic: normalized offered DL load (AP → each STA), equally split per user
 * AP–STA distance: STAs placed on a circle of radius 5 m
 *
 * Delay metric: IP-layer end-to-end delay from FlowMonitor (IPv4 Tx → IPv4 Rx).
 *
 * Output lines parsed by runner:
 *   "Mean DL Latency: X.XXX ms"
 *   "Mean DL Throughput: X.XX Mb/s"
 */

#include "../_common/e2e-arp.h"
#include "../_common/e2e-percentiles.h"
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
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("WifiMloLatencyScenario1E2e");

static double measurementStart = 0;
static uint64_t nackedMpdus = 0;
static uint64_t ackedRetryMpdus = 0;
static void CountNack(Ptr<const WifiMpdu> mpdu)
{
    if (Simulator::Now().GetSeconds() >= measurementStart && mpdu->GetHeader().IsQosData())
        ++nackedMpdus;
}
static void CountAck(Ptr<const WifiMpdu> mpdu)
{
    if (Simulator::Now().GetSeconds() >= measurementStart && mpdu->GetHeader().IsQosData()
        && mpdu->GetHeader().IsRetry())
        ++ackedRetryMpdus;
}

// FlowMonitor instance is created in main(); no global instrumentation here.

// ---------------------------------------------------------------------------

static void
AddPermanentArpEntry(const Ipv4InterfaceContainer& interfaces,
                     uint32_t interfaceIndex,
                     Ipv4Address neighborIp,
                     const Address& neighborMac)
{
    const auto [ipv4, index] = interfaces.Get(interfaceIndex);
    Ptr<Ipv4Interface> interface = DynamicCast<Ipv4L3Protocol>(ipv4)->GetInterface(index);
    Ptr<ArpCache> cache = interface->GetArpCache();
    ArpCache::Entry* entry = cache->Lookup(neighborIp);
    if (entry == nullptr)
    {
        entry = cache->Add(neighborIp);
    }
    entry->SetMacAddress(neighborMac);
    entry->MarkPermanent();
}

// ---------------------------------------------------------------------------

int
main(int argc, char* argv[])
{
    // -----------------------------------------------------------------------
    //  Parameters
    // -----------------------------------------------------------------------
    double      simTime      = 10.0;
    uint32_t    payloadSize  = 1500;
    uint32_t    nMpdus       = 1024;  // A-MPDU size (large to avoid ns-3 queue
                                      // starvation at 160 MHz PHY rates)
    uint32_t    numLinks     = 2;    // 1 or 2
    uint32_t    channelWidth = 80;   // 80 or 160 MHz
    uint32_t    nStas        = 4;    // 1, 4, or 10
    double      normalizedLoad = 1.0; // 0..1, referenced to SLO peak for given width
    double      startupGuard = 2.0;
    double      drainTime = 1.0;
    uint32_t    maxMacQueuePackets = 4096;
    double      maxMacQueueDelayMs = 500.0;
    double      txPowerDbm = 23.0;
    double      offeredLoadMbps = -1.0;

    CommandLine cmd(__FILE__);
    cmd.AddValue("simTime",      "Simulation time [s]",           simTime);
    cmd.AddValue("payloadSize",  "UDP payload size [B]",          payloadSize);
    cmd.AddValue("nMpdus",       "Max MPDUs per A-MPDU",          nMpdus);
    cmd.AddValue("numLinks",     "Number of links (1 or 2)",      numLinks);
    cmd.AddValue("channelWidth", "Channel width per link [MHz]",  channelWidth);
    cmd.AddValue("nStas",        "Number of stations (1,4,10)",   nStas);
    cmd.AddValue("normalizedLoad", "Normalized offered load [0..1]", normalizedLoad);
    cmd.AddValue("startupGuard", "Idle time before traffic [s]", startupGuard);
    cmd.AddValue("drainTime", "Receiver-only drain interval [s]", drainTime);
    cmd.AddValue("maxMacQueuePackets", "Max Wi-Fi MAC queue size [packets]", maxMacQueuePackets);
    cmd.AddValue("maxMacQueueDelayMs", "Max Wi-Fi MAC queue residence time [ms]", maxMacQueueDelayMs);
    cmd.AddValue("txPowerDbm", "Fixed transmit power for width sensitivity checks [dBm]", txPowerDbm);
    cmd.AddValue("offeredLoadMbps", "Absolute UDP load override; -1 uses normalized load", offeredLoadMbps);
    cmd.Parse(argc, argv);
    measurementStart = startupGuard;
    Config::SetDefault("ns3::WifiPhy::TxPowerStart", DoubleValue(txPowerDbm));
    Config::SetDefault("ns3::WifiPhy::TxPowerEnd", DoubleValue(txPowerDbm));

    if (numLinks < 1 || numLinks > 2)
        NS_ABORT_MSG("numLinks must be 1 or 2");
    if (channelWidth != 80 && channelWidth != 160)
        NS_ABORT_MSG("channelWidth must be 80 or 160");
    if (nStas < 1)
        NS_ABORT_MSG("nStas must be >= 1");
    if (normalizedLoad < 0.0 || normalizedLoad > 1.0)
        NS_ABORT_MSG("normalizedLoad must be in [0, 1]");
    NS_ABORT_MSG_IF(startupGuard < 0.0 || drainTime < 0.0,
                    "startupGuard and drainTime must be non-negative");
    NS_ABORT_MSG_IF(maxMacQueuePackets == 0 || maxMacQueueDelayMs <= 0.0,
                    "Wi-Fi MAC queue limits must be positive");

    std::ostringstream queueSize;
    queueSize << maxMacQueuePackets << "p";
    Config::SetDefault("ns3::WifiMacQueue::MaxSize",
                       QueueSizeValue(QueueSize(queueSize.str())));
    Config::SetDefault("ns3::WifiMacQueue::MaxDelay",
                       TimeValue(MilliSeconds(maxMacQueueDelayMs)));
    Config::SetDefault("ns3::ArpCache::PendingQueueSize",
                       UintegerValue(maxMacQueuePackets));

    RngSeedManager::SetSeed(1); // run number controlled by --RngRun
    Config::SetDefault("ns3::FlowMonitor::DelayBinWidth", DoubleValue(0.0001));

    std::cout << "=== Latency Scenario #1 ===\n"
              << "numLinks: " << numLinks
              << "  channelWidth: " << channelWidth << " MHz"
              << "  nStas: " << nStas
              << "  normalizedLoad: " << normalizedLoad << "\n\n";

    // -----------------------------------------------------------------------
    //  Channel center numbers
    //  Paper "Channel 1"   → 80 MHz: ch42,  160 MHz: ch50
    //  Paper "Channel 100" → 80 MHz: ch106, 160 MHz: ch114
    // -----------------------------------------------------------------------
    uint32_t ch0 = (channelWidth == 80) ? 42 : 50;
    uint32_t ch1 = (channelWidth == 80) ? 106 : 114;

    // -----------------------------------------------------------------------
    //  Nodes
    // -----------------------------------------------------------------------
    NodeContainer apNode;   apNode.Create(1);
    NodeContainer staNodes; staNodes.Create(nStas);

    // -----------------------------------------------------------------------
    //  Spectrum channels (one per link — ensures cross-band isolation)
    // -----------------------------------------------------------------------
    // Residential building propagation environment (per Jeknic & Kocan, IT 2024).
    // Log-distance with exponent 3.5 approximates the residential model used in
    // the original paper (matlab residential propagation model has similar slope).
    auto makeResidentialChannel = []() {
        Ptr<MultiModelSpectrumChannel> c = CreateObject<MultiModelSpectrumChannel>();
        Ptr<LogDistancePropagationLossModel> plm =
            CreateObject<LogDistancePropagationLossModel>();
        plm->SetAttribute("Exponent", DoubleValue(3.5));
        plm->SetAttribute("ReferenceDistance", DoubleValue(1.0));
        plm->SetAttribute("ReferenceLoss", DoubleValue(40.0));
        c->AddPropagationLossModel(plm);
        return c;
    };

    Ptr<MultiModelSpectrumChannel> specCh0 = makeResidentialChannel();

    Ptr<MultiModelSpectrumChannel> specCh1;
    if (numLinks == 2)
    {
        specCh1 = makeResidentialChannel();
    }

    // -----------------------------------------------------------------------
    //  PHY helper
    // -----------------------------------------------------------------------
    SpectrumWifiPhyHelper phy(numLinks);
    phy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
    if (numLinks > 1)
        phy.SetPcapCaptureType(WifiPhyHelper::PcapCaptureType::PCAP_PER_LINK);

    {
        std::ostringstream oss;
        oss << "{" << ch0 << ", " << channelWidth << ", BAND_5GHZ, 0}";
        phy.AddChannel(specCh0, WIFI_SPECTRUM_5_GHZ);
        phy.Set(0, "ChannelSettings",             StringValue(oss.str()));
        phy.Set(0, "Antennas",                    UintegerValue(2));
        phy.Set(0, "MaxSupportedTxSpatialStreams", UintegerValue(2));
        phy.Set(0, "MaxSupportedRxSpatialStreams", UintegerValue(2));
    }
    if (numLinks == 2)
    {
        std::ostringstream oss;
        oss << "{" << ch1 << ", " << channelWidth << ", BAND_5GHZ, 0}";
        phy.AddChannel(specCh1, WIFI_SPECTRUM_5_GHZ);
        phy.Set(1, "ChannelSettings",             StringValue(oss.str()));
        phy.Set(1, "Antennas",                    UintegerValue(2));
        phy.Set(1, "MaxSupportedTxSpatialStreams", UintegerValue(2));
        phy.Set(1, "MaxSupportedRxSpatialStreams", UintegerValue(2));
    }

    // -----------------------------------------------------------------------
    //  WiFi (EHT 802.11be, MCS 11)
    // -----------------------------------------------------------------------
    const char* DATA_MODE = "EhtMcs11";
    const char* CTL_MODE  = "EhtMcs0";

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211be);
    wifi.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
    for (uint32_t l = 0; l < numLinks; ++l)
        wifi.SetRemoteStationManager(l, "ns3::ConstantRateWifiManager",
                                     "DataMode",    StringValue(DATA_MODE),
                                     "ControlMode", StringValue(CTL_MODE));

    // -----------------------------------------------------------------------
    //  MAC helper
    //  No multi-user scheduler: the latency scenario studies contention
    //  effects (queuing delay vs. #STAs) using SU round-robin scheduling,
    //  which is the model in [16].  DL OFDMA is not assumed.
    //  Using RrMultiUserScheduler with EHT and nStas >= 9 also triggers a
    //  known ns-3 bug (placeholder RuSpec shared by ≥9 candidates causes
    //  WifiTxVector::IsValid() to return false → SIGABRT).
    // -----------------------------------------------------------------------
    WifiMacHelper wifiMac;

    Ssid ssid = Ssid("mlo-lat1");

    // Install AP first
    wifiMac.SetType("ns3::ApWifiMac",
                    "Ssid",            SsidValue(ssid),
                    "BE_MaxAmpduSize", UintegerValue(nMpdus * (payloadSize + 60)),
                    "MpduBufferSize",  UintegerValue(nMpdus));
    NetDeviceContainer apDev = wifi.Install(phy, wifiMac, apNode);
    auto apMac = DynamicCast<WifiNetDevice>(apDev.Get(0))->GetMac();
    apMac->TraceConnectWithoutContext("NAckedMpdu", MakeCallback(&CountNack));
    apMac->TraceConnectWithoutContext("AckedMpdu", MakeCallback(&CountAck));

    // Install STAs
    wifiMac.SetType("ns3::StaWifiMac",
                    "Ssid",          SsidValue(ssid),
                    "ActiveProbing", BooleanValue(false),
                    "BE_MaxAmpduSize", UintegerValue(nMpdus * (payloadSize + 60)),
                    "MpduBufferSize",  UintegerValue(nMpdus));
    NetDeviceContainer staDev = wifi.Install(phy, wifiMac, staNodes);

    // -----------------------------------------------------------------------
    //  Mobility
    //  AP at origin; STAs arranged on a circle of radius 5 m.
    // -----------------------------------------------------------------------
    MobilityHelper mobility;
    Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator>();
    pos->Add(Vector(0.0, 0.0, 0.0)); // AP

    const double R = 5.0; // m
    for (uint32_t i = 0; i < nStas; ++i)
    {
        double angle = 2.0 * M_PI * i / nStas;
        pos->Add(Vector(R * std::cos(angle), R * std::sin(angle), 0.0));
    }
    mobility.SetPositionAllocator(pos);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(apNode);
    mobility.Install(staNodes);

    // -----------------------------------------------------------------------
    //  Internet stack + IP
    // -----------------------------------------------------------------------
    InternetStackHelper stack;
    stack.Install(apNode);
    stack.Install(staNodes);

    Ipv4AddressHelper addr;
    addr.SetBase("10.1.0.0", "255.255.255.0");
    Ipv4InterfaceContainer apIf  = addr.Assign(apDev);
    Ipv4InterfaceContainer staIf = addr.Assign(staDev);
    WarmUpE2e(apIf, staIf, startupGuard);

    // Exclude ARP discovery and its pending-packet burst from the measured
    // traffic interval.  Direct cache population is required for MLO because
    // NeighborCacheHelper queries WifiNetDevice::GetChannel(), which is
    // intentionally unavailable when a device uses multiple channels.
    for (uint32_t i = 0; i < nStas; ++i)
    {
        AddPermanentArpEntry(apIf, 0, staIf.GetAddress(i), staDev.Get(i)->GetAddress());
        AddPermanentArpEntry(staIf, i, apIf.GetAddress(0), apDev.Get(0)->GetAddress());
    }

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // -----------------------------------------------------------------------
    //  Traffic: total offered load is normalized to single-link SLO peak,
    //  then split equally across STAs.
    //  Reference SLO peaks measured from 4.1.1 with nMpdus=1024:
    //    80 MHz  -> 1120 Mb/s  (remeasured 2026-10-22 at 23 dBm, 5 m)
    //    160 MHz -> 2224 Mb/s  (remeasured 2026-10-22 at 23 dBm, 5 m)
    // -----------------------------------------------------------------------
    const double tStart = startupGuard;
    const double tStop  = simTime + tStart;
    uint16_t     port   = 5000;

    const double singleLinkPeakMbps = (channelWidth == 80) ? 1120.0 : 2224.0;
    const double totalOfferedLoadMbps = offeredLoadMbps >= 0.0 ? offeredLoadMbps : normalizedLoad * singleLinkPeakMbps;
    const double perStaLoadMbps = totalOfferedLoadMbps / static_cast<double>(nStas);

    for (uint32_t i = 0; i < nStas; ++i)
    {
        UdpServerHelper server(port);
        ApplicationContainer sApp = server.Install(staNodes.Get(i));
        sApp.Start(Seconds(tStart));
        sApp.Stop(Seconds(tStop + drainTime));

        if (perStaLoadMbps > 0.0)
        {
            UdpClientHelper client(staIf.GetAddress(i), port);
            client.SetAttribute("MaxPackets", UintegerValue(0xFFFFFFFF));
            const double pktBits = static_cast<double>(payloadSize) * 8.0;
            const double intervalUs = pktBits / perStaLoadMbps;
            // Preserve fractional microseconds at the simulator time resolution.
            client.SetAttribute("Interval", TimeValue(Seconds(intervalUs * 1e-6)));
            client.SetAttribute("PacketSize", UintegerValue(payloadSize));
            ApplicationContainer cApp = client.Install(apNode.Get(0));
            cApp.Start(Seconds(tStart));
            cApp.Stop(Seconds(tStop));
        }
        ++port;
    }

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
    std::vector<uint64_t> bins;
    for (auto& kv : stats)
    {
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(kv.first);
        // DL flow only: source = AP (10.1.0.1)
        if (t.sourceAddress != apIf.GetAddress(0) || t.destinationPort == 49000)
            continue;
        totalTxPackets += kv.second.txPackets;
        totalLostPackets += kv.second.lostPackets;
        totalDelaySec += kv.second.delaySum.GetSeconds();
        totalRxPackets += kv.second.rxPackets;
        totalRxBytes += kv.second.rxBytes;
        const auto& histogram = kv.second.delayHistogram;
        bins.resize(std::max<size_t>(bins.size(), histogram.GetNBins()), 0);
        for (uint32_t i = 0; i < histogram.GetNBins(); ++i)
            bins[i] += histogram.GetBinCount(i);
    }
    double meanLatencyMs = (totalRxPackets > 0)
                               ? (totalDelaySec / totalRxPackets) * 1000.0
                               : 0.0;
    double meanThrMbps = (simTime > 0.0)
                             ? (totalRxBytes * 8.0) / (simTime * 1e6)
                             : 0.0;

    // Always emit results; flag with SATURATED when delivered < 95 % of offered
    // so plotters can render those points with a distinct (hollow) marker.
    double deliveryRatio = (totalTxPackets > 0)
                               ? static_cast<double>(totalRxPackets) / totalTxPackets
                               : 1.0;
    if (deliveryRatio < 0.95)
    {
        std::cout << "SATURATED: delivery_ratio=" << deliveryRatio
                  << " offered=" << totalOfferedLoadMbps << " Mb/s"
                  << " delivered=" << meanThrMbps << " Mb/s\n";
    }

    std::cout << "\nMean DL Latency: "    << meanLatencyMs << " ms\n";
    std::cout << "Mean DL Throughput: " << meanThrMbps   << " Mb/s\n";
    PrintE2ePercentiles(bins, 0.0001);
    std::cout << "DL Nacked MPDU events: " << nackedMpdus << "\n";
    std::cout << "DL Acked retry MPDUs: " << ackedRetryMpdus << "\n";
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
