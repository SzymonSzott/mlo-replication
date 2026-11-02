function [thr, latency50, latency99] = fig17_link_assignment(randnum, simTime, radiusList, isSTR, allChannelsShared, bandAndChannel, aggrLimit, mcs, maxThr, expectedLoad)
 
%kod do odtworzenia wykresu 5 z badania 10293865

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
    numLinks = size(bandAndChannel,1);
    staNodes = wlanNode.empty();
    apNodes = wlanNode.empty();
    numSTA = 4;
    channelBW = 80e6;
    
    [pktSize, dataRate, onTime, offTime] = generateTrafficParams(maxThr, numSTA, expectedLoad);

    if isSTR
        if allChannelsShared %5 links all shared among 4 bss
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

        else % STR 1+1
            for linkIdx = 1:numLinks
                apLinkCfg(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=aggrLimit);
                staSTRLinkCfg(linkIdx) = wlanLinkConfig(BandAndChannel=bandAndChannel(linkIdx,:),ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=aggrLimit);
            end
            %apLinkCfg(numLinks) = wlanLinkConfig(BandAndChannel=bandAndChannel(numLinks,:),ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2);
            %staSTRLinkCfg(numLinks) = wlanLinkConfig(BandAndChannel=bandAndChannel(numLinks,:),ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2);
            
            
            apNewLinkCfg1 = [apLinkCfg(1) apLinkCfg(numLinks)];
            staNewSTRLinkCfg1 = [staSTRLinkCfg(1) staSTRLinkCfg(numLinks)];
            apNewLinkCfg2 = [apLinkCfg(2) apLinkCfg(numLinks)];
            staNewSTRLinkCfg2 = [staSTRLinkCfg(2) staSTRLinkCfg(numLinks)];
            apNewLinkCfg3 = [apLinkCfg(3) apLinkCfg(numLinks)];
            staNewSTRLinkCfg3 = [staSTRLinkCfg(3) staSTRLinkCfg(numLinks)];
            apNewLinkCfg4 = [apLinkCfg(4) apLinkCfg(numLinks)];
            staNewSTRLinkCfg4 = [staSTRLinkCfg(4) staSTRLinkCfg(numLinks)];

            apMLDCfg(1) = wlanMultilinkDeviceConfig(Mode="AP",LinkConfig=apNewLinkCfg1);
            staSTRMLDCfg(1) = wlanMultilinkDeviceConfig(Mode="STA",LinkConfig=staNewSTRLinkCfg1);
            apMLDCfg(2) = wlanMultilinkDeviceConfig(Mode="AP",LinkConfig=apNewLinkCfg2);
            staSTRMLDCfg(2) = wlanMultilinkDeviceConfig(Mode="STA",LinkConfig=staNewSTRLinkCfg2);
            apMLDCfg(3) = wlanMultilinkDeviceConfig(Mode="AP",LinkConfig=apNewLinkCfg3);
            staSTRMLDCfg(3) = wlanMultilinkDeviceConfig(Mode="STA",LinkConfig=staNewSTRLinkCfg3);
            apMLDCfg(4) = wlanMultilinkDeviceConfig(Mode="AP",LinkConfig=apNewLinkCfg4);
            staSTRMLDCfg(4) = wlanMultilinkDeviceConfig(Mode="STA",LinkConfig=staNewSTRLinkCfg4);
            
            

            for idx = 1:numSTA
                
                [staPosition, apPosition] = randomPositionsFermat(1, radiusList(idx));
                apNodes(idx) = wlanNode(Position=apPosition,Name="AP",DeviceConfig=apMLDCfg(idx), PHYAbstractionMethod=phyAbstraction,MACFrameAbstraction=macAbstraction);
                staNodes(idx) = wlanNode( ...
                    Position=staPosition, ...
                    Name="STR STA " + idx, ...
                    DeviceConfig=staSTRMLDCfg(idx), ...
                    PHYAbstractionMethod=phyAbstraction, ...
                    MACFrameAbstraction=macAbstraction);
              
                associateStations(apNodes(idx),staNodes(idx));

                trafficSource(idx) = networkTrafficOnOff(DataRate=dataRate,PacketSize=pktSize,OnTime=onTime,OffTime=offTime);       
                addTrafficSource(apNodes(idx),trafficSource(idx),DestinationNode=staNodes(idx))
            end
        end

    else
        %staIdx = 1;
        bandAndChannel12 = [5 1; 6 1]; % Wiersze 1 i 2
        bandAndChannel34 = [5 100; 6 100]; % Wiersze 3 i 4
        %bandAndChannel56 = bandAndChannel(5, :); % Wiersze 5 
        
        apLinkCfg12 = wlanLinkConfig(BandAndChannel=bandAndChannel12,ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2, MPDUAggregationLimit=aggrLimit);
        staEMLSR12LinkCfg = wlanLinkConfig(BandAndChannel=bandAndChannel12,ChannelBandwidth=channelBW, NumTransmitAntennas=1,NumSpaceTimeStreams=1,MCS=mcs, MPDUAggregationLimit=aggrLimit);
        
        apMLDCfg12 = wlanMultilinkDeviceConfig(Mode="AP",LinkConfig=apLinkCfg12);
        staEMLSR12MLDCfg = wlanMultilinkDeviceConfig(Mode="STA",LinkConfig=staEMLSR12LinkCfg,EnhancedMultilinkMode="EMLSR");

        
        apLinkCfg34 = wlanLinkConfig(BandAndChannel=bandAndChannel34,ChannelBandwidth=channelBW,MCS=mcs, NumSpaceTimeStreams=2, NumTransmitAntennas=2);
        staEMLSR34LinkCfg = wlanLinkConfig(BandAndChannel=bandAndChannel34,ChannelBandwidth=channelBW, NumTransmitAntennas=1,NumSpaceTimeStreams=1,MCS=mcs);
        
        apMLDCfg34 = wlanMultilinkDeviceConfig(Mode="AP",LinkConfig=apLinkCfg34);
        staEMLSR34MLDCfg = wlanMultilinkDeviceConfig(Mode="STA",LinkConfig=staEMLSR34LinkCfg,EnhancedMultilinkMode="EMLSR");
        
        for idx = 1:2
            [staPosition, apPosition] = randomPositionsFermat(1, radiusList(idx));
            apNodes(idx) = wlanNode(Position=apPosition,Name="AP",DeviceConfig=apMLDCfg12, PHYAbstractionMethod=phyAbstraction,MACFrameAbstraction=macAbstraction);
            staNodes(idx) = wlanNode( ...
                Position=staPosition, ...
                Name="EMLSR STA " + idx, ...
                DeviceConfig=staEMLSR12MLDCfg, ...
                PHYAbstractionMethod=phyAbstraction, ...
                MACFrameAbstraction=macAbstraction);
          
            associateStations(apNodes(idx),staNodes(idx));

            trafficSource(idx) = networkTrafficOnOff(DataRate=dataRate,PacketSize=pktSize,OnTime=onTime,OffTime=offTime);       
            addTrafficSource(apNodes(idx),trafficSource(idx),DestinationNode=staNodes(idx))
        end
        for idx = 1:2
            newIdx = idx + 2;
            [staPosition, apPosition] = randomPositionsFermat(1, radiusList(newIdx));
            apNodes(newIdx) = wlanNode(Position=apPosition,Name="AP",DeviceConfig=apMLDCfg34, PHYAbstractionMethod=phyAbstraction,MACFrameAbstraction=macAbstraction);
            staNodes(newIdx) = wlanNode( ...
                Position=staPosition, ...
                Name="EMLSR STA " + newIdx, ...
                DeviceConfig=staEMLSR34MLDCfg, ...
                PHYAbstractionMethod=phyAbstraction, ...
                MACFrameAbstraction=macAbstraction);
          
            associateStations(apNodes(newIdx),staNodes(newIdx));

            trafficSource(newIdx) = networkTrafficOnOff(DataRate=dataRate,PacketSize=pktSize,OnTime=onTime,OffTime=offTime);       
            addTrafficSource(apNodes(newIdx),trafficSource(newIdx),DestinationNode=staNodes(newIdx))
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