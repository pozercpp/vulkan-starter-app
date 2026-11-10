#pragma once
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
inline const std::array<glm::vec3, 6> vertices = {{
    {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
}};
inline constexpr std::array<uint16_t, 24> indices = {{
    2,4,0, 2,1,4, 2,5,1, 2,0,5,
    3,0,4, 3,4,1, 3,1,5, 3,5,0
}};
