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
  for (auto const &p : stats)
    {
      const FlowMonitor::FlowStats &st = p.second;
      if (st.rxPackets == 0)
        {
          continue;
        }

      double tFirst = st.timeFirstRxPacket.GetSeconds();
      double tLast  = st.timeLastRxPacket.GetSeconds();
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
//  MAC-level: MacTx (wszystkie pakiety wychodzące z MAC)
// ============================================================================

static uint64_t g_macTxBytes = 0;
static Time     g_macFirstTx = Seconds(0.0);
static Time     g_macLastTx  = Seconds(0.0);
static bool     g_macStarted = false;

// OKNO CZASOWE dla MAC (żeby liczyć jak L3 / czas pracy aplikacji)
static Time     g_macWinStart = Seconds(0.0);
static Time     g_macWinStop  = Seconds(0.0);

static void
MacTxTrace(std::string context, Ptr<const Packet> packet)
{
  Time now = Simulator::Now();
  if (now < g_macWinStart || now > g_macWinStop)
    {
      return;
    }

  if (!g_macStarted)
    {
      g_macStarted = true;
      g_macFirstTx = now;
    }
  g_macLastTx = now;
  g_macTxBytes += packet->GetSize();
}

// ============================================================================
//  PHY-level: PhyTxPsduBegin (PSDU + airtime)
// ============================================================================

static uint64_t g_phyTxBits    = 0;
static Time     g_phyAirtime   = Seconds(0.0);
static Time     g_phyFirstTx   = Seconds(0.0);
static Time     g_phyLastTx    = Seconds(0.0);
static bool     g_phyStarted   = false;

static void
PhyTxPsduBeginTrace(std::string context,
                    WifiConstPsduMap psduMap,
                    WifiTxVector txVector,
                    double txPowerW)
{
  if (!g_phyStarted)
    {
      g_phyStarted = true;
      g_phyFirstTx = Simulator::Now();
    }
  g_phyLastTx = Simulator::Now();

  for (auto const &it : psduMap)
    {
      Ptr<const WifiPsdu> psdu = it.second;
      g_phyTxBits += static_cast<uint64_t>(psdu->GetSize()) * 8u;
    }

  Time dur = WifiPhy::CalculateTxDuration(psduMap, txVector, WIFI_PHY_BAND_5GHZ);
  g_phyAirtime += dur;
}

// ============================================================================

int
main(int argc, char *argv[])
{
  uint32_t nAp   = 1;
  uint32_t nSta  = 1;
  double   simTime = 10.0;       // [s] per thesis Table 3.3
  uint32_t payloadSize = 1500;   // [B]
  uint32_t nMpdus = 1024;        // tylko informacyjnie
  uint32_t numLinks = 1;         // 1, 2 lub 3
  std::string mloMode = "SLO";   // SLO, STR, EMLSR

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

  std::cout << "Throughput Scenario #1\n";
  std::cout << "Mode      : " << mloMode << "\n";
  std::cout << "Num links : " << numLinks << "\n";

  RngSeedManager::SetSeed(1);

  // Węzły
  NodeContainer apNodes;
  apNodes.Create(nAp);
  NodeContainer staNodes;
  staNodes.Create(nSta);

  // Kanały 5 GHz dla linków
  std::vector< Ptr<MultiModelSpectrumChannel> > spectrumChannels(numLinks);

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

  // PHY: numLinks linków (SpectrumWifiPhy)
  SpectrumWifiPhyHelper phy(numLinks);
  phy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
  phy.SetPcapCaptureType(WifiPhyHelper::PcapCaptureType::PCAP_PER_LINK);
  phy.SetErrorRateModel("ns3::NistErrorRateModel");

  for (uint32_t i = 0; i < numLinks; ++i)
    {
      phy.AddChannel(spectrumChannels[i], WIFI_SPECTRUM_5_GHZ);
    }

  uint32_t chanNums[3] = {42, 58, 106};

  for (uint32_t i = 0; i < numLinks; ++i)
    {
      std::ostringstream oss;
      oss << "{" << chanNums[i] << ", 80, BAND_5GHZ, 0}";
      phy.Set(i, "ChannelSettings", StringValue(oss.str()));
      phy.Set(i, "Antennas", UintegerValue(2));
      phy.Set(i, "MaxSupportedTxSpatialStreams", UintegerValue(2));
      phy.Set(i, "MaxSupportedRxSpatialStreams", UintegerValue(2));
    }

  phy.Set("ChannelSwitchDelay", TimeValue(MicroSeconds(100)));

  WifiHelper wifi;
  wifi.SetStandard(WIFI_STANDARD_80211be);

  wifi.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));

  for (uint32_t i = 0; i < numLinks; ++i)
    {
      wifi.SetRemoteStationManager(i,
                                   "ns3::ConstantRateWifiManager",
                                   "DataMode", StringValue("EhtMcs8"),
                                   "ControlMode", StringValue("EhtMcs0"));
    }

  if (mloMode == "EMLSR" && numLinks > 1)
    {
      wifi.ConfigEhtOptions("EmlsrActivated", BooleanValue(true));
      wifi.ConfigEhtOptions("TransitionTimeout", TimeValue(MicroSeconds(1024)));
      wifi.ConfigEhtOptions("MediumSyncDuration", TimeValue(MicroSeconds(3200)));
      wifi.ConfigEhtOptions("MsdOfdmEdThreshold", IntegerValue(-72));
      wifi.ConfigEhtOptions("MsdMaxNTxops", UintegerValue(0));
    }

  WifiMacHelper wifiMac;
  wifiMac.SetMultiUserScheduler("ns3::RrMultiUserScheduler",
                                "EnableUlOfdma", BooleanValue(false),
                                "EnableBsrp", BooleanValue(false));

  Ssid ssid = Ssid("wifi-mlo-thr1");

  wifiMac.SetType("ns3::ApWifiMac",
                  "Ssid", SsidValue(ssid),
                  "QosSupported", BooleanValue(true));
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
                              "EmlsrPaddingDelay", TimeValue(MicroSeconds(32)),
                              "EmlsrTransitionDelay", TimeValue(MicroSeconds(128)),
                              "SwitchAuxPhy", BooleanValue(true),
                              "AuxPhyChannelWidth", UintegerValue(80),
                              "AuxPhyTxCapable", BooleanValue(true),
                              "PutAuxPhyToSleep", BooleanValue(false));
    }

  wifiMac.SetType("ns3::StaWifiMac",
                  "Ssid", SsidValue(ssid),
                  "QosSupported", BooleanValue(true),
                  "ActiveProbing", BooleanValue(false));
  NetDeviceContainer staDevices = wifi.Install(phy, wifiMac, staNodes);

  MobilityHelper mobility;
  Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator>();
  pos->Add(Vector(0.0, 0.0, 0.0));
  pos->Add(Vector(5.0, 0.0, 0.0));  // 5 m link distance per thesis Table 3.3
  mobility.SetPositionAllocator(pos);
  mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
  mobility.Install(apNodes);
  mobility.Install(staNodes);

  InternetStackHelper stack;
  stack.Install(apNodes);
  stack.Install(staNodes);

  Ipv4AddressHelper address;
  address.SetBase("10.1.0.0", "255.255.255.0");
  Ipv4InterfaceContainer apIf  = address.Assign(apDevices);
  Ipv4InterfaceContainer staIf = address.Assign(staDevices);

  Ipv4GlobalRoutingHelper::PopulateRoutingTables();

  uint16_t port = 5000;

  UdpServerHelper server(port);
  ApplicationContainer serverApp = server.Install(staNodes.Get(0));
  serverApp.Start(Seconds(0.0));
  serverApp.Stop(Seconds(simTime + 1.0));

  UdpClientHelper client(staIf.GetAddress(0), port);
  client.SetAttribute("MaxPackets", UintegerValue(0xFFFFFFFF));
  client.SetAttribute("Interval", TimeValue(Seconds(1e-6)));
  client.SetAttribute("PacketSize", UintegerValue(payloadSize));

  ApplicationContainer clientApp = client.Install(apNodes.Get(0));
  clientApp.Start(Seconds(0.1));
  clientApp.Stop(Seconds(simTime + 1.0));

  // OKNO CZASOWE MAC = czas działania klienta UDP
  g_macWinStart = Seconds(0.1);
  g_macWinStop  = Seconds(simTime + 1.0);

  FlowMonitorHelper fmHelper;
  Ptr<FlowMonitor> flowmon = fmHelper.InstallAll();

  Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Mac/MacTx",
                  MakeCallback(&MacTxTrace));

  Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/PhyTxPsduBegin",
                  MakeCallback(&PhyTxPsduBeginTrace));

  Simulator::Stop(Seconds(simTime + 1.0));
  Simulator::Run();

  double thrL3 = GetFlowThroughputMbps(flowmon);
  std::cout << "Average L3 throughput (FlowMonitor): "
            << thrL3 << " Mb/s" << std::endl;

  // --- MAC ---
  double durationMac = (g_macWinStop - g_macWinStart).GetSeconds();
  if (durationMac <= 0.0)
    {
      durationMac = simTime;
    }
  double thrMac = (g_macTxBytes * 8.0) / (durationMac * 1e6); // Mb/s
  std::cout << "MAC-level throughput (MacTx, all Wi-Fi devices): "
            << thrMac << " Mb/s" << std::endl;

  // --- PHY ---
  double airtime = g_phyAirtime.GetSeconds();
  if (!g_phyStarted || airtime <= 0.0)
    {
      airtime = 0.0;
    }

  double phyThrOverSim = (static_cast<double>(g_phyTxBits)) / (simTime * 1e6);

  double phyDataRate = 0.0;
  if (airtime > 0.0)
    {
      phyDataRate = (static_cast<double>(g_phyTxBits)) / (airtime * 1e6);
    }

  double airtimeUtil = 0.0;
  if (simTime > 0.0)
    {
      airtimeUtil = airtime / simTime;
    }

  std::cout << "PHY-level effective throughput over sim time: "
            << phyThrOverSim << " Mb/s" << std::endl;
  std::cout << "PHY-level data rate (bits/airtime): "
            << phyDataRate << " Mb/s" << std::endl;
  std::cout << "PHY airtime utilization: "
            << airtimeUtil * 100.0 << " % of simulation time" << std::endl;

  Simulator::Destroy();
  return 0;
}
