function [thr, avgLatency] = fig08_09_transmission_duration(randnum, simTime, radius, radiusList, bandAndChannel, aggrLimit, channelBW, mcs, isFullBuffer, maxThr, expectedLoad, expectedOccupancies, txop)
   
% inspired by "Aggregation Algorithm to Increase Throughput of Multi-Link
% Wi-Fi 7 Devices"


    % Set seed of random number generator for reproducible results
    rng(randnum,"combRecursive");
    % Set simulation time
    simulationTime = simTime;

   
    staNodes = wlanNode.empty();
    phyAbstraction="tgax-evaluation-methodology";
    macAbstraction = true;
    
    % Create wireless network simulator
    networkSimulator = wirelessNetworkSimulator.init;
    % Band and channel values for each link
    numLinks = size(bandAndChannel,1);
    
    [staPosition, apPosition] = randomPositionsFermat(1, radius);

    % Create the link config objects for AP MLD, STR STA MLD
    for linkIdx = 1:numLinks
        apLinkCfg(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel(linkIdx,:),MPDUAggregationLimit=aggrLimit,ChannelBandwidth=channelBW,MCS=mcs,TXOPLimit=txop, ...
            NumSpaceTimeStreams=2, NumTransmitAntennas=2, TransmitPower=23);
        staSTRLinkCfg(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel(linkIdx,:),MPDUAggregationLimit=aggrLimit,ChannelBandwidth=channelBW,MCS=mcs,TXOPLimit=txop, ...
            NumSpaceTimeStreams=2, NumTransmitAntennas=2, TransmitPower=23);
    end

    % Create MLD config objects for AP MLD, STR STA MLD
    apMLDCfg = wlanMultilinkDeviceConfig(Mode="AP",LinkConfig=apLinkCfg);
    staSTRMLDCfg = wlanMultilinkDeviceConfig(Mode="STA",LinkConfig=staSTRLinkCfg);
   
    
    
    % Create the AP MLD node, STR STA MLD node
    apNode = wlanNode(Position=apPosition,Name="AP",DeviceConfig=apMLDCfg, PHYAbstractionMethod=phyAbstraction,MACFrameAbstraction=macAbstraction);
      
    
    staNode= wlanNode( ...
        Position=staPosition, ...
        Name="STR STA ", ...
        DeviceConfig=staSTRMLDCfg, ...
        PHYAbstractionMethod=phyAbstraction, ...
        MACFrameAbstraction=macAbstraction);
           
   
    if isFullBuffer
        % Associate the STAs to AP MLD and configure downlink full buffer traffic
        associateStations(apNode,staNode,FullBufferTraffic="DL");
    else
        associateStations(apNode,staNode);
        [pktSize, dataRate, onTime, offTime] = generateTrafficParams(maxThr, 1, expectedLoad);
        
        trafficSource = networkTrafficOnOff(DataRate=dataRate,PacketSize=pktSize,OnTime=onTime,OffTime=offTime);       
        addTrafficSource(apNode,trafficSource,DestinationNode=staNode);
        
    end
    

    staNodesList = cell(1, numLinks);
    apNodes = wlanNode.empty();
    for linkIdx = 1:numLinks
        % Create AP OBSS 
        numSTA = expectedOccupancies(linkIdx) * 10;
        if numSTA > 0
            [staOBSSpositions, apOBSSposition] = randomPositionsFermat(numSTA, radiusList(linkIdx));
            obssAPCfg = wlanDeviceConfig(Mode="AP", MCS=mcs, BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW, TransmissionFormat="EHT-SU", ...
                NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=aggrLimit, TransmitPower=23);
            obssAP = wlanNode(Position=apOBSSposition, Name="OBSS AP " + linkIdx, DeviceConfig=obssAPCfg, PHYAbstractionMethod=phyAbstraction, MACFrameAbstraction=macAbstraction);
            apNodes(end+1) = obssAP;
            
            %sta Config
            staCfg(linkIdx) = wlanDeviceConfig(Mode="STA", MCS=mcs, BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW, TransmissionFormat="EHT-SU", ...
                NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=aggrLimit, TransmitPower=23);
            
            [pktSize, dataRate, onTime, offTime] = generateTrafficParamsOBSS(maxThr);
    
            staNodesList{linkIdx} = wlanNode.empty;
            for staIdx = 1:numSTA
                staNodesList{linkIdx}(staIdx) = wlanNode(Position=staOBSSpositions(staIdx,:), Name="OBSS STA " + linkIdx + "." + staIdx, DeviceConfig=staCfg(linkIdx), PHYAbstractionMethod=phyAbstraction, MACFrameAbstraction=macAbstraction);
                
            end
            
            associateStations(obssAP, staNodesList{linkIdx})
                
            for staIdx = 1:numSTA
                trafficObss(staIdx) = networkTrafficOnOff(DataRate=dataRate, PacketSize=pktSize, OnTime=onTime, OffTime=offTime);
                addTrafficSource(staNodesList{linkIdx}(staIdx), trafficObss(staIdx), DestinationNode=obssAP);
                
            end
        end
    end

    if isempty(apNodes)
        nodes = [staNode apNode];

    else

        % flatten
        flattenedStaNodes = [staNodesList{:}];
        staNodes = [ staNode flattenedStaNodes ];
        nodes = [staNodes apNode apNodes ];
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

    apThroughput = throughput(perfViewerObj, apNode.ID);
    avgLatency = averageReceiveLatency(perfViewerObj, staNode.ID);
    thr = apThroughput;

    
