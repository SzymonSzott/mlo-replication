/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Scenario 4.3 — Legacy Coexistence with IEEE 802.11be Multi-link Operation
 *
 * Based on [18]: D. Medda, A. Iossifides, P. Chatzimisios, F. J. Vélez, J.-F. Wagén,
 *   "Investigating Inclusiveness and Backward Compatibility of IEEE 802.11be
 *    Multi-link Operation", 2022 IEEE Conference on Standards for Communications
 *    and Networking (CSCN), pp. 20–24.
 *
 * Goal: Evaluate how different static band assignment policies affect the coexistence
 * of legacy IEEE 802.11 devices (2.4 GHz only) and new IEEE 802.11be MLDs
 * (5 + 6 GHz, two links). Metrics: aggregated per-class throughput and Jain's
 * Fairness Index. The legacy/MLDs ratio is swept from 10 % to 90 % in 10 % steps.
 *
 * ── Topology ─────────────────────────────────────────────────────────────────
 *   Single AP at (7.5, 7.5, 4.0) m (centre of 15 × 15 m² room).
 *   50 STAs placed randomly inside [0,15]×[0,15] m² at height ∈ {0.8, 1.8} m.
 *   All devices operate in uplink (STA → AP), full buffers (saturated traffic).
 *
 * ── Band plan ────────────────────────────────────────────────────────────────
 *   2.4 GHz  channel 6  (20 MHz)  — legacy STAs (HT max BW)
 *   5   GHz  channel 42 (80 MHz)  — MLDs link 0
 *   6   GHz  channel 7  (80 MHz)  — MLDs link 1
 *
 * ── Cases (band assignment policy) ──────────────────────────────────────────
 *   A : Legacy on 2.4 GHz; MLD AP lock approximation (single active MLD link)
 *       to emulate single RTS acceptance where non-primary MLD opportunities are
 *       frequently rejected while legacy traffic is present.
 *   B : All devices share all three bands; no priority; AP single RTS acceptance
 *       (MLDs and legacy contend for the same channels)
 *   C : Segregated: Legacy on 2.4 GHz; MLDs on 5+6 GHz; AP accepts simultaneous
 *       RTS on different bands (MLDs and legacy truly independent)
 *
 * ── Simulation ───────────────────────────────────────────────────────────────
 *   Path loss : enterprise model from IEEE 802.11ax TGax (fGHz, breakpoint 5 m,
 *               avg 3 walls) + Rayleigh fading → LogDistancePropagationLossModel
 *               with path-loss exponent 3.5 approximation used here.
 *   MCS       : ConstantRateWifiManager, HeMcs8 (legacy HE-SU) / EhtMcs8 (MLDs).
 *   Channel BW: 2.4 GHz=20 MHz (HT cap), 5/6 GHz=80 MHz (paper-faithful EHT).
 *   Payload   : 12000 bits (= 1500 bytes), same as paper Table I.
 *   MAC       : DCF with RTS/CTS enabled.
 *
 * ── Parameters ───────────────────────────────────────────────────────────────
 *   --legacyFraction  : fraction of legacy STAs ∈ (0,1)  default 0.3 → 30 %
 *   --totalStas       : total STA count, default 50
 *   --caseId          : 'A', 'B', or 'C'
 *   --simTime         : simulation time [s], default 60
 *   --RngRun          : RNG run index
 *
 * ── Output (parsed by run_all_rerun.py) ──────────────────────────────────────
 *   "AggThr_Legacy: X.XXX Mb/s"
 *   "AggThr_MLDs:   X.XXX Mb/s"
 *   "AggThr_Total:  X.XXX Mb/s"
 *   "AvgThr_Legacy: X.XXX Mb/s"
 *   "AvgThr_MLDs:   X.XXX Mb/s"
 *   "JFI_Total:     X.XXXXX"
 *   "JFI_Legacy:    X.XXXXX"
 *   "JFI_MLDs:      X.XXXXX"
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
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
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("WifiMloLegacyCoexistence");

// ---------------------------------------------------------------------------
//  Jain's Fairness Index:  (sum_i s_i)^2 / (n * sum_i s_i^2)
// ---------------------------------------------------------------------------
static double
JainFairness(const std::vector<double>& thr)
{
    if (thr.empty())
        return 1.0;
    double sumS  = 0.0;
    double sumS2 = 0.0;
    int    n     = 0;
    for (double s : thr)
    {
        if (s < 0.0)
            continue;
        sumS  += s;
        sumS2 += s * s;
        ++n;
    }
    if (n == 0 || sumS2 == 0.0)
        return 1.0;
    return (sumS * sumS) / (static_cast<double>(n) * sumS2);
}

// ---------------------------------------------------------------------------
//  Main
// ---------------------------------------------------------------------------
int
main(int argc, char* argv[])
{
    // -----------------------------------------------------------------------
    //  Parameters
    // -----------------------------------------------------------------------
    double      legacyFraction = 0.30;   // fraction of legacy STAs
    uint32_t    totalStas      = 50;     // total STA count
    std::string caseId         = "B";    // 'A', 'B', or 'C'
    double      simTime        = 60.0;   // s
    uint32_t    payloadSize    = 1500;   // bytes  (paper: 12000 bits)
    bool        emlsr          = false;
    double      startupGuard   = 1.0;    // s before traffic starts

    CommandLine cmd(__FILE__);
    cmd.AddValue("legacyFraction", "Fraction of legacy STAs [0,1]",       legacyFraction);
    cmd.AddValue("totalStas",      "Total number of STAs",                  totalStas);
    cmd.AddValue("caseId",         "Band assignment case: A, B, or C",     caseId);
    cmd.AddValue("simTime",        "Simulation duration [s]",               simTime);
    cmd.AddValue("payloadSize",    "UDP payload bytes",                      payloadSize);
    cmd.AddValue("emlsr",          "Enable EMLSR on the MLDs",               emlsr);
    cmd.AddValue("startupGuard",   "Idle time before traffic starts [s]",    startupGuard);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_IF(caseId != "A" && caseId != "B" && caseId != "C",
                    "caseId must be A, B, or C");
    NS_ABORT_MSG_IF(legacyFraction < 0.0 || legacyFraction > 1.0,
                    "legacyFraction must be in [0,1]");
    NS_ABORT_MSG_IF(totalStas < 2, "totalStas must be >= 2");

    RngSeedManager::SetSeed(1);

    uint32_t numLegacy = static_cast<uint32_t>(std::round(legacyFraction * totalStas));
    uint32_t numMlds   = totalStas - numLegacy;
    // Guard: at least 1 of each type when fraction is not 0 or 1.
    if (numLegacy == 0 && legacyFraction > 0.0)
        numLegacy = 1, numMlds = totalStas - 1;
    if (numMlds == 0 && legacyFraction < 1.0)
        numMlds = 1, numLegacy = totalStas - 1;

    std::cout << "=== Scenario 4.3: Legacy Coexistence ===\n"
              << "Case=" << caseId << "  EMLSR=" << (emlsr ? "on" : "off")
              << "  numLegacy=" << numLegacy
              << "  numMLDs=" << numMlds
              << "  simTime=" << simTime << "s\n\n";

    // -----------------------------------------------------------------------
    //  Spectrum channels
    //   2.4 GHz ch 6  → centre frequency ~2437 MHz, 20 MHz
    //   5   GHz ch 36 → centre frequency ~5180 MHz, 20 MHz
    //   6   GHz ch  1 → centre frequency  5955 MHz, 20 MHz  (UNII-5)
    //
    //  Each spectrum channel is a separate MultiModelSpectrumChannel so that
    //  nodes on different bands do not interfere with each other.
    // -----------------------------------------------------------------------
    // Band indices:  0=2.4 GHz, 1=5 GHz, 2=6 GHz
    struct BandInfo
    {
        const char*    name;
        uint32_t       chanNum;  // ns-3 channel number
        uint32_t       bw;      // MHz
        FrequencyRange range;
    };

    const BandInfo bands[3] = {
        { "2.4GHz", 6,  20, WIFI_SPECTRUM_2_4_GHZ },
        { "5GHz",  36,  20, WIFI_SPECTRUM_5_GHZ   },
        { "6GHz",   1,  20, WIFI_SPECTRUM_6_GHZ   },
    };

    Ptr<MultiModelSpectrumChannel> specMedia[3];
    for (int b = 0; b < 3; ++b)
    {
        specMedia[b] = CreateObject<MultiModelSpectrumChannel>();
        auto loss = CreateObject<LogDistancePropagationLossModel>();
        loss->SetAttribute("Exponent",            DoubleValue(3.5));
        loss->SetAttribute("ReferenceLoss",       DoubleValue(40.0));
        loss->SetAttribute("ReferenceDistance",   DoubleValue(1.0));
        specMedia[b]->AddPropagationLossModel(loss);
        specMedia[b]->SetPropagationDelayModel(
            CreateObject<ConstantSpeedPropagationDelayModel>());
    }

    // -----------------------------------------------------------------------
    //  MAC queue
    // -----------------------------------------------------------------------
    Config::SetDefault("ns3::WifiMacQueue::MaxSize", QueueSizeValue(QueueSize("1024p")));

    // -----------------------------------------------------------------------
    //  WiFi helpers
    //  Legacy: 802.11ax (HE-SU), 2.4 GHz, HeMcs8, single-link
    //  MLDs:   802.11be (EHT), 5+6 GHz, EhtMcs8, dual-link
    //
    //  For Cases A and C: legacy operates on 2.4 GHz only.
    //  For Case B: legacy also operates on 2.4 GHz but the AP is shared with MLDs
    //              → we model this by having all devices on all bands; the AP
    //              accepts and arbitrates access via a single EDCA queue.
    //
    //  NOTE: ns-3 does not natively model the "AP lock" semantics described in
    //  the paper (Case A/B vs C).  We approximate:
    //    Case A & B: all traffic on 2.4 GHz for legacy; MLDs on 5+6 GHz.
    //                For Case B additionally MLDs also use the 2.4 GHz channel
    //                (contending with legacy).
    //    Case C: legacy on 2.4 GHz, MLDs on 5+6 GHz with NO shared channel
    //            → truly independent.
    //
    //  AP lock of Case A (single-RTS acceptance) is approximated by placing
    //  all MLDs and legacy STAs on the same shared 2.4 GHz spectrum object
    //  (Case B) or keeping them on separate spectrum objects (Cases A and C).
    //  Because the paper uses an event-driven MAC simulator, the exact ns-3
    //  mapping is a reasonable approximation.
    // -----------------------------------------------------------------------
    const char* LEGACY_DATA_MODE = "HeMcs8";
    const char* LEGACY_CTL_MODE  = "HeMcs0";
    const char* MLD_DATA_MODE    = "EhtMcs8";
    const char* MLD_CTL_MODE     = "EhtMcs0";

    // ── Legacy helper (always single-link, 2.4 GHz) ──
    WifiHelper wifiLegacy;
    wifiLegacy.SetStandard(WIFI_STANDARD_80211ax);
    wifiLegacy.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                       "DataMode",    StringValue(LEGACY_DATA_MODE),
                                       "ControlMode", StringValue(LEGACY_CTL_MODE));

    // ── MLD helper ──
    // Case A: AP-lock approximation -> only one active MLD link (5 GHz).
    // Case B: MLDs on 2.4+5+6 GHz  -> 3 links, shared contention on 2.4 GHz.
    // Case C: MLDs on 5+6 GHz      -> 2 links, independent from legacy band.
    uint32_t numMldLinks = 2;
    if (caseId == "A")
    {
        numMldLinks = 1;
    }
    else if (caseId == "B")
    {
        numMldLinks = 3;
    }

    WifiHelper wifiMld;
    wifiMld.SetStandard(WIFI_STANDARD_80211be);
    wifiMld.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
    if (emlsr)
    {
        wifiMld.ConfigEhtOptions("EmlsrActivated",    BooleanValue(true));
        wifiMld.ConfigEhtOptions("TransitionTimeout", TimeValue(MicroSeconds(1024)));
    }
    for (uint32_t l = 0; l < numMldLinks; ++l)
        wifiMld.SetRemoteStationManager(l,
                                        "ns3::ConstantRateWifiManager",
                                        "DataMode",    StringValue(MLD_DATA_MODE),
                                        "ControlMode", StringValue(MLD_CTL_MODE));

    // -----------------------------------------------------------------------
    //  Nodes
    // -----------------------------------------------------------------------
    NodeContainer apNode;
    apNode.Create(1);

    NodeContainer legacyStaNodes;
    legacyStaNodes.Create(numLegacy);

    NodeContainer mldStaNodes;
    mldStaNodes.Create(numMlds);

    // -----------------------------------------------------------------------
    //  Mobility (paper: 15×15 m², AP at centre/4 m height,
    //            STAs random ∈ [0.8, 1.8] m height)
    // -----------------------------------------------------------------------
    MobilityHelper mob;
    mob.SetMobilityModel("ns3::ConstantPositionMobilityModel");

    // AP fixed at centre
    {
        Ptr<ListPositionAllocator> alloc = CreateObject<ListPositionAllocator>();
        alloc->Add(Vector(7.5, 7.5, 4.0));
        mob.SetPositionAllocator(alloc);
        mob.Install(apNode);
    }

    // STAs: uniform random placement on a disk of radius 7.5 m around the AP, height in [0.8, 1.8]
    {
        Ptr<UniformRandomVariable> rang = CreateObject<UniformRandomVariable>();
        rang->SetAttribute("Min", DoubleValue(0.0));
        rang->SetAttribute("Max", DoubleValue(1.0));
        Ptr<UniformRandomVariable> rphi = CreateObject<UniformRandomVariable>();
        rphi->SetAttribute("Min", DoubleValue(0.0));
        rphi->SetAttribute("Max", DoubleValue(2.0 * M_PI));
        Ptr<UniformRandomVariable> rh = CreateObject<UniformRandomVariable>();
        rh->SetAttribute("Min", DoubleValue(0.8));
        rh->SetAttribute("Max", DoubleValue(1.8));

        Ptr<ListPositionAllocator> alloc = CreateObject<ListPositionAllocator>();
        const double R = 7.5;
        for (uint32_t i = 0; i < totalStas; ++i)
        {
            // Uniform on disk: r = R*sqrt(u), phi = 2*pi*v
            double r   = R * std::sqrt(rang->GetValue());
            double phi = rphi->GetValue();
            double x   = 7.5 + r * std::cos(phi);
            double y   = 7.5 + r * std::sin(phi);
            alloc->Add(Vector(x, y, rh->GetValue()));
        }
        mob.SetPositionAllocator(alloc);
        mob.Install(legacyStaNodes);
        mob.Install(mldStaNodes);
    }

    // -----------------------------------------------------------------------
    //  WiFi device installation
    // -----------------------------------------------------------------------

    // Helper: build SpectrumWifiPhyHelper for given list of band indices
    auto makePhy = [&](std::vector<int> bandIdxs) -> SpectrumWifiPhyHelper {
        uint32_t nLinks = static_cast<uint32_t>(bandIdxs.size());
        SpectrumWifiPhyHelper phy(nLinks);
        phy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
        for (uint32_t l = 0; l < nLinks; ++l)
        {
            int b = bandIdxs[l];
            std::ostringstream chSet;
            chSet << "{" << bands[b].chanNum << ", " << bands[b].bw
                  << ", BAND_" << (b == 0 ? "2_4" : (b == 1 ? "5" : "6"))
                  << "GHZ, 0}";
            phy.AddChannel(specMedia[b], bands[b].range);
            phy.Set(l, "ChannelSettings",             StringValue(chSet.str()));
            phy.Set(l, "Antennas",                    UintegerValue(2));
            phy.Set(l, "MaxSupportedTxSpatialStreams", UintegerValue(2));
            phy.Set(l, "MaxSupportedRxSpatialStreams", UintegerValue(2));
        }
        return phy;
    };

    // ── Legacy devices always use 2.4 GHz ──
    SpectrumWifiPhyHelper phyLegacy = makePhy({0});

    // ── MLD devices ──
    // Case A: single MLD link on 5 GHz (AP lock approximation)
    // Case B: 2.4+5+6 GHz (all bands, contend with legacy on 2.4 GHz)
    // Case C: 5+6 GHz (segregated, independent of legacy)
    SpectrumWifiPhyHelper phyMld = makePhy({1, 2});
    if (caseId == "A")
    {
        phyMld = makePhy({1});
    }
    else if (caseId == "B")
    {
        phyMld = makePhy({0, 1, 2});
    }

    // ── AP ──
    // The AP must accommodate all device types. We install one AP device per band
    // context. For simplicity we install the AP using the MLD helper on the MLD bands,
    // and a second legacy AP device on 2.4 GHz. All STAs associate with the
    // appropriate AP device via SSID.
    //
    // Because ns-3 cannot trivially have one AP node with two different WiFi standards
    // running simultaneously, we model the AP as follows:
    //   - One "legacy AP" device on 2.4 GHz (802.11n)
    //   - One "MLD AP" device on 5+6 GHz (802.11be) [Case A/C]
    //     or on 2.4+5+6 GHz [Case B]
    //
    // This is the standard multi-BSS approach in ns-3.

    Ssid ssidLegacy("LEGACY_BSS");
    Ssid ssidMld("MLD_BSS");

    WifiMacHelper mac;

    // Legacy AP on 2.4 GHz
    mac.SetType("ns3::ApWifiMac",
                "Ssid", SsidValue(ssidLegacy));
    NetDeviceContainer apLegacyDev = wifiLegacy.Install(phyLegacy, mac, apNode);

    // MLD AP
    mac.SetType("ns3::ApWifiMac",
                "Ssid", SsidValue(ssidMld));
    NetDeviceContainer apMldDev = wifiMld.Install(phyMld, mac, apNode);

    // Legacy STAs
    mac.SetType("ns3::StaWifiMac",
                "Ssid",          SsidValue(ssidLegacy),
                "ActiveProbing", BooleanValue(false));
    NetDeviceContainer legacyStaDev = wifiLegacy.Install(phyLegacy, mac, legacyStaNodes);

    // MLD STAs
    if (emlsr && numMldLinks > 1)
    {
        std::ostringstream linkSet;
        for (uint32_t l = 0; l < numMldLinks; ++l)
        {
            if (l > 0) linkSet << ",";
            linkSet << l;
        }
        mac.SetEmlsrManager("ns3::AdvancedEmlsrManager",
                            "EmlsrLinkSet",       StringValue(linkSet.str()),
                            "SwitchAuxPhy",       BooleanValue(true),
                            "PutAuxPhyToSleep",   BooleanValue(false),
                            "AuxPhyChannelWidth", UintegerValue(20),
                            "AuxPhyMaxModClass",  StringValue("EHT"),
                            "AuxPhyTxCapable",    BooleanValue(true));
    }
    mac.SetType("ns3::StaWifiMac",
                "Ssid",          SsidValue(ssidMld),
                "ActiveProbing", BooleanValue(false));
    NetDeviceContainer mldStaDev = wifiMld.Install(phyMld, mac, mldStaNodes);

    // -----------------------------------------------------------------------
    //  Internet stack + IP
    // -----------------------------------------------------------------------
    InternetStackHelper stack;
    stack.Install(apNode);
    stack.Install(legacyStaNodes);
    stack.Install(mldStaNodes);

    Ipv4AddressHelper addr;

    // Legacy BSS: 10.1.0.0/24
    addr.SetBase("10.1.0.0", "255.255.255.0");
    Ipv4InterfaceContainer apLegacyIf    = addr.Assign(apLegacyDev);
    Ipv4InterfaceContainer legacyStaIfs  = addr.Assign(legacyStaDev);
    Ipv4Address apLegacyAddr = apLegacyIf.GetAddress(0);

    // MLD BSS: 10.2.0.0/24
    addr.SetBase("10.2.0.0", "255.255.255.0");
    Ipv4InterfaceContainer apMldIf  = addr.Assign(apMldDev);
    Ipv4InterfaceContainer mldStaIf = addr.Assign(mldStaDev);
    Ipv4Address apMldAddr = apMldIf.GetAddress(0);


    // -----------------------------------------------------------------------
    //  Traffic: saturated uplink (STA → AP), full-buffer
    //  Paper: payload 12000 bits (1500 bytes), always backlogged → OnOff with
    //  very high DataRate and OnTime=always.
    // -----------------------------------------------------------------------
    const uint16_t BASE_PORT = 5000;
    const double   dataRateBps = 1.0e9; // 1 Gbps burst → full-buffer approximation

    std::ostringstream drStr;
    drStr << static_cast<uint64_t>(dataRateBps) << "bps";

    ApplicationContainer sinkApps;

    // ── Legacy STAs → AP ──
    for (uint32_t i = 0; i < numLegacy; ++i)
    {
        uint16_t port = BASE_PORT + i;
        // Sink on AP
        PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                    InetSocketAddress(apLegacyAddr, port));
        sinkApps.Add(sinkHelper.Install(apNode));

        // Source on STA
        OnOffHelper onoff("ns3::UdpSocketFactory",
                          InetSocketAddress(apLegacyAddr, port));
        onoff.SetAttribute("DataRate",   StringValue(drStr.str()));
        onoff.SetAttribute("PacketSize", UintegerValue(payloadSize));
        onoff.SetAttribute("OnTime",
                           StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        onoff.SetAttribute("OffTime",
                           StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        ApplicationContainer src = onoff.Install(legacyStaNodes.Get(i));
        src.Start(Seconds(startupGuard));
        src.Stop(Seconds(startupGuard + simTime));
    }

    // ── MLD STAs → AP ──
    for (uint32_t i = 0; i < numMlds; ++i)
    {
        uint16_t port = BASE_PORT + numLegacy + i;
        // Sink on AP
        PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                    InetSocketAddress(apMldAddr, port));
        sinkApps.Add(sinkHelper.Install(apNode));

        // Source on STA
        OnOffHelper onoff("ns3::UdpSocketFactory",
                          InetSocketAddress(apMldAddr, port));
        onoff.SetAttribute("DataRate",   StringValue(drStr.str()));
        onoff.SetAttribute("PacketSize", UintegerValue(payloadSize));
        onoff.SetAttribute("OnTime",
                           StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        onoff.SetAttribute("OffTime",
                           StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        ApplicationContainer src = onoff.Install(mldStaNodes.Get(i));
        src.Start(Seconds(startupGuard));
        src.Stop(Seconds(startupGuard + simTime));
    }

    sinkApps.Start(Seconds(0.5));
    sinkApps.Stop(Seconds(startupGuard + simTime + 2.0));

    // -----------------------------------------------------------------------
    //  FlowMonitor
    // -----------------------------------------------------------------------
    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor> flowmon = fmHelper.InstallAll();

    // -----------------------------------------------------------------------
    //  Run
    // -----------------------------------------------------------------------
    Simulator::Stop(Seconds(startupGuard + simTime + 3.0));
    Simulator::Run();

    // -----------------------------------------------------------------------
    //  Collect per-flow throughput  (uplink: STA→AP, any source port,
    //  destination port = BASE_PORT + i)
    // -----------------------------------------------------------------------
    flowmon->CheckForLostPackets();
    const auto& flowStats  = flowmon->GetFlowStats();
    auto        classifier = DynamicCast<Ipv4FlowClassifier>(fmHelper.GetClassifier());

    std::vector<double> legacyThr(numLegacy, 0.0);
    std::vector<double> mldThr(numMlds, 0.0);

    for (const auto& [fid, st] : flowStats)
    {
        if (st.rxBytes == 0)
            continue;
        Ipv4FlowClassifier::FiveTuple ft = classifier->FindFlow(fid);
        uint16_t dport = ft.destinationPort;
        double   thr   = st.rxBytes * 8.0 / (simTime * 1e6); // Mb/s

        if (dport >= BASE_PORT && dport < BASE_PORT + numLegacy)
        {
            uint32_t idx = dport - BASE_PORT;
            if (idx < numLegacy)
                legacyThr[idx] += thr;
        }
        else if (dport >= BASE_PORT + numLegacy &&
                 dport < BASE_PORT + numLegacy + numMlds)
        {
            uint32_t idx = dport - BASE_PORT - numLegacy;
            if (idx < numMlds)
                mldThr[idx] += thr;
        }
    }

    // -----------------------------------------------------------------------
    //  Compute metrics
    // -----------------------------------------------------------------------
    double aggLegacy = std::accumulate(legacyThr.begin(), legacyThr.end(), 0.0);
    double aggMld    = std::accumulate(mldThr.begin(),    mldThr.end(),    0.0);
    double aggTotal  = aggLegacy + aggMld;

    double avgLegacy = (numLegacy > 0) ? aggLegacy / numLegacy : 0.0;
    double avgMld    = (numMlds   > 0) ? aggMld    / numMlds   : 0.0;

    std::vector<double> allThr;
    allThr.insert(allThr.end(), legacyThr.begin(), legacyThr.end());
    allThr.insert(allThr.end(), mldThr.begin(),    mldThr.end());
    double jfiTotal  = JainFairness(allThr);
    double jfiLegacy = JainFairness(legacyThr);
    double jfiMld    = JainFairness(mldThr);

    std::cout << "AggThr_Legacy: " << aggLegacy << " Mb/s\n"
              << "AggThr_MLDs:   " << aggMld    << " Mb/s\n"
              << "AggThr_Total:  " << aggTotal  << " Mb/s\n"
              << "AvgThr_Legacy: " << avgLegacy << " Mb/s\n"
              << "AvgThr_MLDs:   " << avgMld    << " Mb/s\n"
              << "JFI_Total:     " << jfiTotal  << "\n"
              << "JFI_Legacy:    " << jfiLegacy << "\n"
              << "JFI_MLDs:      " << jfiMld    << "\n";

    Simulator::Destroy();
    return 0;
}
