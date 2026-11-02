function latency = fig25_vr(simTime, randnum, radius, channelBW, bandAndChannel, mcs, numSTA, isDownlink, isMLO, dlOnTime)
    if nargin < 10, dlOnTime = 50e-6; end
   

    % Set seed of random number generator for reproducible results
    rng(randnum,"combRecursive");
    % Set simulation time
    simulationTime = simTime;
   
    staNodes = wlanNode.empty();
    staIndex=1;
    phyAbstraction="tgax-evaluation-methodology";
    macAbstraction = true;
    aggregationLimit = 1024;
    % Create wireless network simulator
    networkSimulator = wirelessNetworkSimulator.init;
    % Band and channel values for each link
    %bandAndChannel = [5 1;5 136];
    numLinks = size(bandAndChannel,1);
    
    [staPositions, apPosition] = randomPositionsFermat(numSTA, radius);

    
    if isMLO
        % Create the link config objects for AP MLD, STR STA MLD
        for linkIdx = 1:numLinks
            apLinkCfg(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=aggregationLimit, TransmitPower=23);
            staSTRLinkCfg(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=aggregationLimit, TransmitPower=23);
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

    else
        apSLOCfg = wlanDeviceConfig(Mode="AP", MCS=mcs, BandAndChannel=bandAndChannel(1,:),ChannelBandwidth=channelBW, TransmissionFormat="EHT-SU",MPDUAggregationLimit=aggregationLimit, ...
            NumSpaceTimeStreams=2, NumTransmitAntennas=2, TransmitPower=23);
        staSLOCfg = wlanDeviceConfig(Mode="STA", MCS=mcs, BandAndChannel=bandAndChannel(1,:),ChannelBandwidth=channelBW, TransmissionFormat="EHT-SU",MPDUAggregationLimit=aggregationLimit, ...
            NumSpaceTimeStreams=2, NumTransmitAntennas=2, TransmitPower=23);
    
        apNode = wlanNode(Position=apPosition,Name="AP",DeviceConfig=apSLOCfg, PHYAbstractionMethod=phyAbstraction,MACFrameAbstraction=macAbstraction);
        if numSTA>0   
            for i = 1:numSTA
                staNodes(staIndex) = wlanNode( ...
                    Position=staPositions(staIndex, :), ...
                    Name="SLO STA " + i, ...
                    DeviceConfig=staSLOCfg, ...
                    PHYAbstractionMethod=phyAbstraction, ...
                    MACFrameAbstraction=macAbstraction);
                staIndex = staIndex + 1;
            end
        end

    end

  
    nodes = [apNode staNodes];
    
    associateStations(apNode,staNodes);
    dlTraffic = networkTrafficOnOff(DataRate=1100/dlOnTime,PacketSize=1500,OnTime=dlOnTime,OffTime=11.11e-3-dlOnTime);
    addTrafficSource(apNode,dlTraffic,DestinationNode=staNodes)
    ulTraffic = networkTrafficOnOff(DataRate=4.256e6,PacketSize=133,OnTime=1e-6,OffTime=11.109e-3);
    addTrafficSource(staNodes,ulTraffic,DestinationNode=apNode)

    % Add channel model to the simulator
    channel = hSLSTGaxMultiFrequencySystemChannel(nodes, PathLossModel = 'residential');
    addChannelModel(networkSimulator,channel.ChannelFcn);

    % Add nodes to network simulator
    addNodes(networkSimulator,nodes);

    % Create node performance visualization object
    perfViewerObj = hPerformanceViewer(nodes,simulationTime);

    % Run the simulation
    run(networkSimulator,simulationTime);
   
    latency75list = getpPacketLatencyVector(perfViewerObj, 75);
    latency90list = getpPacketLatencyVector(perfViewerObj, 90);
    latency = [latency75list(2,:) latency90list(1,:)];

    
