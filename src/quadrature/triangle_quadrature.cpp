
#include "triangle_quadrature.hpp"

#include <algorithm>

namespace neon
{
triangle_quadrature::triangle_quadrature(Rule rule)
{
    switch (rule)
    {
        case Rule::OnePoint:
        {
            w = {1.0};
            clist = {{0, 1.0 / 3.0, 1.0 / 3.0}};
            break;
        }
        case Rule::ThreePoint:
        {
            w = {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
            clist = {{0, 2.0 / 3.0, 1.0 / 6.0}, {1, 1.0 / 6.0, 2.0 / 3.0}, {2, 1.0 / 6.0, 1.0 / 6.0}};
            break;
        }
        case Rule::FourPoint:
        {
            w = {-0.5625, 0.5208333333333333333333, 0.5208333333333333333333, 0.5208333333333333333333};
            clist = {{0, 1.0 / 3.0, 1.0 / 3.0}, {1, 0.6, 0.2}, {2, 0.2, 0.6}, {3, 0.2, 0.2}};
            break;
        }
    }
    std::transform(begin(w), end(w), begin(w), [](auto const i) { return 0.5 * i; });
}
}
