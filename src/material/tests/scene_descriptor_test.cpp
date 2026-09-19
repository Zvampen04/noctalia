#include "scene_descriptor.h"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace noctalia::material;
#define CHECK(x) do { if (!(x)) { std::cerr << "failed: " #x << '\n'; std::exit(1); } } while (false)
int main() {
  SceneDescriptor scene{800,600,{}};
  ScenePlane p; p.group=7; p.width=400;p.height=80;p.clip={0,0,800,600};
  p.transform={0.75F,0.2F,-0.2F,0.75F,50,100};p.radii={20,18,12,8};p.concaveCorners=2;
  p.parameters.primitive=Primitive::Optical;p.parameters.optical.thickness=27;
  p.surface="bar";scene.planes.push_back(p);
  scene.planes[0].parameters.plateau.faceCurvature=-.75F;
  scene.planes[0].parameters.plateau.faceHighlight=.2F;
  scene.planes[0].parameters.plateau.faceShadow=.3F;
  scene.planes[0].parameters.plateau.shadowDistance=17.F;
  scene.planes[0].parameters.plateau.shadowIntensity=.44F;
  scene.planes[0].parameters.optical.planeMode=1;
  scene.planes[0].parameters.optical.backdropBrightness=.2F;
  scene.planes[0].parameters.optical.backdropContrast=1.2F;
  scene.planes[0].parameters.optical.backdropSaturation=1.8F;
  scene.planes[0].parameters.optical.rimWidth=0;
  scene.planes[0].parameters.optical.rimFalloff=3;
  scene.planes[0].parameters.optical.refractionRadius=250;
  scene.planes[0].parameters.optical.lensMapping=1;
  scene.planes[0].parameters.optical.lensStrength=.3F;
  scene.planes[0].parameters.optical.lensFalloff=4;
  scene.planes[0].cornerPower=4;
  scene.planes[0].paintClip=SceneRoundedClip{{-10,0,200,50},20,10};
  auto bytes=encodeScene(scene);CHECK(!bytes.empty());CHECK(decodeScene(bytes)==scene);
  for (std::size_t n=0;n<bytes.size();++n) CHECK(!decodeScene(std::span(bytes.data(),n)));
  auto trailing=bytes;trailing.push_back(0);CHECK(!decodeScene(trailing));
  auto unsupported=bytes;unsupported[4]=static_cast<std::uint8_t>(kSceneVersion+1);CHECK(!decodeScene(unsupported));
  auto oldVersion=bytes;oldVersion[4]=4;CHECK(!decodeScene(oldVersion));
  for(float power : {1.0F,10.1F,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
    auto invalid=scene;invalid.planes[0].cornerPower=power;CHECK(encodeScene(invalid).empty());
    invalid=scene;invalid.planes[0].paintClip->cornerPower=power;CHECK(encodeScene(invalid).empty());
  }
  auto bad=scene;bad.planes[0].transform.fill(0);CHECK(encodeScene(bad).empty());
  bad=scene;bad.planes[0].parameters.optical.thickness=std::numeric_limits<float>::quiet_NaN();CHECK(encodeScene(bad).empty());
  bad=scene;bad.planes[0].parameters.optical.refractiveIndex=0.9F;CHECK(encodeScene(bad).empty());
  bad=scene;bad.planes[0].surface="../client";CHECK(encodeScene(bad).empty());
  bad=scene;bad.planes.push_back(p);CHECK(encodeScene(bad).empty());
  bad=scene;bad.planes[0].parameters.optical.planeMode=.5F;CHECK(encodeScene(bad).empty());
  bad=scene;bad.planes[0].parameters.optical.lensMapping=.5F;CHECK(encodeScene(bad).empty());
  bad=scene;bad.planes[0].parameters.optical.refractionRadius=513;CHECK(encodeScene(bad).empty());
  bad=scene;bad.planes[0].parameters.optical.lensFalloff=0;CHECK(encodeScene(bad).empty());
  bad=scene;bad.planes[0].paintClip->radius=30;CHECK(encodeScene(bad).empty());
  bad=scene;bad.planes[0].paintClip->bounds.x=std::numeric_limits<float>::infinity();CHECK(encodeScene(bad).empty());
  bad=scene;bad.planes[0].clip.width=900;CHECK(encodeScene(bad).empty());
  bad=scene;bad.planes.clear();CHECK(decodeScene(encodeScene(bad))==bad);
  bad=scene;bad.planes.resize(kMaxScenePlanes);
  for (std::size_t i=0;i<bad.planes.size();++i) {bad.planes[i]=p;bad.planes[i].group=i+1;}
  CHECK(decodeScene(encodeScene(bad))==bad);bad.planes.push_back(p);CHECK(encodeScene(bad).empty());
  std::cout << "scene descriptor bounds, roundtrip, geometry and atomic rejection passed\n";
}
