function [thr, latency50, latency99] = fig12_load_without_contention(randnum, simTime, radius, isMLO, channelBW, bandAndChannel, mcs, maxThr, expectedLoad)
   

    % Set seed of random number generator for reproducible results
    rng(randnum,"combRecursive");
    % Set simulation time
    simulationTime = simTime;

    phyAbstraction="tgax-evaluation-methodology";
    macAbstraction = true;
    
    % Create wireless network simulator
    networkSimulator = wirelessNetworkSimulator.init;
    % Band and channel values for each link
    %bandAndChannel = [5 1;5 136];
    numLinks = size(bandAndChannel,1);
    aggrLimit = 1024;

    
    [staPosition, apPosition] = randomPositionsFermat(1, radius);

    % Create the link config objects for AP MLD, STR STA MLD
    for linkIdx = 1:numLinks
        apLinkCfg(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, MPDUAggregationLimit=aggrLimit,NumSpaceTimeStreams=2,NumTransmitAntennas=2);
        staSTRLinkCfg(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, MPDUAggregationLimit=aggrLimit, NumSpaceTimeStreams=2, NumTransmitAntennas=2);
    end

    % Create MLD config objects for AP MLD, STR STA MLD
    apMLDCfg = wlanMultilinkDeviceConfig(Mode="AP",LinkConfig=apLinkCfg);
    staSTRMLDCfg = wlanMultilinkDeviceConfig(Mode="STA",LinkConfig=staSTRLinkCfg);
    
    
    
    % Create the AP MLD node, STR STA MLD node
    apNode = wlanNode(Position=apPosition,Name="AP",DeviceConfig=apMLDCfg, PHYAbstractionMethod=phyAbstraction,MACFrameAbstraction=macAbstraction);
    
    if isMLO
        staNode = wlanNode( ...
                Position=staPosition, ...
                Name="STR STA", ...
                DeviceConfig=staSTRMLDCfg, ...
                PHYAbstractionMethod=phyAbstraction, ...
                MACFrameAbstraction=macAbstraction);

    else
        staCfg = wlanDeviceConfig(Mode="STA", BandAndChannel=bandAndChannel(1,:), ChannelBandwidth=channelBW, MCS=mcs,MPDUAggregationLimit=aggrLimit, TransmissionFormat="EHT-SU", ...
            NumSpaceTimeStreams=2, NumTransmitAntennas=2);
        staNode = wlanNode( ...
                Position=staPosition, ...
                Name="SL STA", ...
                DeviceConfig=staCfg, ...
                PHYAbstractionMethod=phyAbstraction, ...
                MACFrameAbstraction=macAbstraction);
    end           
      
    nodes = [apNode staNode];
    
    % Associate the STAs to AP MLD
    associateStations(apNode,staNode);

    %traffic
    [pktSize, dataRate, onTime, offTime] = generateTrafficParams(maxThr, 1, expectedLoad);
   
    trafficSource = networkTrafficOnOff(DataRate=dataRate,PacketSize=pktSize,OnTime=onTime,OffTime=offTime);       
    addTrafficSource(apNode,trafficSource,DestinationNode=staNode)
    

    % Add channel model to the simulator
    channel = hSLSTGaxMultiFrequencySystemChannel(nodes);
    addChannelModel(networkSimulator,channel.ChannelFcn);

    % Add nodes to network simulator
    addNodes(networkSimulator,nodes);

    % Create node performance visualization object
    perfViewerObj = hPerformanceViewer(nodes,simulationTime);

    % Run the simulation
    run(networkSimulator,simulationTime);
    
    % Calculate throughput at AP
    apThroughput = throughput(perfViewerObj,apNode.ID);
    latency50list = getpPacketLatencyVector(perfViewerObj, 50);
    latency99list = getpPacketLatencyVector(perfViewerObj, 99);

    latency50 = latency50list(2,:);
    latency99 = latency99list(2,:);
    thr = apThroughput;