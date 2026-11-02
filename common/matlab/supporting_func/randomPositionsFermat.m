function [staPositions, apPosition] = randomPositionsFermat(numStations, radius)
    %AP
    apPosition = [radius, radius, 0];
    
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

    staPositions = [x, y, zeros(numStations, 1)];

   
end