function latency = fig10_11_user_density(randnum, simTime, numSTA, radius, channelBW, bandAndChannel, mcs, maxThr, expectedLoad)
   

    %seed of random number generator for reproducible results
    rng(randnum,"combRecursive");
    % Set simulation time
    simulationTime = simTime;

    
    staNodes = wlanNode.empty();
    phyAbstraction="tgax-evaluation-methodology";
    macAbstraction = true;
    
    % Create wireless network simulator
    networkSimulator = wirelessNetworkSimulator.init;
    % Band and channel values for each link
    %bandAndChannel = [5 1;5 136];
    numLinks = size(bandAndChannel,1);
    
    [staPositions, apPosition] = randomPositionsUniform(numSTA, radius);

    % Create the link config objects for AP MLD, STR STA MLD
    for linkIdx = 1:numLinks
        apLinkCfg(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, ...
            NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=1024, TransmitPower=23);
        staSTRLinkCfg(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, ...
            NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=1024, TransmitPower=23);
    end

    % Create MLD config objects for AP MLD, STR STA MLD
    apMLDCfg = wlanMultilinkDeviceConfig(Mode="AP",LinkConfig=apLinkCfg);
    staSTRMLDCfg = wlanMultilinkDeviceConfig(Mode="STA",LinkConfig=staSTRLinkCfg);
   
    
    
    % Create the AP MLD node, STR STA MLD node
    apNode = wlanNode(Position=apPosition,Name="AP",DeviceConfig=apMLDCfg, PHYAbstractionMethod=phyAbstraction,MACFrameAbstraction=macAbstraction);
    if numSTA>0   
        for i = 1:numSTA
            staNodes(i) = wlanNode( ...
                Position=staPositions(i, :), ...
                Name="STR STA " + i, ...
                DeviceConfig=staSTRMLDCfg, ...
                PHYAbstractionMethod=phyAbstraction, ...
                MACFrameAbstraction=macAbstraction);
        end
    end

  
    nodes = [apNode staNodes];
    
    % Associate the STAs to AP MLD and configure uplink full buffer traffic
    associateStations(apNode,staNodes);

    %traffic
    [pktSize, dataRate, onTime, offTime] = generateTrafficParams(maxThr, numSTA, expectedLoad);
    for staIdx=1:numSTA
        trafficSource(staIdx) = networkTrafficOnOff(DataRate=dataRate,PacketSize=pktSize,OnTime=onTime,OffTime=offTime);       
        addTrafficSource(apNode,trafficSource(staIdx),DestinationNode=staNodes(staIdx))
    end

    % Add channel model to the simulator
    channel = hSLSTGaxMultiFrequencySystemChannel(nodes, PathLossModel = 'residential');
    addChannelModel(networkSimulator,channel.ChannelFcn);

    % Add nodes to network simulator
    addNodes(networkSimulator,nodes);

    % Create node performance visualization object
    perfViewerObj = hPerformanceViewer(nodes,simulationTime);

    % Run the simulation
    run(networkSimulator,simulationTime);
   
    staLatency = averageReceiveLatency(perfViewerObj,[staNodes(:).ID]);
    latency = mean(staLatency, "omitnan");
end
