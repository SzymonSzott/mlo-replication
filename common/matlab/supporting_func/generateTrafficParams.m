function [pktSize, dataRate, onTime, offTime] = generateTrafficParams(maxThr, numSTA, expectedLoad)
    
    % INPUTS:
    % maxThr - max received thr in Mbps
    % numSTA - number of stations
    % expectedLoad - expected load (part of max thr)

    % OUTPUTS:
    % params for networkTrafficOnOff obj

    maxThr_bps = maxThr * 1e6;  %max received thr in bps

    totalOfferedTraffic = expectedLoad * maxThr_bps;  
    trafficPerUser = totalOfferedTraffic / numSTA;  
    
    pktSizeBits = 1500 * 8;  
    pktRate = trafficPerUser / pktSizeBits;  
    burstInterval = 8.3e-3;  
    pktsPerBurst = max(1, round(pktRate * burstInterval));  

    onTime = pktsPerBurst * 1e-3;  
    offTime = burstInterval - onTime;  
    offTime = max(offTime, 1e-9);  

    pktSize = 1500;  
    dataRate = trafficPerUser / 1000;  %rate in kbps
end
