function [thr, latency50, latency99] = fig15_load_under_contention(randnum, simTime, radiusList, isMLO, allChannelsShared, bandAndChannel, mcs, maxThr, expectedLoad, aggrLimit, transmissionFormat)
 
%kod do odtworzenia wykresu 4a z badania 10293865

    %total traffic evenly spread among all BSSs, i.e., one quarter each

    % Set seed of random number generator for reproducible results
    rng(randnum,"combRecursive");
    % Set simulation time
    simulationTime = simTime;

    phyAbstraction="tgax-evaluation-methodology";
    macAbstraction = true;
    
    % Create wireless network simulator
    networkSimulator = wirelessNetworkSimulator.init;
    % calculate number of links
    bandAndChannel12 = [5 1; 6 1];
    %numLinks12 = size(bandAndChannel12,1);
    
    bandAndChannel34 = [5 100; 6 100];
    %numLinks34 = size(bandAndChannel34,1);

    bandAndChannel = [5 1; 5 100; 6 1; 6 100];
    numLinks = size(bandAndChannel,1);

    staNodes = wlanNode.empty();
    apNodes = wlanNode.empty();
    numSTA = 4;
    channelBW = 80e6;
    
    [pktSize, dataRate, onTime, offTime] = generateTrafficParams(maxThr, numSTA, expectedLoad);

    if isMLO
        if allChannelsShared
            for linkIdx = 1:numLinks
                apLinkCfg(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=aggrLimit);
                staSTRLinkCfg(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=aggrLimit);
            end
            
            apMLDCfg = wlanMultilinkDeviceConfig(Mode="AP",LinkConfig=apLinkCfg);
            staSTRMLDCfg = wlanMultilinkDeviceConfig(Mode="STA",LinkConfig=staSTRLinkCfg);
            

            for idx = 1:numSTA
                
                [staPosition, apPosition] = randomPositionsFermat(1, radiusList(idx));
                apNodes(idx) = wlanNode(Position=apPosition,Name="AP",DeviceConfig=apMLDCfg, PHYAbstractionMethod=phyAbstraction,MACFrameAbstraction=macAbstraction);
                staNodes(idx) = wlanNode( ...
                    Position=staPosition, ...
                    Name="STR STA " + idx, ...
                    DeviceConfig=staSTRMLDCfg, ...
                    PHYAbstractionMethod=phyAbstraction, ...
                    MACFrameAbstraction=macAbstraction);
              
                associateStations(apNodes(idx),staNodes(idx));

                trafficSource(idx) = networkTrafficOnOff(DataRate=dataRate,PacketSize=pktSize,OnTime=onTime,OffTime=offTime);       
                addTrafficSource(apNodes(idx),trafficSource(idx),DestinationNode=staNodes(idx))
            end

        else
            %staIdx = 1;
            for linkIdx = 1:2
                apLinkCfg12(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel12(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=aggrLimit);
                staSTRLinkCfg12(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel12(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=aggrLimit);
                apLinkCfg34(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel34(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=aggrLimit);
                staSTRLinkCfg34(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel34(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=aggrLimit);
            end
            apMLDCfg12 = wlanMultilinkDeviceConfig(Mode="AP",LinkConfig=apLinkCfg12);
            staSTRMLDCfg12 = wlanMultilinkDeviceConfig(Mode="STA",LinkConfig=staSTRLinkCfg12);
            apMLDCfg34 = wlanMultilinkDeviceConfig(Mode="AP",LinkConfig=apLinkCfg34);
            staSTRMLDCfg34 = wlanMultilinkDeviceConfig(Mode="STA",LinkConfig=staSTRLinkCfg34);
            
            for idx = 1:2
                [staPosition, apPosition] = randomPositionsFermat(1, radiusList(idx));
                apNodes(idx) = wlanNode(Position=apPosition,Name="AP",DeviceConfig=apMLDCfg12, PHYAbstractionMethod=phyAbstraction,MACFrameAbstraction=macAbstraction);
                staNodes(idx) = wlanNode( ...
                    Position=staPosition, ...
                    Name="STR STA " + idx, ...
                    DeviceConfig=staSTRMLDCfg12, ...
                    PHYAbstractionMethod=phyAbstraction, ...
                    MACFrameAbstraction=macAbstraction);
              
                associateStations(apNodes(idx),staNodes(idx));

                trafficSource(idx) = networkTrafficOnOff(DataRate=dataRate,PacketSize=pktSize,OnTime=onTime,OffTime=offTime);       
                addTrafficSource(apNodes(idx),trafficSource(idx),DestinationNode=staNodes(idx))
            end
            for idx = 1:2
                idxNew = idx + 2;
                [staPosition, apPosition] = randomPositionsFermat(1, radiusList(idxNew));
                apNodes(idxNew) = wlanNode(Position=apPosition,Name="AP",DeviceConfig=apMLDCfg34, PHYAbstractionMethod=phyAbstraction,MACFrameAbstraction=macAbstraction);
                staNodes(idxNew) = wlanNode( ...
                    Position=staPosition, ...
                    Name="STR STA " + (idxNew), ...
                    DeviceConfig=staSTRMLDCfg34, ...
                    PHYAbstractionMethod=phyAbstraction, ...
                    MACFrameAbstraction=macAbstraction);
              
                associateStations(apNodes(idxNew),staNodes(idxNew));

                trafficSource(idxNew) = networkTrafficOnOff(DataRate=dataRate,PacketSize=pktSize,OnTime=onTime,OffTime=offTime);       
                addTrafficSource(apNodes(idxNew),trafficSource(idxNew),DestinationNode=staNodes(idxNew))
            end

        end

    else
        for linkIdx = 1:numLinks
            [staPosition, apPosition] = randomPositionsFermat(1, radiusList(linkIdx));
            apCfg(linkIdx) = wlanDeviceConfig(Mode="AP", MCS=mcs, BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW, TransmissionFormat=transmissionFormat, MPDUAggregationLimit=aggrLimit, ...
                NumSpaceTimeStreams=2, NumTransmitAntennas=2);
            apNodes(linkIdx) = wlanNode(Position=apPosition, Name="AP " + linkIdx, DeviceConfig=apCfg, PHYAbstractionMethod=phyAbstraction, MACFrameAbstraction=macAbstraction);
        
            %sta Config
            staCfg(linkIdx) = wlanDeviceConfig(Mode="STA", MCS=mcs, BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW, TransmissionFormat=transmissionFormat, MPDUAggregationLimit=aggrLimit, ...
                NumSpaceTimeStreams=2, NumTransmitAntennas=2);
        
            staNodes(linkIdx) = wlanNode(Position=staPosition, Name="STA " + linkIdx, DeviceConfig=staCfg(linkIdx), PHYAbstractionMethod=phyAbstraction, MACFrameAbstraction=macAbstraction);
               
            associateStations(apNodes(linkIdx), staNodes(linkIdx))
            
            traffic(linkIdx) = networkTrafficOnOff(DataRate=dataRate, PacketSize=pktSize, OnTime=onTime, OffTime=offTime);
            addTrafficSource(apNodes(linkIdx), traffic(linkIdx), DestinationNode=staNodes(linkIdx));
         
        end

    end
    
    nodes = [apNodes staNodes];

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
    apThroughput = throughput(perfViewerObj,[apNodes(:).ID]);

    latency50 = getpPacketLatencyVector(perfViewerObj, 50);
    latency99 = getpPacketLatencyVector(perfViewerObj, 99);
   
    thr = apThroughput;

end