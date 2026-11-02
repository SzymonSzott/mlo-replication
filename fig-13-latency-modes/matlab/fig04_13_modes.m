function [thr, avgLatency, latency99list] = fig04_13_modes(randnum, simTime, radiusList, isSTR, numBSS, bandAndChannel,channelBW, mcs, maxThr, isFullBuffer, expectedLoad)
 
%kod do odtworzenia wykresu 3, 4, 5 z badania 10118829, 

    % Set seed of random number generator for reproducible results
    rng(randnum,"combRecursive");
    % Set simulation time
    simulationTime = simTime;

    phyAbstraction="tgax-evaluation-methodology";
    macAbstraction = true;
    
    % Create wireless network simulator
    networkSimulator = wirelessNetworkSimulator.init;
    % calculate number of links
    numLinks = size(bandAndChannel,1);
    staNodes = wlanNode.empty();
    apNodes = wlanNode.empty();
    
    aggregationLimit = 1024;
    
    [pktSize, dataRate, onTime, offTime] = generateTrafficParams(maxThr, numBSS, expectedLoad);
    
   
    if isSTR
        
            for linkIdx = 1:numLinks
                apLinkCfg(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=aggregationLimit, TransmitPower=20);
                staSTRLinkCfg(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=aggregationLimit, TransmitPower=20);
            end
            
            apMLDCfg = wlanMultilinkDeviceConfig(Mode="AP",LinkConfig=apLinkCfg);
            staSTRMLDCfg = wlanMultilinkDeviceConfig(Mode="STA",LinkConfig=staSTRLinkCfg);
            

            for idx = 1:numBSS
                
                [staPosition, apPosition] = randomPositionsFermat(1, radiusList(idx));
                apNodes(idx) = wlanNode(Position=apPosition,Name="AP",DeviceConfig=apMLDCfg, PHYAbstractionMethod=phyAbstraction,MACFrameAbstraction=macAbstraction);
                staNodes(idx) = wlanNode( ...
                    Position=staPosition, ...
                    Name="STR STA " + idx, ...
                    DeviceConfig=staSTRMLDCfg, ...
                    PHYAbstractionMethod=phyAbstraction, ...
                    MACFrameAbstraction=macAbstraction);
                
                if isFullBuffer
                    associateStations(apNodes(idx),staNodes(idx), FullBufferTraffic="DL");
                else
                    associateStations(apNodes(idx),staNodes(idx));
                    trafficSource(idx) = networkTrafficOnOff(DataRate=dataRate,PacketSize=pktSize,OnTime=onTime,OffTime=offTime);       
                    addTrafficSource(apNodes(idx),trafficSource(idx),DestinationNode=staNodes(idx))
                end
            end


    else
        
        apLinkCfg = wlanLinkConfig(BandAndChannel=bandAndChannel,ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=numLinks, MPDUAggregationLimit=aggregationLimit, TransmitPower=20);
        staEMLSRLinkCfg = wlanLinkConfig(BandAndChannel=bandAndChannel,ChannelBandwidth=channelBW, NumTransmitAntennas=1,NumSpaceTimeStreams=1,MCS=mcs, MPDUAggregationLimit=aggregationLimit, TransmitPower=20);
        
        apMLDCfg = wlanMultilinkDeviceConfig(Mode="AP",LinkConfig=apLinkCfg);
        staEMLSRMLDCfg = wlanMultilinkDeviceConfig(Mode="STA",LinkConfig=staEMLSRLinkCfg,EnhancedMultilinkMode="EMLSR");

        
        for idx = 1:numBSS
            [staPosition, apPosition] = randomPositionsFermat(1, radiusList(idx));
            apNodes(idx) = wlanNode(Position=apPosition,Name="AP",DeviceConfig=apMLDCfg, PHYAbstractionMethod=phyAbstraction,MACFrameAbstraction=macAbstraction);
            staNodes(idx) = wlanNode( ...
                Position=staPosition, ...
                Name="EMLSR STA " + idx, ...
                DeviceConfig=staEMLSRMLDCfg, ...
                PHYAbstractionMethod=phyAbstraction, ...
                MACFrameAbstraction=macAbstraction);
          
             if isFullBuffer
                 associateStations(apNodes(idx),staNodes(idx), FullBufferTraffic="DL");
             else
                 associateStations(apNodes(idx),staNodes(idx));
                 trafficSource(idx) = networkTrafficOnOff(DataRate=dataRate,PacketSize=pktSize,OnTime=onTime,OffTime=offTime);       
                 addTrafficSource(apNodes(idx),trafficSource(idx),DestinationNode=staNodes(idx))
             end
        end
        

    end

       
    
    nodes = [apNodes staNodes];

    % Add channel model to the simulator
    channel = hSLSTGaxMultiFrequencySystemChannel(nodes, PathLossModel = 'residential');
    addChannelModel(networkSimulator,channel.ChannelFcn);

    % Add nodes to network simulator
    addNodes(networkSimulator,nodes);

    % Create node performance visualization object
    perfViewerObj = hPerformanceViewer(nodes,simulationTime);

    % Run the simulation
    run(networkSimulator,simulationTime);
   
    % Calculate throughput at AP
    apThroughput = throughput(perfViewerObj,[apNodes(:).ID]);
    avgLatency = averageReceiveLatency(perfViewerObj, [staNodes(:).ID]);
    latency99list = getpPacketLatencyVector(perfViewerObj, 99);
    
    thr = apThroughput;

end