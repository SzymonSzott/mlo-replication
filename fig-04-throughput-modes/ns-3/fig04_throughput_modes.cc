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

NS_LOG_COMPONENT_DEFINE("WifiMloThroughputScenario1");

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

static uint64_t g_macRxBytes = 0;

static void
MacRxTrace(std::string context, Ptr<const Packet> packet)
{
    g_macRxBytes += packet->GetSize();
}

// ============================================================================

int
main(int argc, char* argv[])
{
    uint32_t nAp = 1;
    uint32_t nSta = 1;
    double simTime = 20.0;     // [s]
    uint32_t payloadSize = 1500; // [B]
    uint32_t nMpdus = 1024;   
    uint32_t numLinks = 1;     // 1, 2 lub 3
    std::string mloMode = "SLO"; // SLO, STR, EMLSR

    CommandLine cmd(__FILE__);
    cmd.AddValue("simTime", "Simulation time [s]", simTime);
    cmd.AddValue("payloadSize", "UDP payload size [B]", payloadSize);
    cmd.AddValue("nMpdus", "Number of aggregated MPDUs (not enforced here)", nMpdus);
    cmd.AddValue("numLinks", "Number of links (1, 2, or 3)", numLinks);
    cmd.AddValue("mloMode", "MLO mode: SLO, STR, or EMLSR", mloMode);
    cmd.Parse(argc, argv);

    if (numLinks < 1 || numLinks > 3)
    {
        NS_ABORT_MSG("numLinks must be 1, 2, or 3");
    }
    if (mloMode == "SLO" && numLinks != 1)
    {
        NS_LOG_UNCOND("SLO -> numLinks nadpisane na 1");
        numLinks = 1;
    }
    if (mloMode == "EMLSR" && numLinks <= 1)
    {
        NS_ABORT_MSG("EMLSR requires numLinks > 1");
    }

    std::cout << "Throughput Scenario #1\n";
    std::cout << "Mode      : " << mloMode << "\n";
    std::cout << "Num links : " << numLinks << "\n";

    RngSeedManager::SetSeed(1);

    NodeContainer apNodes;
    apNodes.Create(nAp);
    NodeContainer staNodes;
    staNodes.Create(nSta);

    std::vector<Ptr<MultiModelSpectrumChannel>> spectrumChannels(numLinks);

    for (uint32_t i = 0; i < numLinks; ++i)
    {
        spectrumChannels[i] = CreateObject<MultiModelSpectrumChannel>();

        // Paper-faithful: residential LogDistance (gamma=3.5, refLoss=40 dB @ 1 m)
        Ptr<LogDistancePropagationLossModel> loss = CreateObject<LogDistancePropagationLossModel>();
        loss->SetAttribute("Exponent",          DoubleValue(3.5));
        loss->SetAttribute("ReferenceLoss",     DoubleValue(40.0));
        loss->SetAttribute("ReferenceDistance", DoubleValue(1.0));
        spectrumChannels[i]->AddPropagationLossModel(loss);
        spectrumChannels[i]->SetPropagationDelayModel(
            CreateObject<ConstantSpeedPropagationDelayModel>());
    }

    SpectrumWifiPhyHelper phy(numLinks);
    phy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
    phy.SetPcapCaptureType(WifiPhyHelper::PcapCaptureType::PCAP_PER_LINK);

    for (uint32_t i = 0; i < numLinks; ++i)
    {
        phy.AddChannel(spectrumChannels[i], WIFI_SPECTRUM_5_GHZ);
    }

    // Kanały 80 MHz (5 GHz) – dobrane dalej od siebie (STR ma sens)
    uint32_t chanNums[3] = {42, 106, 155};

    for (uint32_t i = 0; i < numLinks; ++i)
    {   
        std::ostringstream oss;
        oss << "{" << chanNums[i] << ", 80, BAND_5GHZ, 0}";
        phy.Set(i, "ChannelSettings", StringValue(oss.str()));
        phy.Set(i, "Antennas", UintegerValue(2));
        phy.Set(i, "MaxSupportedTxSpatialStreams", UintegerValue(2));
        phy.Set(i, "MaxSupportedRxSpatialStreams", UintegerValue(2));
    }

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211be);
    wifi.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));

    for (uint32_t i = 0; i < numLinks; ++i)
    {
        wifi.SetRemoteStationManager(i,
                                     "ns3::ConstantRateWifiManager",
                                     "DataMode", StringValue("EhtMcs8"),
                                     "ControlMode", StringValue("OfdmRate24Mbps"));
    }

    if (mloMode == "EMLSR" && numLinks > 1)
    {
        wifi.ConfigEhtOptions("EmlsrActivated", BooleanValue(true));
        wifi.ConfigEhtOptions("TransitionTimeout", TimeValue(MicroSeconds(1024)));
    }

    WifiMacHelper wifiMac;
    // Keep SU behavior neutral for 1 STA and avoid MU scheduler side-effects.

    Ssid ssid = Ssid("wifi-mlo-thr1");

    wifiMac.SetType("ns3::ApWifiMac",
                    "Ssid", SsidValue(ssid),
                    "BE_MaxAmpduSize", UintegerValue(1048575),
                    "MpduBufferSize", UintegerValue(1024));
    NetDeviceContainer apDevices = wifi.Install(phy, wifiMac, apNodes);

    if (mloMode == "EMLSR" && numLinks > 1)
    {
        std::string linkSet;
        if (numLinks == 2)
        {
            linkSet = "0,1";
        }
        else
        {
            linkSet = "0,1,2";
        }

        wifiMac.SetEmlsrManager("ns3::DefaultEmlsrManager",
                                "EmlsrLinkSet", StringValue(linkSet),
                    "SwitchAuxPhy", BooleanValue(true),
                    "PutAuxPhyToSleep", BooleanValue(false),
                    "AuxPhyTxCapable", BooleanValue(true));
    }

    wifiMac.SetType("ns3::StaWifiMac",
                    "Ssid", SsidValue(ssid),
                    "ActiveProbing", BooleanValue(false),
                    "BE_MaxAmpduSize", UintegerValue(1048575),
                    "MpduBufferSize", UintegerValue(1024));
    NetDeviceContainer staDevices = wifi.Install(phy, wifiMac, staNodes);

    MobilityHelper mobility;
    Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator>();
    pos->Add(Vector(0.0, 0.0, 0.0));
    pos->Add(Vector(7.0, 0.0, 0.0));  // 7 m link distance per Carrascosa WCNC 2023
    mobility.SetPositionAllocator(pos);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(apNodes);
    mobility.Install(staNodes);

    InternetStackHelper stack;
    stack.Install(apNodes);
    stack.Install(staNodes);

    Ipv4AddressHelper address;
    address.SetBase("10.1.0.0", "255.255.255.0");
    Ipv4InterfaceContainer apIf = address.Assign(apDevices);
    Ipv4InterfaceContainer staIf = address.Assign(staDevices);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    uint16_t basePort = 5000;

    // One saturated flow keeps link usage emergent from MLO/EMLSR itself.
    uint32_t nFlows = 1;
    if ((mloMode == "STR" || mloMode == "EMLSR") && numLinks > 1)
    {
        nFlows = numLinks;
    }

    ApplicationContainer serverApps;
    ApplicationContainer clientApps;

    for (uint32_t i = 0; i < nFlows; ++i)
    {
        uint16_t port = basePort + static_cast<uint16_t>(i);

        UdpServerHelper server(port);
        ApplicationContainer sApp = server.Install(staNodes.Get(0));
        sApp.Start(Seconds(5.0));
        sApp.Stop(Seconds(simTime + 5.0));
        serverApps.Add(sApp);

        UdpClientHelper client(staIf.GetAddress(0), port);
        client.SetAttribute("MaxPackets", UintegerValue(0xFFFFFFFF)); // unlimited
        client.SetAttribute("Interval", TimeValue(MicroSeconds(1)));  // ~12 Gb/s offered load
        client.SetAttribute("PacketSize", UintegerValue(payloadSize));

        ApplicationContainer cApp = client.Install(apNodes.Get(0));
        cApp.Start(Seconds(5.0));  // delay to let EMLSR establish before traffic starts
        cApp.Stop(Seconds(simTime + 5.0));
        clientApps.Add(cApp);
    }

    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor> flowmon = fmHelper.InstallAll();

    // MacRx on STA only (node index 1 = first STA)
    uint32_t staNodeId = staNodes.Get(0)->GetId();
    Config::Connect("/NodeList/" + std::to_string(staNodeId) +
                    "/DeviceList/*/$ns3::WifiNetDevice/Mac/MacRx",
                    MakeCallback(&MacRxTrace));

    Simulator::Stop(Seconds(simTime + 5.0));
    Simulator::Run();

    // --- MacRx: total bits received by STA / simTime ---
    double thrMacRx = (simTime > 0.0) ? (g_macRxBytes * 8.0) / (simTime * 1e6) : 0.0;
    std::cout << "MacRx throughput (STA received bits / simTime): " << thrMacRx << " Mb/s" << std::endl;

    // --- L3 ---
    double thrL3 = GetFlowThroughputMbps(flowmon);
    std::cout << "Average L3 throughput (FlowMonitor): " << thrL3 << " Mb/s" << std::endl;

    Simulator::Destroy();
    return 0;
}
