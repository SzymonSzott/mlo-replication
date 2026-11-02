function thr = table05_baseline_throughput(simTime, randnum, numSTA, radius, channelBW, bandAndChannel, mcs, aggregationLimit)
   

    % Set seed of random number generator for reproducible results
    rng(randnum,"combRecursive");
    % Set simulation time
    simulationTime = simTime;


   
    staNodes = wlanNode.empty();
    staIndex=1;
    phyAbstraction="tgax-evaluation-methodology";
    macAbstraction = true;
    
    % Create wireless network simulator
    networkSimulator = wirelessNetworkSimulator.init;
    % Band and channel values for each link
    %bandAndChannel = [5 1;5 136];
    numLinks = size(bandAndChannel,1);
    
    [staPositions, apPosition] = randomPositionsFermat(numSTA, radius);

    % Create the link config objects for AP MLD, STR STA MLD
    for linkIdx = 1:numLinks
        apLinkCfg(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=aggregationLimit);
        staSTRLinkCfg(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=aggregationLimit);
    end

    % Create MLD config objects for AP MLD, STR STA MLD
    apMLDCfg = wlanMultilinkDeviceConfig(Mode="AP",LinkConfig=apLinkCfg);
    staSTRMLDCfg = wlanMultilinkDeviceConfig(Mode="STA",LinkConfig=staSTRLinkCfg);
   
    
    
    % Create the AP MLD node, STR STA MLD node
    apNode = wlanNode(Position=apPosition,Name="AP",DeviceConfig=apMLDCfg, PHYAbstractionMethod=phyAbstraction,MACFrameAbstraction=macAbstraction);
    if numSTA>0   
        for i = 1:numSTA
            staNodes(staIndex) = wlanNode( ...
                Position=staPositions(staIndex, :), ...
                Name="STR STA " + i, ...
                DeviceConfig=staSTRMLDCfg, ...
                PHYAbstractionMethod=phyAbstraction, ...
                MACFrameAbstraction=macAbstraction);
            staIndex = staIndex + 1;
        end
    end

  
    nodes = [apNode staNodes];
    
    % Associate the STAs to AP MLD and configure uplink full buffer traffic
    associateStations(apNode,staNodes,FullBufferTraffic="DL");

    % Add channel model to the simulator
    channel = hSLSTGaxMultiFrequencySystemChannel(nodes, PathLossModel = 'residential');
    addChannelModel(networkSimulator,channel.ChannelFcn);

    % Add nodes to network simulator
    addNodes(networkSimulator,nodes);

    % Create node performance visualization object
    perfViewerObj = hPerformanceViewer(nodes,simulationTime);

    % Run the simulation
    run(networkSimulator,simulationTime);
   
    % Calculate throughput at STAs
    apThroughput = throughput(perfViewerObj,apNode.ID);
    %getpPacketLatencyVector(perfViewerObj, 95)
    thr = apThroughput;

    
