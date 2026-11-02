function C = mlojobs(scen)
%MLOJOBS Parameter combinations for one figure scenario.
%   Returns a cell array of structs; the index into it is the job id.
C = {};
S = 10;            % simTime, same everywhere
B1 = [5 1];
B2 = [5 1; 5 100];
B3 = [5 1; 5 100; 5 160];
B4 = [5 1; 5 100; 6 1; 6 100];

switch scen

case "fig04_13"
  rl = [6 7]; mcs = 8; mt = 670; bw = 80e6;
  blocks = { ...
    {1, true , true , {B1,B2,B3}, 1}, ...
    {1, true , false, {B2,B3},    1}, ...
    {1, false, true , {B1,B2,B3}, [0.1 0.3 0.5 0.7 0.9]}, ...
    {1, false, false, {B2,B3},    [0.1 0.3 0.5 0.7 0.9]}, ...
    {2, false, true , {B1,B2,B3}, [0.1 0.3 0.5 0.7 0.9]}, ...
    {2, false, false, {B2,B3},    [0.1 0.3 0.5 0.7 0.9]} };
  for b = 1:numel(blocks)
    B = blocks{b};
    for ib = 1:numel(B{4}), for L = B{5}, for r = 1:7
      C{end+1} = struct('f',"fig04_13_modes",'a',{{r,S,rl,B{3},B{1},B{4}{ib},bw,mcs,mt,B{2},L}}); %#ok<AGROW>
    end, end, end
  end

case "fig12"
  rad = 5; mcs = 8; bw = 80e6; L = 1;
  for ib = 1:2
    bac = B2; if ib==2, bac = B4; end
    for mt = 100:100:3000, for r = 1:7
      C{end+1} = struct('f',"fig12_load_without_contention",'a',{{r,S,rad,true,bw,bac,mcs,mt,L}}); %#ok<AGROW>
    end, end
  end
  for mt = 100:100:3000, for r = 1:7
    C{end+1} = struct('f',"fig12_load_without_contention",'a',{{r,S,rad,false,bw,B1,mcs,mt,L}}); %#ok<AGROW>
  end, end

case "fig15"
  rl = [4.5 5 5.5 6]; mcs = 8; L = 1;
  for sh = [true false], for mt = [100 1000 2500], for r = 1:7
    C{end+1} = struct('f',"fig15_load_under_contention",'a',{{r,S,rl,true,sh,B4,mcs,mt,L,1024,"EHT-SU"}}); %#ok<AGROW>
  end, end, end
  for mt = [100 1000 2500], for r = 1:7
    C{end+1} = struct('f',"fig15_load_under_contention",'a',{{r,S,rl,false,true,B4,mcs,mt,L,1024,"EHT-SU"}}); %#ok<AGROW>
  end, end
  for mt = [100 1000 2500], for r = 1:7
    C{end+1} = struct('f',"fig15_load_under_contention",'a',{{r,S,rl,false,true,B4,mcs,mt,L,256,"HE-SU"}}); %#ok<AGROW>
  end, end

case "fig19_20"
  rad = 5; rl = [6 7]; bw = 80e6; mcs = 9; mt = 741;
  occs = {[0.1 0.1],[0.4 0.4],[0.7 0.7],[0.1 0.4],[0.1 0.7],[0.4 0.7]};
  for io = 1:6, for iM = [true false], for L = [0.2 0.4 0.6 0.8], for r = 1:7
    C{end+1} = struct('f',"fig19_20_channel_occupancy",'a',{{r,S,rad,rl,bw,iM,mcs,mt,L,occs{io}}}); %#ok<AGROW>
  end, end, end, end

case "fig07"
  rad = 1; rl = [2 4]; bw = 80e6; mcs = 13; mt = 1112; bac = [5 1; 6 1];
  occL = [0 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9 1];  % literals, not 0:0.1:1 - the colon form makes 0.3*10 non-integer
  for iS = [true false], for iU = [true false], for oc = occL, for r = 1:7
    C{end+1} = struct('f',"fig07_obss_activity",'a',{{r,S,rad,rl,true,iS,iU,bw,bac,mcs,mt,oc}}); %#ok<AGROW>
  end, end, end, end
  for iU = [true false], for oc = occL, for r = 1:7
    C{end+1} = struct('f',"fig07_obss_activity",'a',{{r,S,rad,rl,false,false,iU,bw,bac,mcs,mt,oc}}); %#ok<AGROW>
  end, end, end

case "fig08_09"
  rad = 5; rl = [4 6]; bw = 80e6; mcs = 11; bac = B2;
  mtl = [1339 1860 1854]; aggv = [64 512 1024];
  txops = {[0 0 0 0],[94 94 94 94]};
  for c1 = [0 0.1], for c2 = [0 0.1 0.4 0.7], for it = 1:2, for ia = 1:3, for r = 1:7
    C{end+1} = struct('f',"fig08_09_transmission_duration",'a',{{r,S,rad,rl,bac,aggv(ia),bw,mcs,true,mtl(ia),1,[c1 c2],txops{it}}}); %#ok<AGROW>
  end, end, end, end, end

case "fig10_11"
  rad = 5; mcs = 11;
  bwv = [80e6 160e6]; mtv = [927 1855];
  for ibw = 1:2
    bw = bwv(ibw); mt = mtv(ibw);
    for ib = 1:2
      bac = B1; if ib==2, bac = B2; end
      for n = [1 4 10], for L = [0.2 0.4 0.6 0.8 1], for r = 1:7
        C{end+1} = struct('f',"fig10_11_user_density",'a',{{r,S,n,rad,bw,bac,mcs,mt,L}}); %#ok<AGROW>
      end, end, end
    end
  end

case "fig21_22_23"
  rad = 7.5; mcs = 8; bw = 20e6;
  for iS = [true false], for lp = [10 20 30 40 50 60 70 80 90], for r = 1:7
    C{end+1} = struct('f',"fig21_22_23_legacy_coexistence",'a',{{r,S,50,lp,false,rad,iS,bw,mcs,"HE-SU"}}); %#ok<AGROW>
  end, end, end

case "fig25"
  rad = 3; bac = [6 39; 6 151];
  for iM = [true false], for bw = [20e6 40e6 80e6 160e6 320e6], for mcs = 0:13
    for r = 1:7
      C{end+1} = struct('f',"fig25_vr",'a',{{S,r,rad,bw,bac,mcs,1,true,iM}}); %#ok<AGROW>
    end
  end, end, end

case "fig06"
  rl = [5 6 7]; mcs = 8; bw = 80e6; bac = [5 1; 5 100];
  for iS = [true false], for r = 1:7
    C{end+1} = struct('f',"fig06_slo_mlo_coexistence",'a',{{r,S,rl,iS,bac,bw,mcs}}); %#ok<AGROW>
  end, end

case "fig17"
  rl = [4.5 5 5.5 6]; mcs = 8; L = 1; agg = 1024;
  bac = [5 1; 6 1; 5 100; 6 100; 6 200];
  for iS = [true false], for mt = [100 1000 2500], for r = 1:7
    C{end+1} = struct('f',"fig17_link_assignment",'a',{{r,S,rl,iS,false,bac,agg,mcs,mt,L}}); %#ok<AGROW>
  end, end, end
  for mt = [100 1000 2500], for r = 1:7
    C{end+1} = struct('f',"fig17_link_assignment",'a',{{r,S,rl,true,true,bac,agg,mcs,mt,L}}); %#ok<AGROW>
  end, end

otherwise
  error("nieznany scenariusz: %s", scen);
end
end
