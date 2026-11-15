#pragma once
#include <array>
#include <cmath>

// Double-precision reference for color-coordinate assertions only. Does not
// duplicate production protection/compression/gamut/frequency algorithms.
namespace ColorReference {
using Color = std::array<double,3>;
inline Color ToLab(Color c) {
    for (auto& v:c) v=v<=.04045 ? v/12.92 : std::pow((v+.055)/1.055,2.4);
    const double l=std::cbrt(.4122214708*c[0]+.5363325363*c[1]+.0514459929*c[2]);
    const double m=std::cbrt(.2119034982*c[0]+.6806995451*c[1]+.1073969566*c[2]);
    const double s=std::cbrt(.0883024619*c[0]+.2817188376*c[1]+.6299787005*c[2]);
    return {.2104542553*l+.7936177850*m-.0040720468*s,
        1.9779984951*l-2.4285922050*m+.4505937099*s,
        .0259040371*l+.7827717662*m-.8086757660*s};
}
inline Color FromLab(Color c) {
    const double l=std::pow(c[0]+.3963377774*c[1]+.2158037573*c[2],3);
    const double m=std::pow(c[0]-.1055613458*c[1]-.0638541728*c[2],3);
    const double s=std::pow(c[0]-.0894841775*c[1]-1.2914855480*c[2],3);
    Color rgb{4.0767416621*l-3.3077115913*m+.2309699292*s,
        -1.2684380046*l+2.6097574011*m-.3413193965*s,
        -.0041960863*l-.7034186147*m+1.7076147010*s};
    for (auto& v:rgb) v=v<=.0031308 ? v*12.92 : 1.055*std::pow(v,1/2.4)-.055;
    return rgb;
}
}
