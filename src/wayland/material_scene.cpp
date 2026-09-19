#include "wayland/material_scene.h"
#include "wayland/custom_effect_transport.h"
#include "render/core/mat3.h"
#include "render/scene/render_traversal_guard.h"
#include "noctalia-material-v1-client-protocol.h"
#include "render/scene/rect_node.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include "material/scene_transport.h"
#include "material/custom_effect_lease.h"

namespace {
using namespace noctalia::material;
SceneRect intersect(SceneRect a, SceneRect b) {
  const float right = std::min(a.x + a.width, b.x + b.width);
  const float bottom = std::min(a.y + a.height, b.y + b.height);
  a.x = std::max(a.x, b.x); a.y = std::max(a.y, b.y);
  a.width = std::max(0.0F, right - a.x); a.height = std::max(0.0F, bottom - a.y);
  return a;
}
struct CollectedScene {
  SceneDescriptor scene;
  std::vector<std::shared_ptr<const CustomEffectAsset>> assets;
};

void collect(const Node* node, SceneRect clip, float opacity, bool parentPlane, CollectedScene& collected,
             const Mat3& parentTransform, RenderTraversalGuard::Stack& stack,
             bool retainTransparentCustom, bool ignoreNodeOpacity = false) {
  auto& scene = collected.scene;
  if (!node || node->materialBackdropLocal() || !node->visible()
      || (!retainTransparentCustom && !ignoreNodeOpacity && node->opacity() <= 0)
      || scene.planes.size() >= kMaxScenePlanes) return;
  RenderTraversalGuard guard(stack, node);
  if (!guard) return;
  if (!ignoreNodeOpacity) opacity *= node->opacity();
  if (opacity <= 0 && !retainTransparentCustom) return;
  const float cx = node->transformOriginX(), cy = node->transformOriginY();
  const auto world = parentTransform * Mat3::translation(node->x(), node->y())
      * Mat3::translation(cx, cy) * Mat3::rotation(node->rotation())
      * Mat3::scale(node->scaleX(), node->scaleY()) * Mat3::translation(-cx, -cy);
  if (node->type() == NodeType::RenderProxy) {
    const auto* source = static_cast<const RenderProxyNode*>(node)->source();
    for (const Node* ancestor = node; ancestor; ancestor = ancestor->parent())
      if (ancestor == source) return;
    if (source) collect(source, clip, opacity, parentPlane, collected,
        world * Mat3::translation(-source->x(), -source->y()), stack, retainTransparentCustom, true);
    return;
  }
  if (node->type() == NodeType::Rect) {
    const auto* rect = static_cast<const RectNode*>(node);
    const auto& style = rect->style();
    const bool optical = style.material && style.material->primitive == Primitive::Optical
        && ownsOpticalPlane(*style.material, style.materialPlane);
    const bool custom = style.customBackground && style.customBackground->asset;
    if (optical || custom) {
      const bool localContour = style.segmentContour.kind != SegmentContourKind::None;
      // Local planes (lock authentication panel, for example) use only their
      // own client-rendered wallpaper. Contoured planes use the same local
      // fallback because the external protocol currently carries rounded
      // rectangles only. Both still own ordinary optical descendants.
      if (style.materialBackdrop == MaterialBackdrop::Local || localContour) parentPlane = true;
      const bool independent = custom || (style.material && style.material->optical.planeMode == 1.0F);
      if (!localContour && style.materialBackdrop != MaterialBackdrop::Local && (!parentPlane || independent)
          && (opacity > 0 || custom) && style.fill.a > 0 && rect->width() > 0 && rect->height() > 0
          && (!style.paintClip || (style.paintClip->width>0 && style.paintClip->height>0))) {
        ScenePlane p;
        p.group = rect->materialSeed(); p.role = std::string(rect->materialRole());
        p.surface = std::string(rect->materialSurfaceName());
        p.cornerPower = style.cornerPower.value_or(rect->cornerPower());
        p.width = rect->width(); p.height = rect->height(); p.clip = clip;
        if (style.paintClip) {
          const auto& mask=*style.paintClip;
          p.paintClip=SceneRoundedClip{{mask.x,mask.y,mask.width,mask.height},
              std::clamp(mask.radius,0.0F,std::min(mask.width,mask.height)*0.5F),
              mask.cornerPower.value_or(rect->cornerPower())};
        }
        p.transform = {world.m[0], world.m[1], world.m[3], world.m[4], world.m[6], world.m[7]};
        p.radii = {style.radius.tl, style.radius.tr, style.radius.br, style.radius.bl};
        p.insets = {style.logicalInset.left, style.logicalInset.top, style.logicalInset.right, style.logicalInset.bottom};
        p.concaveCorners = (style.corners.tl == CornerShape::Concave ? 1U : 0U)
            | (style.corners.tr == CornerShape::Concave ? 2U : 0U)
            | (style.corners.br == CornerShape::Concave ? 4U : 0U)
            | (style.corners.bl == CornerShape::Concave ? 8U : 0U);
        p.tint = {style.fill.r, style.fill.g, style.fill.b, style.fill.a};
        p.opacity = opacity;
        p.parameters = style.material ? sanitize(*style.material) : Parameters{};
        if (custom) {
          const auto& binding = *style.customBackground;
          p.customEffect = SceneCustomEffect{
              .transportDigest = customEffectTransportDigest(
                  binding.asset->abi,
                  std::span<const std::uint8_t>(
                      reinterpret_cast<const std::uint8_t*>(binding.asset->source.data()),
                      binding.asset->source.size())),
              .abi = binding.asset->abi,
              .sampleRadius = binding.asset->maxSampleRadiusPx,
              .parameters = binding.parameters,
              .staged = true,
          };
          collected.assets.push_back(binding.asset);
        }
        if (validateScene(SceneDescriptor{scene.width,scene.height,{p}})) {
          scene.planes.push_back(std::move(p)); parentPlane = true;
        }
      }
    }
  }
  const auto ancestorClip = clip;
  if (node->clipChildren()) {
    float left, top, right, bottom;
    Node::transformedBounds(node, world, left, top, right, bottom);
    clip = intersect(clip, {left, top, right-left, bottom-top});
  }
  const auto visit=[&](const Node* child) {
    const auto childClip=child->bypassParentPaintClip() ? ancestorClip : clip;
    if (childClip.width>0 && childClip.height>0)
      collect(child,childClip,opacity,parentPlane,collected,world,stack,retainTransparentCustom);
  };
  const auto& children=node->children();
  if (std::is_sorted(children.begin(),children.end(),[](const auto& a,const auto& b){return a->zIndex()<b->zIndex();})) {
    for (const auto& child:children) visit(child.get());
  } else {
    std::vector<const Node*> ordered;ordered.reserve(children.size());
    for (const auto& child:children) ordered.push_back(child.get());
    std::stable_sort(ordered.begin(),ordered.end(),[](const auto* a,const auto* b){return a->zIndex()<b->zIndex();});
    for (const auto* child:ordered) visit(child);
  }
}

CollectedScene collectSceneData(const Node* root, float width, float height,
                                bool retainTransparentCustom = false) {
  CollectedScene collected{.scene = SceneDescriptor{width, height, {}}};
  RenderTraversalGuard::Stack stack;
  collect(root, {0,0,width,height}, 1, false, collected, Mat3::identity(), stack,
          retainTransparentCustom);
  return collected;
}

SceneDescriptor legacyScene(SceneDescriptor scene) {
  std::erase_if(scene.planes, [](const ScenePlane& plane) {
    return plane.customEffect && plane.parameters.primitive != Primitive::Optical;
  });
  for (auto& plane : scene.planes) plane.customEffect.reset();
  return scene;
}

std::vector<CustomEffectTransportDigest> effectSignature(const SceneDescriptor& scene) {
  std::vector<CustomEffectTransportDigest> result;
  for (const auto& plane : scene.planes)
    if (plane.customEffect) result.push_back(plane.customEffect->transportDigest);
  std::ranges::sort(result);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}
SceneRect bounds(const ScenePlane& p) {
  float left = p.transform[4], right = left, top = p.transform[5], bottom = top;
  for (const auto& xy : std::array<noctalia::material::Vec2, 3>{{{p.width, 0}, {0, p.height}, {p.width, p.height}}}) {
    const float x = p.transform[0]*xy.x + p.transform[2]*xy.y + p.transform[4];
    const float y = p.transform[1]*xy.x + p.transform[3]*xy.y + p.transform[5];
    left = std::min(left,x); right=std::max(right,x); top=std::min(top,y); bottom=std::max(bottom,y);
  }
  return intersect({left, top, right-left, bottom-top}, p.clip);
}

SceneDescriptor groupScene(SceneDescriptor scene) {
  // Backgrounds and controls may be sibling subtrees (the bar is one). A
  // contained plane inherits that outer group instead of refracting twice.
  // Use this only for axis-aligned planes; rotated overlap is not containment.
  std::vector<ScenePlane> grouped;
  for (std::size_t i=0; i<scene.planes.size(); ++i) {
    const auto& p=scene.planes[i]; const auto box=bounds(p); bool contained=false;
    if (p.customEffect || p.parameters.optical.planeMode==1.0F) { grouped.push_back(p); continue; }
    for (std::size_t j=0; j<scene.planes.size(); ++j) {
      if (i==j) continue;
      const auto& q=scene.planes[j];
      if (std::abs(q.transform[1])>0.001F || std::abs(q.transform[2])>0.001F) continue;
      const auto outer=bounds(q);
      if (outer.width*outer.height < box.width*box.height ||
          (outer.width*outer.height == box.width*box.height && j>i)) continue;
      if (box.x>=outer.x && box.y>=outer.y && box.x+box.width<=outer.x+outer.width && box.y+box.height<=outer.y+outer.height) {
        contained=true; break;
      }
    }
    if (!contained) grouped.push_back(p);
  }
  scene.planes=std::move(grouped);
  return scene;
}
}
noctalia::material::SceneDescriptor collectMaterialScene(const Node* root, float width, float height) {
  return groupScene(std::move(collectSceneData(root, width, height).scene));
}
noctalia::material::SceneDescriptor collectMaterialLeaseScene(const Node* root, float width, float height) {
  return groupScene(std::move(collectSceneData(root, width, height, true).scene));
}
MaterialSceneSender::~MaterialSceneSender() { reset(); }
void MaterialSceneSender::reset() {
  releaseRetainedEffects();
  if (m_effects != nullptr && m_effectSubscription != 0)
    m_effects->unsubscribe(m_effectSubscription);
  if (m_resource) noctalia_material_surface_v1_destroy(m_resource);
  m_resource=nullptr; m_effects=nullptr; m_effectSubscription=0; m_requestRedraw={};
  m_effectManagerGeneration=0;
  m_lastPayload.clear(); m_effectSignature.clear();
  m_pendingStagedActivePayload.clear(); m_appliedStagedActivePayload.clear();
  m_pendingStagedWirePayload.clear(); m_appliedStagedWirePayload.clear();
  m_pendingStagedSerial=0; m_pendingActiveSerial=0;
  m_leaseRequestSerial=0;
  m_appliedStagedSerial=0;
  m_stagedApplied=false; m_activeApplied=false; m_supportsArmedFrames=false;
  m_supportsLeases=false; m_leaseRejected=false;
  m_framePlans.invalidate(); m_planPayload.clear();
  m_leasePlans.resetBinding();
  m_manager=nullptr; m_surface=nullptr;
  ++m_geometryGeneration; ++m_targetGeneration;
}

void MaterialSceneSender::reconnectEffects(
    CustomEffectTransportRegistry* effects, std::function<void()> requestRedraw) {
  if (m_effects != effects) {
    releaseRetainedEffects();
    if (m_effects != nullptr && m_effectSubscription != 0) m_effects->unsubscribe(m_effectSubscription);
    m_effects = effects;
    m_effectSubscription = 0;
    m_stagedApplied = false;
    m_activeApplied = false;
    m_framePlans.invalidate();
    m_leasePlans.invalidate();
    m_lastPayload.clear();
    m_pendingStagedActivePayload.clear();
    m_appliedStagedActivePayload.clear();
    m_pendingStagedWirePayload.clear();
    m_appliedStagedWirePayload.clear();
    m_appliedStagedSerial = 0;
    m_leaseRejected = false;
  }
  m_requestRedraw = std::move(requestRedraw);
  if (m_effects != nullptr && m_effectSubscription == 0) {
    m_effectSubscription = m_effects->subscribe([this]() {
      // accepted/ready/rejected and manager reset all change the resource set
      // against which the staged descriptor was acknowledged. Conservatively
      // restage before another active descriptor can be armed.
      m_pendingStagedSerial = 0;
      m_pendingActiveSerial = 0;
      m_stagedApplied = false;
      m_activeApplied = false;
      m_framePlans.invalidate();
      m_leasePlans.invalidate();
      m_lastPayload.clear();
      m_pendingStagedActivePayload.clear();
      m_appliedStagedActivePayload.clear();
      m_pendingStagedWirePayload.clear();
      m_appliedStagedWirePayload.clear();
      m_appliedStagedSerial = 0;
      m_leaseRejected = false;
      if (m_requestRedraw) m_requestRedraw();
    });
  }
}

void MaterialSceneSender::releaseRetainedEffects() {
  if (m_effects != nullptr)
    for (const auto& digest : m_retainedEffects) m_effects->release(digest);
  m_retainedEffects.clear();
}

void MaterialSceneSender::syncRetainedEffects(
    std::span<const std::shared_ptr<const CustomEffectAsset>> assets) {
  if (m_effects == nullptr) {
    m_retainedEffects.clear();
    m_effectManagerGeneration = 0;
    return;
  }
  if (m_effectManagerGeneration != m_effects->managerGeneration()) {
    // reset() already destroyed the old entries, so no release is required.
    m_retainedEffects.clear();
    m_effectManagerGeneration = m_effects->managerGeneration();
  }
  std::map<CustomEffectTransportDigest, std::shared_ptr<const CustomEffectAsset>> desired;
  for (const auto& asset : assets) {
    if (!asset) continue;
    desired.emplace(customEffectTransportDigest(
        asset->abi, std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(asset->source.data()), asset->source.size())), asset);
  }
  std::erase_if(m_retainedEffects, [&](const auto& digest) {
    if (desired.contains(digest)) return false;
    m_effects->release(digest);
    return true;
  });
  for (const auto& [digest, asset] : desired) {
    if (std::ranges::find(m_retainedEffects, digest) != m_retainedEffects.end()) continue;
    if (m_effects->retain(asset)) m_retainedEffects.push_back(digest);
  }
  std::ranges::sort(m_retainedEffects);
}

void MaterialSceneSender::handleApplied(std::uint32_t serial) {
  if (m_framePlans.applied(serial)) {
    if (m_pendingActiveSerial == serial) m_pendingActiveSerial = 0;
    m_activeApplied = true;
    return;
  }
  if (serial == m_pendingStagedSerial) {
    m_pendingStagedSerial = 0;
    m_stagedApplied = true;
    m_appliedStagedSerial = serial;
    m_appliedStagedActivePayload = std::move(m_pendingStagedActivePayload);
    m_appliedStagedWirePayload = std::move(m_pendingStagedWirePayload);
    // v3 intentionally retains the staged descriptor and native body. Only a
    // v4 peer needs another frame to request the active descriptor and arm it.
    if (m_supportsArmedFrames) {
      m_lastPayload.clear();
      if (m_requestRedraw) m_requestRedraw();
    }
  } else if (serial == m_pendingActiveSerial) {
    m_pendingActiveSerial = 0;
    m_activeApplied = true;
  }
}

void MaterialSceneSender::handleRejected(std::uint32_t serial) {
  if (serial != m_pendingStagedSerial && serial != m_pendingActiveSerial) return;
  if (m_supportsLeases && serial == m_pendingActiveSerial) m_leasePlans.invalidate();
  m_pendingStagedSerial = 0;
  m_pendingActiveSerial = 0;
  m_stagedApplied = false;
  m_activeApplied = false;
  m_framePlans.invalidate();
  m_lastPayload.clear();
  m_pendingStagedActivePayload.clear();
  m_appliedStagedActivePayload.clear();
  m_pendingStagedWirePayload.clear();
  m_appliedStagedWirePayload.clear();
  m_appliedStagedSerial = 0;
  if (m_requestRedraw) m_requestRedraw();
}

void MaterialSceneSender::handleArmed(std::uint32_t serial) {
  if (!m_framePlans.armed(serial)) return;
  if (m_requestRedraw) m_requestRedraw();
}

void MaterialSceneSender::handleLeaseGranted(std::uint32_t requestSerial, std::uint32_t token) {
  if (!m_leasePlans.granted(requestSerial, token)) {
    if (m_leasePlans.phase() == MaterialLeasePlanState::Phase::Requested
        && requestSerial == m_leasePlans.requestSerial()) {
      (void)m_leasePlans.rejected(requestSerial);
      m_leaseRejected = true;
      if (m_requestRedraw) m_requestRedraw();
    }
    return;
  }
  if (m_requestRedraw) m_requestRedraw();
}

void MaterialSceneSender::handleLeaseRejected(std::uint32_t requestSerial) {
  if (!m_leasePlans.rejected(requestSerial)) return;
  m_leaseRejected = true;
  if (m_requestRedraw) m_requestRedraw();
}

void MaterialSceneSender::handleLeaseArmed(std::uint32_t sceneSerial, std::uint32_t token) {
  if (!m_leasePlans.armed(sceneSerial, token)) return;
  if (m_requestRedraw) m_requestRedraw();
}

void MaterialSceneSender::handleLeaseRevoked(std::uint32_t token) {
  if (!m_leasePlans.revoked(token)) return;
  m_activeApplied = false;
  if (m_requestRedraw) m_requestRedraw();
}

void MaterialSceneSender::handleLeaseReleased(std::uint32_t token, std::uint32_t sceneSerial) {
  if (!m_leasePlans.released(token, sceneSerial)) return;
  m_stagedApplied = false;
  m_appliedStagedSerial = 0;
  m_pendingActiveSerial = 0;
  m_appliedStagedActivePayload.clear();
  m_appliedStagedWirePayload.clear();
  m_lastPayload.clear();
  if (m_requestRedraw) m_requestRedraw();
}

std::uint32_t MaterialSceneSender::sendPayload(
    noctalia_material_manager_v1* manager, wl_surface* surface,
    std::span<const std::uint8_t> payload) {
  if (payload.empty() || payload.size() > kMaxSceneBytes) return 0;
  if (!m_resource) {
    m_resource=noctalia_material_manager_v1_get_surface(manager,surface);
    if (!m_resource) return 0;
    static const noctalia_material_surface_v1_listener listener{
      .applied=[](void* data, noctalia_material_surface_v1*, std::uint32_t serial) {
        static_cast<MaterialSceneSender*>(data)->handleApplied(serial);
      },
      .rejected=[](void* data, noctalia_material_surface_v1*, std::uint32_t serial, std::uint32_t) {
        static_cast<MaterialSceneSender*>(data)->handleRejected(serial);
      },
      .armed=[](void* data, noctalia_material_surface_v1*, std::uint32_t serial) {
        static_cast<MaterialSceneSender*>(data)->handleArmed(serial);
      },
      .lease_granted=[](void* data, noctalia_material_surface_v1*, std::uint32_t requestSerial,
                        std::uint32_t token) {
        static_cast<MaterialSceneSender*>(data)->handleLeaseGranted(requestSerial, token);
      },
      .lease_rejected=[](void* data, noctalia_material_surface_v1*, std::uint32_t requestSerial,
                         std::uint32_t) {
        static_cast<MaterialSceneSender*>(data)->handleLeaseRejected(requestSerial);
      },
      .lease_armed=[](void* data, noctalia_material_surface_v1*, std::uint32_t sceneSerial,
                      std::uint32_t token) {
        static_cast<MaterialSceneSender*>(data)->handleLeaseArmed(sceneSerial, token);
      },
      .lease_revoked=[](void* data, noctalia_material_surface_v1*, std::uint32_t token,
                        std::uint32_t) {
        static_cast<MaterialSceneSender*>(data)->handleLeaseRevoked(token);
      },
      .lease_released=[](void* data, noctalia_material_surface_v1*, std::uint32_t token,
                         std::uint32_t sceneSerial) {
        static_cast<MaterialSceneSender*>(data)->handleLeaseReleased(token, sceneSerial);
      },
    };
    noctalia_material_surface_v1_add_listener(m_resource,&listener,this);
  }
  const bool chunks = noctalia_material_surface_v1_get_version(m_resource) >= kSceneTransportVersion;
  if (!chunks && payload.size() > kMaxInlineSceneBytes) return 0;
  const auto serial = ++m_serial;
  for (std::size_t offset = 0; offset < payload.size();) {
    const auto length = chunks ? std::min(kSceneChunkBytes, payload.size()-offset) : payload.size();
    if (length > kMaxInlineSceneBytes) return 0;
    wl_array array{length, 0, const_cast<std::uint8_t*>(payload.data()+offset)};
    if (chunks)
      noctalia_material_surface_v1_set_scene_chunk(m_resource, serial, payload.size(), offset, &array);
    else noctalia_material_surface_v1_set_scene(m_resource, serial, &array);
    offset += length;
  }
  return serial;
}

MaterialSceneSender::FramePlan MaterialSceneSender::prepare(
    noctalia_material_manager_v1* manager, CustomEffectTransportRegistry* effects,
    wl_surface* surface, const Node* root, float width, float height,
    std::function<void()> requestRedraw) {
  if (manager == nullptr || surface == nullptr
      || noctalia_material_manager_v1_get_version(manager) < kExactMaterialFrameArmVersion)
    return {};
  const auto protocolVersion = noctalia_material_manager_v1_get_version(manager);
  m_manager = manager;
  m_surface = surface;
  m_supportsArmedFrames = true;
  m_supportsLeases = protocolVersion >= kCustomEffectLeaseVersion;
  reconnectEffects(effects, std::move(requestRedraw));
  auto collected = collectSceneData(root, width, height, m_supportsLeases);
  collected.scene = groupScene(std::move(collected.scene));
  syncRetainedEffects(collected.assets);
  const auto signature = effectSignature(collected.scene);
  if (signature != m_effectSignature) {
    if (m_supportsLeases) m_leasePlans.invalidate();
    m_effectSignature = signature;
    ++m_targetGeneration;
    m_stagedApplied = false;
    m_activeApplied = false;
    m_framePlans.invalidate();
    m_pendingStagedActivePayload.clear();
    m_appliedStagedActivePayload.clear();
    m_pendingStagedWirePayload.clear();
    m_appliedStagedWirePayload.clear();
    m_appliedStagedSerial = 0;
    m_leaseRejected = false;
  }
  bool ready = effects != nullptr && !signature.empty();
  if (ready) {
    for (const auto& asset : collected.assets) {
      const auto digest = effects->ensure(asset);
      if (effects->state(digest) != CustomEffectTransportRegistry::State::Ready) ready = false;
    }
  }

  if (m_supportsLeases) {
    const auto resourceGeneration = effects != nullptr ? effects->generation() : 0;
    m_leasePlans.setGenerations(m_targetGeneration, resourceGeneration);
    auto leaseSignature = customEffectLeaseSignature(collected.scene);

    // A resource failure or identity change cannot leave an omitted native
    // body. Restore a token-zero descriptor immediately before the same
    // native-buffer commit, then wait for the exact release acknowledgement.
    if (m_leasePlans.phase() != MaterialLeasePlanState::Phase::Idle
        && (!ready || !m_leasePlans.compatible(
            leaseSignature, m_targetGeneration, resourceGeneration)))
      m_leasePlans.invalidate();

    if (m_leasePlans.phase() == MaterialLeasePlanState::Phase::Restoring) {
      collected.scene.leaseToken = 0;
      for (auto& plane : collected.scene.planes)
        if (plane.customEffect) plane.customEffect->staged = true;
      auto payload = encodeScene(collected.scene, kLeasedSceneVersion);
      MaterialLeaseFrameKey key{
          .payload = payload,
          .signature = std::move(leaseSignature),
          .targetGeneration = m_targetGeneration,
          .resourceGeneration = resourceGeneration,
      };
      auto restore = m_leasePlans.prepareRestore(std::move(key));
      if (payload.empty() || !restore) return {.mode = FrameMode::Native, .sceneManaged = true};
      return {.mode = FrameMode::Native, .lease = std::move(restore),
              .precommitPayload = std::move(payload), .sceneManaged = true};
    }

    if (m_leaseRejected)
      return {.mode = FrameMode::Native, .sceneManaged = true};

    if (!ready || leaseSignature.empty()) return {};

    auto activeScene = collected.scene;
    activeScene.leaseToken = m_leasePlans.token();
    for (auto& plane : activeScene.planes)
      if (plane.customEffect) plane.customEffect->staged = false;
    auto activePayload = encodeScene(activeScene, kLeasedSceneVersion);
    MaterialLeaseFrameKey key{
        .payload = activePayload,
        .signature = leaseSignature,
        .targetGeneration = m_targetGeneration,
        .resourceGeneration = resourceGeneration,
    };
    for (const auto& plane : activeScene.planes)
      if (plane.customEffect) key.omittedGroups.push_back(plane.group);

    switch (m_leasePlans.phase()) {
    case MaterialLeasePlanState::Phase::Idle: {
      // Request only against the exact currently applied token-zero v7 scene.
      auto stagedScene = activeScene;
      stagedScene.leaseToken = 0;
      for (auto& plane : stagedScene.planes)
        if (plane.customEffect) plane.customEffect->staged = true;
      const auto stagedPayload = encodeScene(stagedScene, kLeasedSceneVersion);
      if (!m_stagedApplied || m_pendingStagedSerial != 0 || m_appliedStagedSerial == 0
          || stagedPayload != m_appliedStagedWirePayload)
        return {};
      const auto requestSerial = ++m_leaseRequestSerial;
      if (requestSerial == 0) return {.mode = FrameMode::Native, .sceneManaged = true};
      if (!m_leasePlans.requested(requestSerial, m_appliedStagedSerial, leaseSignature)) return {};
      noctalia_material_surface_v1_request_lease(m_resource, requestSerial, m_appliedStagedSerial);
      return {.mode = FrameMode::Native, .sceneManaged = true};
    }
    case MaterialLeasePlanState::Phase::Requested:
      return {.mode = FrameMode::Native, .sceneManaged = true};
    case MaterialLeasePlanState::Phase::Granted:
    case MaterialLeasePlanState::Phase::InitialRequested: {
      if (activePayload.empty()) return {.mode = FrameMode::Native, .sceneManaged = true};
      if (m_leasePlans.initialRequestedMatches(key))
        return {.mode = FrameMode::Defer, .sceneManaged = true};
      const auto serial = sendPayload(manager, surface, activePayload);
      if (serial != 0) {
        m_pendingActiveSerial = serial;
        m_leasePlans.initialRequested(serial, std::move(key));
      }
      // Do not commit the pending active scene before lease_armed arrives.
      return {.mode = FrameMode::Defer, .sceneManaged = true};
    }
    case MaterialLeasePlanState::Phase::InitialArmed:
      if (auto prepared = m_leasePlans.prepare(key)) {
        return {.mode = FrameMode::Omit, .omittedGroups = key.omittedGroups,
                .lease = std::move(prepared), .sceneManaged = true};
      }
      if (activePayload.empty()) return {.mode = FrameMode::Native, .sceneManaged = true};
      if (const auto serial = sendPayload(manager, surface, activePayload); serial != 0) {
        m_pendingActiveSerial = serial;
        m_leasePlans.initialRequested(serial, std::move(key));
      }
      return {.mode = FrameMode::Defer, .sceneManaged = true};
    case MaterialLeasePlanState::Phase::Active:
      if (auto prepared = m_leasePlans.prepare(key))
        return {.mode = FrameMode::Omit, .omittedGroups = key.omittedGroups,
                .lease = std::move(prepared), .precommitPayload = std::move(activePayload),
                .sceneManaged = true};
      m_leasePlans.beginRestore();
      if (m_requestRedraw) m_requestRedraw();
      return {.mode = FrameMode::Native, .sceneManaged = true};
    case MaterialLeasePlanState::Phase::Restoring:
      break;
    }
  }

  const auto policy = materialEffectFramePolicy(
      protocolVersion, !signature.empty(), ready, m_stagedApplied);
  if (policy != MaterialEffectFramePolicy::Armed) {
    m_framePlans.invalidate();
    m_activeApplied = false;
    return {};
  }
  for (auto& plane : collected.scene.planes)
    if (plane.customEffect) plane.customEffect->staged = false;
  auto payload = encodeScene(collected.scene, kSceneVersion);
  if (payload.empty()) return {};
  if (payload != m_appliedStagedActivePayload) {
    // Geometry, target parameters and resource references must be staged as the
    // same descriptor before this active form may be armed.
    m_stagedApplied = false;
    m_activeApplied = false;
    m_framePlans.invalidate();
    m_lastPayload.clear();
    return {};
  }
  if (payload != m_planPayload) {
    m_planPayload = payload;
    ++m_geometryGeneration;
    m_framePlans.invalidate();
  }
  MaterialFrameKey key{
      .payload = payload,
      .geometryGeneration = m_geometryGeneration,
      .targetGeneration = m_targetGeneration,
      .resourceGeneration = effects->generation(),
  };
  for (const auto& plane : collected.scene.planes)
    if (plane.customEffect) key.omittedGroups.push_back(plane.group);
  if (const auto prepared = m_framePlans.prepare(key))
    return {.mode = FrameMode::Omit, .omittedGroups = key.omittedGroups, .prepared = prepared};
  if (!m_framePlans.requestedMatches(key)) {
    const auto serial = sendPayload(manager, surface, payload);
    if (serial != 0) {
      m_pendingActiveSerial = serial;
      m_framePlans.requested(serial, std::move(key));
    }
  }
  return {.mode = FrameMode::Defer};
}

bool MaterialSceneSender::commit(const FramePlan& plan) {
  if (plan.prepared)
    return plan.mode == FrameMode::Omit && m_framePlans.committed(*plan.prepared);
  if (!plan.lease) return false;
  if (!m_leasePlans.accepts(*plan.lease)) return false;
  if (plan.lease->kind == PreparedMaterialLeaseFrame::Kind::Restore) {
    const auto serial = sendPayload(m_manager, m_surface, plan.precommitPayload);
    return serial != 0 && m_leasePlans.restoreCommitted(*plan.lease, serial);
  }
  if (plan.mode != FrameMode::Omit) return false;
  if (plan.lease->kind == PreparedMaterialLeaseFrame::Kind::Streaming) {
    const auto serial = sendPayload(m_manager, m_surface, plan.precommitPayload);
    if (serial == 0) return false;
    m_pendingActiveSerial = serial;
    return m_leasePlans.committed(*plan.lease);
  }
  return m_leasePlans.committed(*plan.lease);
}

void MaterialSceneSender::send(
    noctalia_material_manager_v1* manager, CustomEffectTransportRegistry* effects,
    wl_surface* surface, const Node* root, float width, float height,
    std::function<void()> requestRedraw) {
  if (!manager || !surface) return;
  m_supportsArmedFrames =
      noctalia_material_manager_v1_get_version(manager) >= kExactMaterialFrameArmVersion;
  m_supportsLeases =
      noctalia_material_manager_v1_get_version(manager) >= kCustomEffectLeaseVersion;
  m_manager = manager;
  m_surface = surface;
  reconnectEffects(effects, std::move(requestRedraw));
  auto collected = collectSceneData(root, width, height, m_supportsLeases);
  collected.scene = groupScene(std::move(collected.scene));
  syncRetainedEffects(collected.assets);
  const auto signature = effectSignature(collected.scene);
  if (signature != m_effectSignature) {
    m_effectSignature = signature;
    ++m_targetGeneration;
    m_pendingStagedSerial = 0;
    m_pendingActiveSerial = 0;
    m_stagedApplied = false;
    m_activeApplied = false;
    m_framePlans.invalidate();
    m_lastPayload.clear();
    m_pendingStagedActivePayload.clear();
    m_appliedStagedActivePayload.clear();
    m_pendingStagedWirePayload.clear();
    m_appliedStagedWirePayload.clear();
    m_appliedStagedSerial = 0;
    m_leaseRejected = false;
  }

  bool allReady = effects != nullptr
      && noctalia_material_manager_v1_get_version(manager) >= kCustomEffectTransportVersion;
  if (allReady) {
    for (const auto& asset : collected.assets) {
      const auto digest = effects->ensure(asset);
      if (effects->state(digest) != CustomEffectTransportRegistry::State::Ready) allReady = false;
    }
  }
  SceneDescriptor wireScene;
  std::vector<std::uint8_t> stagedActivePayload;
  std::uint32_t wireVersion = kLegacySceneVersion;
  bool staged = false;
  const auto policy = materialEffectFramePolicy(
      noctalia_material_manager_v1_get_version(manager), !signature.empty(), allReady, m_stagedApplied);
  if (policy != MaterialEffectFramePolicy::Native) {
    if (policy == MaterialEffectFramePolicy::Armed) return;
    wireScene = std::move(collected.scene);
    staged = true;
    auto activeScene = wireScene;
    for (auto& plane : activeScene.planes)
      if (plane.customEffect) plane.customEffect->staged = false;
    const auto customWireVersion = m_supportsLeases ? kLeasedSceneVersion : kSceneVersion;
    stagedActivePayload = encodeScene(activeScene, customWireVersion);
    for (auto& plane : wireScene.planes)
      if (plane.customEffect) plane.customEffect->staged = staged;
    wireVersion = customWireVersion;
  } else {
    wireScene = legacyScene(std::move(collected.scene));
  }
  auto payload=encodeScene(wireScene, wireVersion);
  if (payload.empty() || payload==m_lastPayload) return;
  // Older compositors cannot receive a large scene. Clear stale optics safely.
  const bool chunks = noctalia_material_manager_v1_get_version(manager) >= kSceneTransportVersion;
  auto wire = (!chunks && payload.size() > kMaxInlineSceneBytes)
      ? encodeScene(SceneDescriptor{.width=width, .height=height}) : payload;
  if (wire.empty() || wire.size() > kMaxSceneBytes) return;
  const auto serial = sendPayload(manager, surface, wire);
  if (serial == 0) return;
  if (wireVersion == kSceneVersion || wireVersion == kLeasedSceneVersion) {
    if (staged) {
      m_pendingStagedSerial = serial;
      m_pendingStagedActivePayload = std::move(stagedActivePayload);
      m_pendingStagedWirePayload = payload;
    }
    else m_pendingActiveSerial = serial;
  }
  m_lastPayload=std::move(payload);
}
