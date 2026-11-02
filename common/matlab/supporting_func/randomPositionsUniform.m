function [staPositions, apPosition] = randomPositionsUniform(numStations, radius)
    apPosition = [radius, radius, 0];

    r = radius * 0.9 * sqrt(rand(numStations, 1));
    theta = 2 * pi * rand(numStations, 1);

    x = apPosition(1) + r .* cos(theta);
    y = apPosition(2) + r .* sin(theta);

    staPositions = [x, y, zeros(numStations, 1)];
end
