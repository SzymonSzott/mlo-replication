function [thr, latencyMainSTA, avgLatency ] = fig19_20_channel_occupancy(randnum, simTime, radius, radiusList, channelBW, isMLO, mcs, maxThr, expectedLoad, expectedOccupancies)
   

    
    % Set seed of random number generator for reproducible results
    rng(randnum,"combRecursive");
    % Set simulation time
    simulationTime = simTime;

    %mcs = 11;
    apNodes = wlanNode.empty();
    staIdx=1;
    phyAbstraction="tgax-evaluation-methodology";
    macAbstraction = true;
    
    % Create wireless network simulator
    networkSimulator = wirelessNetworkSimulator.init;
    %determine number of links
    bandAndChannel = [5 36; 5 100];
    numLinks = size(bandAndChannel,1);
    aggrLimit = 1024;
    %main station and access point positions
    [staPosition, apPosition] = randomPositionsFermat(1, radius);
    
    if isMLO
        % Create the link config objects for AP MLD, STR STA MLD
        for linkIdx = 1:numLinks
            apLinkCfg(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=aggrLimit);
            staSTRLinkCfg(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=aggrLimit);
        end
    
        % Create MLD config objects for AP MLD, STR STA MLD
        apMLDCfg = wlanMultilinkDeviceConfig(Mode="AP",LinkConfig=apLinkCfg);
        staSTRMLDCfg = wlanMultilinkDeviceConfig(Mode="STA",LinkConfig=staSTRLinkCfg);
    
        apNode = wlanNode(Position=apPosition,Name="AP",DeviceConfig=apMLDCfg, PHYAbstractionMethod=phyAbstraction,MACFrameAbstraction=macAbstraction);
        staNode = wlanNode( ...
                    Position=staPosition(1, :), ...
                    Name="STR STA", ...
                    DeviceConfig=staSTRMLDCfg, ...
                    PHYAbstractionMethod=phyAbstraction, ...
                    MACFrameAbstraction=macAbstraction); 

    else
        apSLOCfg = wlanDeviceConfig(Mode="AP", MCS=mcs, BandAndChannel=bandAndChannel(1,:),ChannelBandwidth=channelBW, TransmissionFormat="EHT-SU", MPDUAggregationLimit=aggrLimit, ...
            NumSpaceTimeStreams=2, NumTransmitAntennas=2);
        staSLOCfg = wlanDeviceConfig(Mode="STA", MCS=mcs, BandAndChannel=bandAndChannel(1,:),ChannelBandwidth=channelBW, TransmissionFormat="EHT-SU", MPDUAggregationLimit=aggrLimit, ...
            NumSpaceTimeStreams=2, NumTransmitAntennas=2);
    
        apNode = wlanNode(Position=apPosition,Name="AP",DeviceConfig=apSLOCfg, PHYAbstractionMethod=phyAbstraction,MACFrameAbstraction=macAbstraction);
        staNode = wlanNode( ...
                    Position=staPosition(1, :), ...
                    Name="MAIN STA", ...
                    DeviceConfig=staSLOCfg, ...
                    PHYAbstractionMethod=phyAbstraction, ...
                    MACFrameAbstraction=macAbstraction);

    end


    % Associate the STAs to AP MLD 
    associateStations(apNode,staNode);

    %traffic from main bss
    [pktSize1, dataRate1, onTime1, offTime1] = generateTrafficParams(maxThr, 1, expectedLoad);
    
    trafficSource = networkTrafficOnOff(DataRate=dataRate1,PacketSize=pktSize1,OnTime=onTime1,OffTime=offTime1);       
    addTrafficSource(staNode,trafficSource,DestinationNode=apNode);
    
    staNodesList = cell(1, numLinks);
    for linkIdx = 1:numLinks
        %AP OBSS 
        numSTA = expectedOccupancies(linkIdx) * 10;
        [staOBSSpositions, apOBSSposition] = randomPositionsFermat(numSTA, radiusList(linkIdx));
        obssAPCfg = wlanDeviceConfig(Mode="AP", MCS=mcs, BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW, TransmissionFormat="EHT-SU", MPDUAggregationLimit=aggrLimit, ...
            NumSpaceTimeStreams=2, NumTransmitAntennas=2);
        apNodes(linkIdx) = wlanNode(Position=apOBSSposition, Name="OBSS AP " + linkIdx, DeviceConfig=obssAPCfg, PHYAbstractionMethod=phyAbstraction, MACFrameAbstraction=macAbstraction);
        
        %sta Config
        staCfg(linkIdx) = wlanDeviceConfig(Mode="STA", MCS=mcs, BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW, TransmissionFormat="EHT-SU", MPDUAggregationLimit=aggrLimit, ...
            NumSpaceTimeStreams=2, NumTransmitAntennas=2);
        
        [pktSize, dataRate, onTime, offTime] = generateTrafficParamsOBSS(maxThr);

        staNodesList{linkIdx} = wlanNode.empty;
        for staIdx = 1:numSTA
            staNodesList{linkIdx}(staIdx) = wlanNode(Position=staOBSSpositions(staIdx,:), Name="OBSS STA " + linkIdx + "." + staIdx, DeviceConfig=staCfg(linkIdx), PHYAbstractionMethod=phyAbstraction, MACFrameAbstraction=macAbstraction);
            
        end
        
        associateStations(apNodes(linkIdx), staNodesList{linkIdx})
            
        for staIdx = 1:numSTA
            traffic(staIdx) = networkTrafficOnOff(DataRate=dataRate, PacketSize=pktSize, OnTime=onTime, OffTime=offTime);
            addTrafficSource(staNodesList{linkIdx}(staIdx), traffic(staIdx), DestinationNode=apNodes(linkIdx));
            
        end

        
    end

    % flatten
    flattenedStaNodes = [staNodesList{:}];
    staNodes = [ staNode flattenedStaNodes ];
    nodes = [staNodes apNode apNodes ];

    
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
    staThroughput = throughput(perfViewerObj,staNode.ID);
    latency = getpPacketLatencyVector(perfViewerObj, 95);
    
    avgLatency = averageReceiveLatency(perfViewerObj, apNode.ID);
    latencyMainSTA = latency(numel(staNodes)+1,:);
    thr = staThroughput;

    