#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace spring_response {
  struct Parameters { float mass=1, stiffness=250, dampening=25; };
  struct Sampled {
    std::array<float,65> values{};
    float horizon=1, minimum=0, maximum=1;
  };

  inline Parameters sanitize(Parameters p) {
    const auto bounded=[](float value,float fallback,float low,float high) {
      return std::isfinite(value)?std::clamp(value,low,high):fallback;
    };
    return {bounded(p.mass,1,0.51F,100),bounded(p.stiffness,250,0.51F,10000),
            bounded(p.dampening,25,0.51F,1000)};
  }

  inline float evaluate(float seconds, Parameters p) {
    p=sanitize(p); seconds=std::isfinite(seconds)?std::max(seconds,0.0F):0.0F;
    const float mass=p.mass, stiffness=p.stiffness;
    const float damping=p.dampening, omega0=std::sqrt(stiffness/mass);
    const float gamma=damping/(2*mass), displacement=-1;
    float value=0;
    if (gamma < omega0) {
      const float omegaD=std::sqrt(omega0*omega0-gamma*gamma);
      value=std::exp(-gamma*seconds)*(displacement*std::cos(omegaD*seconds)
          +gamma*displacement/omegaD*std::sin(omegaD*seconds));
    } else if (std::abs(gamma-omega0) <= std::max(omega0,1.0F)*0.0001F) {
      value=std::exp(-gamma*seconds)*(displacement+gamma*displacement*seconds);
    } else {
      const float root=std::sqrt(gamma*gamma-omega0*omega0), r1=-gamma+root, r2=-gamma-root;
      const float a=(-r2*displacement)/(r1-r2), b=displacement-a;
      value=a*std::exp(r1*seconds)+b*std::exp(r2*seconds);
    }
    return 1+value;
  }

  inline Sampled sample(Parameters p) {
    p=sanitize(p);
    Sampled out;
    const float gamma=std::max(p.dampening,0.0001F)/(2*std::max(p.mass,0.0001F));
    out.horizon=std::clamp(7/gamma,0.35F,3.0F);
    for (std::size_t i=0;i<out.values.size();++i) {
      const float value=evaluate(out.horizon*static_cast<float>(i)/(out.values.size()-1),p);
      out.values[i]=value; out.minimum=std::min(out.minimum,value); out.maximum=std::max(out.maximum,value);
    }
    const float padding=std::max(0.05F,(out.maximum-out.minimum)*0.08F);
    out.minimum-=padding; out.maximum+=padding;
    return out;
  }
}
