// Native-size sharpening in q=sqrt(linear). Filtering remains on the complete
// E grid, but avoids five sRGB power functions, E8 quantization and the final
// RGB EOTF polynomial. Write q*q to an sRGB attachment; a raw UNORM alias is
// invalid for this algorithm. This is a perceptual approximation of E8 CAS,
// not a bit-exact rearrangement. The legacy path remains a separate reference.

// Monotone, endpoint-constrained cubic approximation of sRGB(q*q). Only the
// two neighborhood extrema need it; RGB and the high-pass stay in q space.
// The linear sRGB toe is exact. See scripts/shaders/validate_foveation_gamma2.py.
float gamma2_green_proxy(float q){
    if(q<=.05595355216606)return 12.92*q*q;
    if(q>=1.0)return 1.0;
    return clamp(((.19876842200756073*q-.52150958776474)*q+1.3565999269485474)*q-.03385872393846512,0.0,1.0);
}

// Bound the *encoded RGB* sharpening increment to 23/255, including colors
// whose red/blue channels cross the sRGB toe. A fixed q cap would unnecessarily
// weaken bright edges. This cubic is a conservative lower envelope of 1/f'(q),
// f(q)=sRGB(q*q), over the possible RGB interval. The 4e-6 downward margin
// covers fitted extrema and FP32 evaluation; the toe keeps the global floor.
float gamma2_sharpen_cap(vec3 base){
    float q=max(min(base.r,min(base.g,base.b))-.10259270,.05595355216606);
    float inverse_slope=((1.0546282662630735*q-2.129381314802799)*q+1.6043986913567392)*q+.6077911154768247;
    return min((23.0/255.0)*max(1.0/1.447,inverse_slope),.10259269);
}

float gamma2_sharpen_delta(vec3 base,vec4 neighbors){
    float lo=gamma2_green_proxy(min(base.g,min(min(neighbors.x,neighbors.y),min(neighbors.z,neighbors.w))));
    float hi=gamma2_green_proxy(max(base.g,max(max(neighbors.x,neighbors.y),max(neighbors.z,neighbors.w))));
    float t=min(lo,1.0-hi);
    float kt=.175*params.options.y*t;
    // Algebraically equivalent CAS weight in the chosen domain; one square
    // root and one division, instead of sqrt(t/hi) followed by another divide.
    float delta=kt*(4.0*base.g-dot(neighbors,vec4(1)))/max(sqrt(hi*t)-4.0*kt,1e-6);
    float cap=gamma2_sharpen_cap(base);
    return clamp(delta,-cap,cap);
}
