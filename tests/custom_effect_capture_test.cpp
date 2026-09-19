#include "render/custom_effect/custom_effect_capture.h"

#include <cassert>
#include <cmath>
#include <limits>

int main() {
  const auto ordinary=customEffectCapture(0,0,200,100,200,100,40,20,4,Mat3::translation(20,10));
  assert(ordinary);
  assert(ordinary->x==16 && ordinary->y==66 && ordinary->width==48 && ordinary->height==28);
  assert(ordinary->axisXx==1.0F && ordinary->axisYy==-1.0F);

  const auto scaled=customEffectCapture(10,20,400,200,200,100,40,20,4,
      Mat3::translation(20,10)*Mat3::scale(2,0.5F));
  assert(scaled);
  assert(scaled->x>=10 && scaled->y>=20);
  assert(scaled->x+scaled->width<=410 && scaled->y+scaled->height<=220);
  assert(scaled->axisXx==4.0F && scaled->axisYy==-1.0F);

  constexpr float pi=3.14159265358979323846F;
  const auto rotated=customEffectCapture(0,0,300,300,300,300,60,20,8,
      Mat3::translation(100,100)*Mat3::rotation(pi*0.5F));
  assert(rotated && rotated->width>20 && rotated->height>60);

  const auto mirrored=customEffectCapture(0,0,200,100,200,100,40,20,4,
      Mat3::translation(100,40)*Mat3::scale(-1,-1));
  assert(mirrored && mirrored->width==48 && mirrored->height==28);

  auto shear=Mat3::identity();
  shear.m[3]=1.0F;
  const auto sheared=customEffectCapture(0,0,300,200,300,200,40,20,10,shear);
  assert(sheared);
  // The largest singular value of [[1,1],[0,1]] is the golden ratio, so a
  // 10px local sampling disk needs 17px integer capture padding.
  assert(sheared->x==0 && sheared->width==77 && sheared->height==37);

  assert(!customEffectCapture(0,0,0,100,200,100,40,20,4,Mat3::identity()));
  assert(!customEffectCapture(0,0,200,100,200,100,0,20,4,Mat3::identity()));
  assert(!customEffectCapture(0,0,200,100,200,100,40,20,4,Mat3::scale(0,0)));
  auto invalid=Mat3::identity();
  invalid.m[0]=std::numeric_limits<float>::quiet_NaN();
  assert(!customEffectCapture(0,0,200,100,200,100,40,20,4,invalid));

  const auto bounded=customEffectCapture(0,0,64,64,64,64,32,32,100000,Mat3::identity());
  assert(bounded && bounded->x==0 && bounded->y==0 && bounded->width==64 && bounded->height==64);
  return 0;
}
