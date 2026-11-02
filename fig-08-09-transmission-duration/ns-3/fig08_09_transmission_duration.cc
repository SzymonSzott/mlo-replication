/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Throughput Scenario #4  (based on [19])
 *
 * Purpose: evaluate the impact of MPDU aggregation settings and TXOP limit
 * on throughput for a 2-link STR station.
 *
 * Main BSS: 1 AP + 1 STA, STR, DL only (full-buffer UDP, AP→STA)
 *   Link 0: 5 GHz ch42  80 MHz  ("Channel 1"   in the paper)
 *   Link 1: 5 GHz ch106 80 MHz  ("Channel 100" in the paper)
 *
 * OBSS interference (channel occupancy model):
 *   Link 1 (ch106): 1 BSS (AP+STA) with rate = ch2OccPct/100 × CH_CAPACITY  (always)
 *   Link 0 (ch42):  1 BSS (AP+STA) with rate = ch1OccPct/100 × CH_CAPACITY  (if ch1OccPct>0)
 *
 * PHY: EHT 802.11be, MCS 11, 2 SS, GI 800 ns, 80 MHz
 * A-MPDU: nMpdus ∈ {64, 512, 1024}  (Block Ack buffer = AMPDU frame limit)
 * TXOP:   txopLimitMs ∈ {0, 3}       (0 = single-A-MPDU-per-access, 3 ≈ 2976 µs)
 *
 * Both channels share one MultiModelSpectrumChannel (WIFI_SPECTRUM_5_GHZ);
 * WifiBandwidthFilter provides ch42/ch106 frequency isolation.
 * TXOP limit is rounded to nearest 32 µs multiple (required by 802.11):
 *   0 ms → 0 µs,  3 ms → 2976 µs (93×32 µs).
 *
 * Output line parsed by runner:
 *   "MainBSS DL MacRx: X.XX Mb/s"
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

#include <cstdint>
#include <iostream>
#include <string>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("WifiMloScenario4");

// MacRx byte counter – main BSS STA (DL only)
static uint64_t g_dlBytes = 0;

static void
MacRxDl(std::string /*ctx*/, Ptr<const Packet> pkt)
{
    g_dlBytes += pkt->GetSize();
}

int
main(int argc, char* argv[])
{
    // -----------------------------------------------------------------------
    //  Parameters
    // -----------------------------------------------------------------------
    double   simTime     = 10.0;
    uint32_t payloadSize = 1500;
    uint32_t nMpdus      = 1024;  // A-MPDU frame limit: 64 | 512 | 1024
    double   txopLimitMs = 0.0;   // TXOP limit [ms]: 0 or 3
    double   ch2OccPct   = 10.0;  // ch106 occupancy [%]  10..70
    double   ch1OccPct   = 0.0;   // ch42  occupancy [%]  0 or 10

    CommandLine cmd(__FILE__);
    cmd.AddValue("simTime",     "Simulation time [s]",                   simTime);
    cmd.AddValue("payloadSize", "UDP payload [B]",                       payloadSize);
    cmd.AddValue("nMpdus",      "A-MPDU frame limit (64|512|1024)",      nMpdus);
    cmd.AddValue("txopLimitMs", "TXOP limit [ms] (0 or 3)",              txopLimitMs);
    cmd.AddValue("ch2OccPct",   "ch106 occupancy % (10-70)",             ch2OccPct);
    cmd.AddValue("ch1OccPct",   "ch42  occupancy % (0 or 10)",           ch1OccPct);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_IF(nMpdus != 64 && nMpdus != 512 && nMpdus != 1024,
                    "nMpdus must be 64, 512 or 1024");
    NS_ABORT_MSG_IF(txopLimitMs != 0.0 && txopLimitMs != 3.0,
                    "txopLimitMs must be 0 or 3");
    NS_ABORT_MSG_IF(ch2OccPct < 10.0 || ch2OccPct > 70.0,
                    "ch2OccPct must be 10..70");
    NS_ABORT_MSG_IF(ch1OccPct != 0.0 && ch1OccPct != 10.0,
                    "ch1OccPct must be 0 or 10");

    // Round TXOP limit to nearest 32 µs (802.11 constraint).
    // 0 ms → 0 µs (unlimited), 3 ms → 2976 µs (93 × 32 µs).
    const Time txopLimit = (txopLimitMs == 0.0)
        ? MicroSeconds(0)
        : MicroSeconds(static_cast<uint64_t>(txopLimitMs * 1000.0 / 32.0) * 32);

    RngSeedManager::SetSeed(1);

    std::cout << "=== Scenario 4 ===\n"
              << "nMpdus=" << nMpdus
              << "  txopLimit=" << txopLimit.GetMicroSeconds() << "us"
              << "  ch2Occ=" << ch2OccPct << "%"
              << "  ch1Occ=" << ch1OccPct << "%\n\n";

    // -----------------------------------------------------------------------
    //  Nodes
    // -----------------------------------------------------------------------
    NodeContainer mainAp;  mainAp.Create(1);
    NodeContainer mainSta; mainSta.Create(1);

    // OBSS on ch106 (link 1, always present)
    NodeContainer obss2Ap;  obss2Ap.Create(1);
    NodeContainer obss2Sta; obss2Sta.Create(1);

    // OBSS on ch42 (link 0, only when ch1OccPct > 0)
    bool hasObss1 = (ch1OccPct > 0.0);
    NodeContainer obss1Ap;  if (hasObss1) obss1Ap.Create(1);
    NodeContainer obss1Sta; if (hasObss1) obss1Sta.Create(1);

    // -----------------------------------------------------------------------
    //  Spectrum channels — ONE per frequency (ch42 / ch106) so each frequency
    //  has its own event scheduler. Sharing a single channel object across
    //  the two main-BSS links serialises STR PHYs in ns-3 and kills parallel
    //  transmission. Co-channel BSSs share the same object so they still
    //  contend (CSMA/CA); cross-channel BSSs do not.
    // -----------------------------------------------------------------------
    auto makeChannel = []() {
        auto ch = CreateObject<MultiModelSpectrumChannel>();
        Ptr<LogDistancePropagationLossModel> loss = CreateObject<LogDistancePropagationLossModel>();
        loss->SetAttribute("Exponent",          DoubleValue(3.5));
        loss->SetAttribute("ReferenceLoss",     DoubleValue(40.0));
        loss->SetAttribute("ReferenceDistance", DoubleValue(1.0));
        ch->AddPropagationLossModel(loss);
        ch->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());
        return ch;
    };
    auto ch42  = makeChannel();
    auto ch106 = makeChannel();

    auto cfgLink = [](SpectrumWifiPhyHelper& phy,
                      uint32_t linkId,
                      Ptr<MultiModelSpectrumChannel> ch,
                      const FrequencyRange& range,
                      const char* chanSettings)
    {
        phy.AddChannel(ch, range);
        phy.Set(linkId, "ChannelSettings",              StringValue(chanSettings));
        phy.Set(linkId, "Antennas",                     UintegerValue(2));
        phy.Set(linkId, "MaxSupportedTxSpatialStreams",  UintegerValue(2));
        phy.Set(linkId, "MaxSupportedRxSpatialStreams",  UintegerValue(2));
    };

    // Main BSS: 2 links — link 0 on ch42 channel, link 1 on ch106 channel
    SpectrumWifiPhyHelper phyMain(2);
    phyMain.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
    phyMain.SetPcapCaptureType(WifiPhyHelper::PcapCaptureType::PCAP_PER_LINK);
    cfgLink(phyMain, 0, ch42,  WIFI_SPECTRUM_5_GHZ, "{42,  80, BAND_5GHZ, 0}");
    cfgLink(phyMain, 1, ch106, WIFI_SPECTRUM_5_GHZ, "{106, 80, BAND_5GHZ, 0}");

    // OBSS on ch106 — single link, shares the ch106 channel object with main link 1
    SpectrumWifiPhyHelper phyObss2(1);
    phyObss2.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
    cfgLink(phyObss2, 0, ch106, WIFI_SPECTRUM_5_GHZ, "{106, 80, BAND_5GHZ, 0}");

    // OBSS on ch42 — single link, shares the ch42 channel object with main link 0
    SpectrumWifiPhyHelper phyObss1(1);
    phyObss1.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
    cfgLink(phyObss1, 0, ch42, WIFI_SPECTRUM_5_GHZ, "{42, 80, BAND_5GHZ, 0}");

    // -----------------------------------------------------------------------
    //  WiFi helpers
    // -----------------------------------------------------------------------
    const char* DATA_MODE = "EhtMcs11";
    const char* CTL_MODE  = "EhtMcs0";

    // Bound the A-MPDU byte size by the frame-count limit (nMpdus).
    // Using the absolute ns-3 EHT max (6.5 MB) lets a single A-MPDU take
    // ~21 ms at MCS 11; with MLO the NAV duration across two links can then
    // exceed the 802.11 max of 32767 µs, crashing in SetDuration().
    // Per-MPDU overhead: ~4 B delimiter + ~34 B MAC header + 8 B LLC = ~50 B.
    // For nMpdus=1024, payloadSize=1500 → ~1.55 MB → ~5.2 ms at MCS 11 (safe).
    const uint32_t maxAmpduBytes = nMpdus * (payloadSize + 50);

    // Main BSS (STR, 2 links)
    WifiHelper wifiMain;
    wifiMain.SetStandard(WIFI_STANDARD_80211be);
    wifiMain.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
    for (uint8_t l = 0; l < 2; ++l)
        wifiMain.SetRemoteStationManager(l, "ns3::ConstantRateWifiManager",
                                         "DataMode",    StringValue(DATA_MODE),
                                         "ControlMode", StringValue(CTL_MODE));

    // OBSS helpers (single-link; same MCS so they generate realistic frames)
    WifiHelper wifiObss;
    wifiObss.SetStandard(WIFI_STANDARD_80211be);
    wifiObss.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
    wifiObss.SetRemoteStationManager(uint8_t{0}, "ns3::ConstantRateWifiManager",
                                     "DataMode",    StringValue(DATA_MODE),
                                     "ControlMode", StringValue(CTL_MODE));

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
                        "BE_MaxAmpduSize", UintegerValue(maxAmpduBytes),
                        "MpduBufferSize",  UintegerValue(nMpdus));
    };
    auto setStaMac = [&](const char* ssid) {
        wifiMac.SetType("ns3::StaWifiMac",
                        "Ssid",            SsidValue(Ssid(ssid)),
                        "ActiveProbing",   BooleanValue(false),
                        "BE_MaxAmpduSize", UintegerValue(maxAmpduBytes),
                        "MpduBufferSize",  UintegerValue(nMpdus));
    };

    // -----------------------------------------------------------------------
    //  Install devices
    // -----------------------------------------------------------------------
    setApMac("main");
    NetDeviceContainer mainApDev  = wifiMain.Install(phyMain, wifiMac, mainAp);
    setStaMac("main");
    NetDeviceContainer mainStaDev = wifiMain.Install(phyMain, wifiMac, mainSta);

    setApMac("obss2");
    NetDeviceContainer obss2ApDev  = wifiObss.Install(phyObss2, wifiMac, obss2Ap);
    setStaMac("obss2");
    NetDeviceContainer obss2StaDev = wifiObss.Install(phyObss2, wifiMac, obss2Sta);

    NetDeviceContainer obss1ApDev, obss1StaDev;
    if (hasObss1)
    {
        setApMac("obss1");
        obss1ApDev  = wifiObss.Install(phyObss1, wifiMac, obss1Ap);
        setStaMac("obss1");
        obss1StaDev = wifiObss.Install(phyObss1, wifiMac, obss1Sta);
    }

    // -----------------------------------------------------------------------
    //  Apply TXOP limit on main BSS AP and STA (all links).
    //  Always set explicitly so txopLimitMs=0 (unlimited/no-TXOP) is enforced
    //  the same way as txopLimitMs=3, rather than relying on the ns-3 default.
    //  0 µs satisfies the 802.11 constraint (0 % 32 == 0) and means unlimited.
    // -----------------------------------------------------------------------
    {
        for (auto node : {mainAp.Get(0), mainSta.Get(0)})
        {
            auto dev    = DynamicCast<WifiNetDevice>(node->GetDevice(0));
            auto mac    = DynamicCast<WifiMac>(dev->GetMac());
            auto beTxop = DynamicCast<QosTxop>(mac->GetQosTxop(AC_BE));
            std::vector<Time> limits(2, txopLimit);
            beTxop->SetTxopLimits(limits);
        }
    }

    // -----------------------------------------------------------------------
    //  Mobility
    //  Main AP at origin, STA 5 m away (praca_mgr Table 3.6: r = 5 m).
    //  OBSS APs close enough to be in CCA range (< 20 m).
    // -----------------------------------------------------------------------
    MobilityHelper mobility;
    auto pos = CreateObject<ListPositionAllocator>();

    pos->Add(Vector(0.0, 0.0, 0.0));  // main AP
    pos->Add(Vector(5.0, 0.0, 0.0));  // main STA
    pos->Add(Vector(20.0, 0.0, 0.0)); // obss2 AP (ch106)
    pos->Add(Vector(25.0, 0.0, 0.0)); // obss2 STA
    if (hasObss1)
    {
        pos->Add(Vector(0.0, 20.0, 0.0)); // obss1 AP (ch42)
        pos->Add(Vector(0.0, 25.0, 0.0)); // obss1 STA
    }

    mobility.SetPositionAllocator(pos);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(mainAp);
    mobility.Install(mainSta);
    mobility.Install(obss2Ap);
    mobility.Install(obss2Sta);
    if (hasObss1)
    {
        mobility.Install(obss1Ap);
        mobility.Install(obss1Sta);
    }

    // -----------------------------------------------------------------------
    //  Internet stack + IP
    // -----------------------------------------------------------------------
    InternetStackHelper stack;
    stack.Install(mainAp);
    stack.Install(mainSta);
    stack.Install(obss2Ap);
    stack.Install(obss2Sta);
    if (hasObss1)
    {
        stack.Install(obss1Ap);
        stack.Install(obss1Sta);
    }

    Ipv4AddressHelper addr;

    addr.SetBase("10.1.0.0", "255.255.255.0");
    addr.Assign(mainApDev);  // Main AP address (unused in DL-only scenario)
    Ipv4InterfaceContainer mainStaIf = addr.Assign(mainStaDev);

    addr.SetBase("10.2.0.0", "255.255.255.0");
    addr.Assign(obss2ApDev);
    Ipv4InterfaceContainer obss2StaIf = addr.Assign(obss2StaDev);

    Ipv4InterfaceContainer obss1StaIf;
    if (hasObss1)
    {
        addr.SetBase("10.3.0.0", "255.255.255.0");
        addr.Assign(obss1ApDev);
        obss1StaIf = addr.Assign(obss1StaDev);
    }

    // -----------------------------------------------------------------------
    //  Traffic
    //  Main BSS: full-buffer downlink (AP→STA).
    //  OBSS flows: rate-limited to model the requested channel occupancy.
    //
    //  Channel capacity at MCS11, 2SS, 80 MHz, GI=800ns ≈ 2401.9 Mb/s.
    //  OBSS send rate = occPct/100 × CH_CAPACITY (capped at saturation).
    // -----------------------------------------------------------------------
    // Useful single-link goodput (NOT raw PHY rate). Measured from the 4.1.1
    // baseline (1 link, MCS 11, 80 MHz, 2 SS, A-MPDU max 1024). Using the PHY
    // rate (2401.9 Mb/s) here causes OBSS to over-saturate the link at high
    // OccPct, which makes throughput plateau at the single-link baseline
    // instead of degrading linearly with occupancy (cf. praca_mgr Fig 4.4).
    constexpr double CH_CAPACITY_MBPS = 1116.0; // EHT MCS11 2SS 80MHz GI800 useful goodput

    double   tStart = 1.0;
    double   tStop  = simTime + 1.0;
    uint16_t port   = 5000;

    // Helper: add a UDP flow with a given target rate (Mb/s); 0 = saturated
    auto addFlow = [&](Ptr<Node> src, Ptr<Node> dst, Ipv4Address dstAddr, double rateMbps)
    {
        UdpServerHelper server(port);
        auto sApps = server.Install(dst);
        sApps.Start(Seconds(tStart));
        sApps.Stop(Seconds(tStop));

        UdpClientHelper client(dstAddr, port);
        client.SetAttribute("MaxPackets", UintegerValue(0xFFFFFFFF));
        client.SetAttribute("PacketSize", UintegerValue(payloadSize));

        if (rateMbps <= 0.0)
        {
            // Saturated: send as fast as possible
            client.SetAttribute("Interval", TimeValue(MicroSeconds(1)));
        }
        else
        {
            // Paced: compute inter-packet gap from payload size and desired rate
            double pktBits = static_cast<double>(payloadSize) * 8.0;
            double intervalUs = pktBits / rateMbps; // µs per packet
            client.SetAttribute("Interval", TimeValue(MicroSeconds(std::max(1.0, intervalUs))));
        }
        auto cApps = client.Install(src);
        cApps.Start(Seconds(tStart));
        cApps.Stop(Seconds(tStop));
        ++port;
    };

    // Main BSS DL (saturated)
    addFlow(mainAp.Get(0), mainSta.Get(0), mainStaIf.GetAddress(0), 0.0);

    // OBSS2 (ch106): paced to ch2OccPct % of channel capacity
    addFlow(obss2Ap.Get(0), obss2Sta.Get(0), obss2StaIf.GetAddress(0),
            ch2OccPct / 100.0 * CH_CAPACITY_MBPS);

    // OBSS1 (ch42): paced to ch1OccPct % of channel capacity (if present)
    if (hasObss1)
        addFlow(obss1Ap.Get(0), obss1Sta.Get(0), obss1StaIf.GetAddress(0),
                ch1OccPct / 100.0 * CH_CAPACITY_MBPS);

    // -----------------------------------------------------------------------
    //  MacRx trace on main BSS STA (DL)
    // -----------------------------------------------------------------------
    {
        uint32_t staId = mainSta.Get(0)->GetId();
        Config::Connect("/NodeList/" + std::to_string(staId) +
                            "/DeviceList/*/$ns3::WifiNetDevice/Mac/MacRx",
                        MakeCallback(&MacRxDl));
    }

    Simulator::Stop(Seconds(tStop + 1.0));
    Simulator::Run();

    double thr = (simTime > 0.0) ? (g_dlBytes * 8.0) / (simTime * 1e6) : 0.0;
    std::cout << "\nMainBSS DL MacRx: " << thr << " Mb/s\n";

    Simulator::Destroy();
    return 0;
}
