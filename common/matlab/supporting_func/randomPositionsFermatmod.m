function [staPositions, apPosition] = randomPositionsFermatmod(numStations, radius)
    %AP
    apPosition = [radius, radius, 4];
    
    %stale spirali Fermata
    goldenAngle = pi * (3 - sqrt(5));  % ≈ 137.5°

    %indeksy
    i = (1:numStations)';

    %promien znormalizowany
    r = sqrt(i / numStations) * radius * 0.9;
    theta = i * goldenAngle;

    %przeksztalcenie na kartezjanskie
    x = apPosition(1) + r .* cos(theta);
    y = apPosition(2) + r .* sin(theta);

    z = 0.8 + (1.8 - 0.8) * rand(numStations, 1);

    %pozycje stacji
    staPositions = [x, y, z];