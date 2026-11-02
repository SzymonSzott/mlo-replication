function [pktSize, dataRate, onTime, offTime] = generateTrafficParamsOBSS(maxThr)
    
    % INPUTS:
    % maxThr - max received thr in Mbps

    % OUTPUTS:
    % params for networkTrafficOnOff obj

    maxThr_bps = maxThr * 1e6;  %max received thr in bps

    trafficPerUser = 0.1 * maxThr_bps;   
    
    pktSizeBits = 1500 * 8;  
    pktRate = trafficPerUser / pktSizeBits;  
    burstInterval = 8.3e-3;  
    pktsPerBurst = max(1, round(pktRate * burstInterval));  

    onTime = pktsPerBurst * 1e-3;  
    offTime = burstInterval - onTime;  
    offTime = max(offTime, 1e-6);  

    pktSize = 1500;  
    dataRate = trafficPerUser / 1000;  %rate in kbps
end
