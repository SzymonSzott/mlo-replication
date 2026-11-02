function thr = fig21_22_23_legacy_coexistence(randnum, simTime, numSTA, legacyPerc, caseA, radius, isSTR, channelBW, mcs, transmissionFormat)
   

    % Set seed of random number generator for reproducible results
    rng(randnum,"combRecursive");
    % Set simulation time
    simulationTime = simTime;

    %mcs = 11;
    staNodes = wlanNode.empty();
    staIdx = 1;

    phyAbstraction="tgax-evaluation-methodology";
    macAbstraction = true;
    
    aggregationLimit = 64;
    transmissionPower = 15;
 
    
    % Create wireless network simulator
    networkSimulator = wirelessNetworkSimulator.init;
    %determine number of links
    
    bandAndChannelAP = [2.4 1; 5 36; 6 1];
    numLinksAP = size(bandAndChannelAP, 1);

    if caseA
        bandAndChannel = [5 36; 6 1]; %case A MLO will use only two channels
    else
        bandAndChannel = [2.4 1; 5 36; 6 1]; %case B MLO will use all channels
    end

    numLinks = size(bandAndChannel,1);
    
    numStaSLO = round(numSTA*(legacyPerc/100));
    numStaMLO = numSTA - numStaSLO;
    
    
    %main station and access point positions
    [staPositions, apPosition] = randomPositionsFermatmod(numSTA, radius);
    


    if isSTR
        % Create the link config objects for AP MLD, STR STA MLD
        for linkAP = 1:numLinksAP
        apLinkCfg(linkAP) = wlanLinkConfig(...
                BandAndChannel=bandAndChannelAP(linkAP,:),...
                ChannelBandwidth=channelBW,...
                MCS=mcs,...
                NumSpaceTimeStreams=2,...
                NumTransmitAntennas=2,...
                MPDUAggregationLimit=aggregationLimit,...
                TransmitPower=transmissionPower);

        end
        for linkIdx = 1:numLinks
            
            staSTRLinkCfg(linkIdx) = wlanLinkConfig(...
                BandAndChannel=bandAndChannel(linkIdx,:),...
                ChannelBandwidth=channelBW,...
                MCS=mcs, ...
                NumSpaceTimeStreams=2, ...
                NumTransmitAntennas=2,...
                MPDUAggregationLimit=aggregationLimit,...
                TransmitPower=transmissionPower);
        end
    
        % Create MLD config objects for AP MLD, STR STA MLD
        apMLDCfg = wlanMultilinkDeviceConfig(Mode="AP",LinkConfig=apLinkCfg);
        staSTRMLDCfg = wlanMultilinkDeviceConfig(Mode="STA",LinkConfig=staSTRLinkCfg);
    
        apNode = wlanNode(Position=apPosition,Name="AP",DeviceConfig=apMLDCfg, PHYAbstractionMethod=phyAbstraction,MACFrameAbstraction=macAbstraction);
        
        for idx = 1:numStaMLO
            staNodes(staIdx) = wlanNode( ...
                Position=staPositions(staIdx, :), ...
                Name="STR STA" +numStaMLO, ...
                DeviceConfig=staSTRMLDCfg, ...
                PHYAbstractionMethod=phyAbstraction, ...
                MACFrameAbstraction=macAbstraction); 
            staIdx = staIdx + 1;
        end
        
    else
        %EMLSR AP i MLD
        apLinkCfg = wlanLinkConfig( ...
            BandAndChannel=bandAndChannel, ...
            ChannelBandwidth=channelBW,...
            MCS=mcs, ...
            NumSpaceTimeStreams=2, ...
            NumTransmitAntennas=numLinks, ...
            MPDUAggregationLimit=aggregationLimit,...
            TransmitPower=transmissionPower);
        staEMLSRLinkCfg = wlanLinkConfig( ...
            BandAndChannel=bandAndChannel, ...
            ChannelBandwidth=channelBW, ...
            NumTransmitAntennas=1, ...
            NumSpaceTimeStreams=1, ...
            MCS=mcs, ...
            MPDUAggregationLimit=aggregationLimit,...
            TransmitPower=transmissionPower);
        
        %Config objects for EMLSR
        apMLDCfg = wlanMultilinkDeviceConfig(Mode="AP",LinkConfig=apLinkCfg);
        staEMLSRMLDCfg = wlanMultilinkDeviceConfig(Mode="STA",LinkConfig=staEMLSRLinkCfg,EnhancedMultilinkMode="EMLSR");

        apNode = wlanNode(Position=apPosition,Name="AP",DeviceConfig=apMLDCfg, PHYAbstractionMethod=phyAbstraction,MACFrameAbstraction=macAbstraction);
        
        for idx = 1:numStaMLO
            staNodes(staIdx) = wlanNode( ...
                Position=staPositions(staIdx,:), ...
                Name="EMLSR STA " + idx, ...
                DeviceConfig=staEMLSRMLDCfg, ...
                PHYAbstractionMethod=phyAbstraction, ...
                MACFrameAbstraction=macAbstraction);
            staIdx = staIdx + 1;

        end

    end

    
    %Single link stations 
    staSingleCfg = wlanDeviceConfig(...
        Mode="STA", ...
        BandAndChannel=[2.4 1], ...
        ChannelBandwidth=channelBW, ...
        MCS=mcs,...
        TransmissionFormat=transmissionFormat, ...
        NumSpaceTimeStreams=2, ...
        NumTransmitAntennas=2, ...
        MPDUAggregationLimit=aggregationLimit, ...
        TransmitPower=transmissionPower);

    for idx = 1:numStaSLO
        staNodes(staIdx) = wlanNode( ...
            Position=staPositions(staIdx,:), ...
            Name="SL STA "+ idx, ...
            DeviceConfig=staSingleCfg, ...
            PHYAbstractionMethod=phyAbstraction, ...
            MACFrameAbstraction=macAbstraction);
        staIdx = staIdx + 1;

    end
        

    
    % Associate the STAs to AP MLD 
    associateStations(apNode,staNodes, FullBufferTraffic="UL");
    nodes = [ apNode staNodes];

    
    % Add channel model to the simulator
    channel = hSLSTGaxMultiFrequencySystemChannel(nodes, PathLossModel = 'residential');
    addChannelModel(networkSimulator,channel.ChannelFcn);

    % Add nodes to network simulator
    addNodes(networkSimulator,nodes);

    % Create node performance visualization object
    perfViewerObj = hPerformanceViewer(nodes,simulationTime);

    % Run the simulation
    run(networkSimulator,simulationTime);
   
    
    % Calculate throughput at STAs if uplink
    throughputs = throughput(perfViewerObj,[staNodes(:).ID]);
    

    latency = getpPacketLatencyVector(perfViewerObj, 95);
    thr = throughputs;

    