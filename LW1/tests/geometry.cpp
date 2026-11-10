#include "../src/geometry.hpp"
#include <cmath>
#include <iostream>
#include <map>
#include <utility>
int main() {
    std::map<std::pair<int,int>, int> edges;
    for (size_t i=0;i<indices.size();i+=3) {
        auto a=vertices[indices[i]], b=vertices[indices[i+1]], c=vertices[indices[i+2]];
        if (glm::dot(glm::cross(b-a,c-a),a+b+c)<=0) return 1;
        for(int j=0;j<3;++j) {
            int u=indices[i+j], v=indices[i+(j+1)%3];
            if(std::abs(glm::length(vertices[u]-vertices[v])-std::sqrt(2.f))>1e-6f) return 2;
            if(u>v) std::swap(u,v);
            ++edges[{u,v}];
        }
    }
    if(edges.size()!=12) return 3;
    for(auto edge:edges) if(edge.second!=2) return 4;
    std::cout << "Regular closed octahedron: 6 vertices, 12 edges, 8 outward faces\n";
}
