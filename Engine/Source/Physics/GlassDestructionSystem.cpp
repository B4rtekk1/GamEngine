#include "Engine/Physics/GlassDestructionSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/ECS/Components/MeshRendererComponent.h"
#include "Engine/ECS/Components/RigidbodyRuntime.h"
#include <algorithm>
#include <cmath>
#include <random>
#include <limits>
#include <stdexcept>
#include <map>
#include <array>
#include <glm/gtc/constants.hpp>
#include <chrono>
#include <mutex>
#include <bit>
#include "Engine/Core/Profiler.h"

namespace Engine {
    namespace {
        constexpr float Epsilon = 1.0e-5F;
        bool finite(glm::vec3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
        void validate(const GlassDestructionComponent &glass) {
            if (!std::isfinite(glass.size.x) || !std::isfinite(glass.size.y) ||
                glass.size.x < 0.01F || glass.size.y < 0.01F ||
                !std::isfinite(glass.thickness) || glass.thickness < 0.001F ||
                !std::isfinite(glass.density) || glass.density <= 0.0F ||
                !std::isfinite(glass.fractureThreshold) || glass.fractureThreshold <= 0.0F ||
                !std::isfinite(glass.fragmentLifetime) || glass.fragmentLifetime <= 0.0F ||
                static_cast<unsigned>(glass.type) > 3 || static_cast<unsigned>(glass.supportMode) > 2 ||
                glass.supportEdges > 15 || glass.layers == 0 || glass.layers > 16 ||
                !std::isfinite(glass.bondStrength) || glass.bondStrength <= 0.0F ||
                !std::isfinite(glass.crackWidth) || glass.crackWidth <= 0.0F ||
                glass.maxFragments < 4 || glass.maxFragments > 128 || glass.maxDynamicFragments > 128)
                throw std::invalid_argument("Invalid rectangular glass destruction settings");
        }

        bool enableLegacyPane(Registry &registry, Entity entity) {
            const auto &renderer = registry.get<MeshRendererComponent>(entity);
            if (renderer.material.pbr.transmission <= 0.0F || !renderer.mesh ||
                !registry.has<ColliderComponent>(entity)) return false;
            const auto &collider = registry.get<ColliderComponent>(entity);
            const auto *box = std::get_if<BoxCollider>(&collider.shape);
            if (!box || collider.isTrigger || collider.offset.length() > Epsilon) return false;
            if (registry.has<RigidbodyComponent>(entity) &&
                registry.get<RigidbodyComponent>(entity).type != RigidbodyType::Static) return false;

            // Legacy Glass presets are centered cube meshes flattened along Z.
            // Require the mesh bounds to match the box, so unrelated meshes with
            // a glass material cannot silently become rectangular panes.
            const AABB bounds = renderer.mesh.resource()->bounds;
            const auto center = (bounds.min.native() + bounds.max.native()) * 0.5F;
            const auto size = bounds.max.native() - bounds.min.native();
            if (!finite(size) || glm::length(center) > Epsilon ||
                glm::length(size * 0.5F - box->halfExtents.native()) > Epsilon ||
                size.x < 0.01F || size.y < 0.01F || size.z < 0.001F) return false;
            const auto source = renderer.mesh.source();
            if (!source || source->vertices.empty() || source->indices.empty()) return false;
            const auto half = size * 0.5F;
            if (std::ranges::any_of(source->vertices, [&](const Vertex &vertex) {
                return glm::length(glm::abs(vertex.position.native()) - half) > Epsilon;
            })) return false;
            const Transform world = TransformSystem::worldTransform(registry, entity);
            const auto worldSize = size * world.scale.native();
            if (!finite(worldSize) || worldSize.x <= 0.0F || worldSize.y <= 0.0F || worldSize.z <= 0.0F ||
                worldSize.z > 0.05F * std::min(worldSize.x, worldSize.y)) return false;
            GlassDestructionComponent settings;
            settings.size = glm::vec2{size};
            // Thickness is local geometry, not the world-space PBR value. The
            // old preset's Z scale (e.g. 0.01) already supplies physical thickness.
            settings.thickness = size.z;
            registry.add<GlassDestructionComponent>(entity, std::move(settings));
            return true;
        }

        std::vector<glm::vec2> clip(const std::vector<glm::vec2> &polygon, glm::vec2 normal, float offset) {
            std::vector<glm::vec2> result;
            if (polygon.empty()) return result;
            glm::vec2 previous = polygon.back();
            float previousDistance = glm::dot(previous, normal) - offset;
            for (glm::vec2 point : polygon) {
                const float distance = glm::dot(point, normal) -
                    offset;
                if ((distance <= 0.0F) != (previousDistance <= 0.0F))
                    result.push_back(previous + (point - previous) * (previousDistance / (previousDistance - distance)));
                if (distance <= 0.0F) result.push_back(point);
                previous = point;
                previousDistance = distance;
            }
            return result;
        }

        float cross2(glm::vec2 a, glm::vec2 b) { return a.x*b.y-a.y*b.x; }

        bool containsPoint(const std::vector<glm::vec2> &polygon, glm::vec2 point) {
            bool inside = false;
            for (std::size_t i=0, j=polygon.size()-1; i<polygon.size(); j=i++) {
                const auto a=polygon[j], b=polygon[i], edge=b-a;
                const float length2=glm::dot(edge,edge);
                if (length2>0 && glm::distance(point, a+edge*std::clamp(glm::dot(point-a,edge)/length2,0.0F,1.0F))<Epsilon)
                    return true;
                if ((a.y>point.y)!=(b.y>point.y) && point.x<(b.x-a.x)*(point.y-a.y)/(b.y-a.y)+a.x)
                    inside=!inside;
            }
            return inside;
        }

        // Ear clipping handles concave crack boundaries without filling notches.
        std::vector<std::array<unsigned,3>> triangulate(const std::vector<glm::vec2> &polygon) {
            std::vector<unsigned> remaining;
            for (unsigned i=0; i<polygon.size(); ++i) remaining.push_back(i);
            std::vector<std::array<unsigned,3>> triangles;
            while (remaining.size()>3) {
                bool found=false;
                for (std::size_t i=0; i<remaining.size(); ++i) {
                    const auto a=remaining[(i+remaining.size()-1)%remaining.size()], b=remaining[i],
                        c=remaining[(i+1)%remaining.size()];
                    const auto ab=polygon[b]-polygon[a], ac=polygon[c]-polygon[a];
                    const float determinant=cross2(ab,ac);
                    if (determinant<=0) continue;
                    bool occupied=false;
                    for (auto index:remaining) {
                        if (index==a || index==b || index==c) continue;
                        
                        const auto q=polygon[index];
                        const float tolerance=determinant*1.0e-6F;
                        if (cross2(ab,q-polygon[a])>=-tolerance &&
                            cross2(polygon[c]-polygon[b],q-polygon[b])>=-tolerance &&
                            cross2(-ac,q-polygon[c])>=-tolerance) { occupied=true; break; }
                    }
                    if (occupied) continue;
                    triangles.push_back({a,b,c});
                    remaining.erase(remaining.begin()+static_cast<std::ptrdiff_t>(i));
                    found=true; break;
                }
                if (!found) {
                    // Matching T junctions contain exactly collinear vertices.
                    auto flat=remaining.end();
                    for (auto it=remaining.begin(); it!=remaining.end(); ++it) {
                        const auto i=static_cast<std::size_t>(it-remaining.begin());
                        const auto a=polygon[remaining[(i+remaining.size()-1)%remaining.size()]], b=polygon[*it],
                            c=polygon[remaining[(i+1)%remaining.size()]];
                        if (std::abs(cross2(b-a,c-b))<=1.0e-10F && glm::dot(b-a,c-b)>=0) {flat=it; break;}
                    }
                    if (flat==remaining.end()) throw std::runtime_error("Invalid glass crack polygon");
                    remaining.erase(flat);
                }
            }
            if (remaining.size()==3 && cross2(polygon[remaining[1]]-polygon[remaining[0]],
                polygon[remaining[2]]-polygon[remaining[0]])>0)
                triangles.push_back({remaining[0],remaining[1],remaining[2]});
            return triangles;
        }

        std::vector<std::vector<glm::vec2>> convexPartition(const std::vector<glm::vec2> &polygon) {
            std::vector<std::vector<glm::vec2>> parts;
            for (auto triangle:triangulate(polygon))
                parts.push_back({polygon[triangle[0]],polygon[triangle[1]],polygon[triangle[2]]});
            // Merge adjacent ears wherever the union remains convex. This keeps
            // exact notches while avoiding one PhysX shape per tiny triangle.
            for (std::size_t i=0; i<parts.size(); ++i) {
                for (std::size_t j=i+1; j<parts.size();) {
                    bool merged=false;
                    for (std::size_t a=0; !merged && a<parts[i].size(); ++a) {
                        for (std::size_t b=0; !merged && b<parts[j].size(); ++b) {
                            if (parts[i][a]!=parts[j][(b+1)%parts[j].size()] ||
                                parts[i][(a+1)%parts[i].size()]!=parts[j][b]) continue;
                            std::vector<glm::vec2> candidate;
                            for (std::size_t k=1; k<=parts[i].size(); ++k)
                                candidate.push_back(parts[i][(a+k)%parts[i].size()]);
                            for (std::size_t k=2; k<parts[j].size(); ++k)
                                candidate.push_back(parts[j][(b+k)%parts[j].size()]);
                            bool convex=true;
                            for (std::size_t k=0; k<candidate.size(); ++k) {
                                const auto x=candidate[k], y=candidate[(k+1)%candidate.size()],
                                    z=candidate[(k+2)%candidate.size()];
                                convex &= cross2(y-x,z-y)>=0;
                            }
                            if (!convex) continue;
                            parts[i]=std::move(candidate);
                            parts.erase(parts.begin()+static_cast<std::ptrdiff_t>(j));
                            merged=true;
                        }
                    }
                    if (merged) j=i+1; else ++j;
                }
            }
            return parts;
        }

        bool simplePolygon(const std::vector<glm::vec2> &polygon) {
            // A half-plane cut through a concavity can create disconnected
            // islands. Reject such a cut rather than bridging empty material.
            for (std::size_t i=0; i<polygon.size(); ++i) {
                const auto a=polygon[i], b=polygon[(i+1)%polygon.size()];
                for (std::size_t j=i+2; j<polygon.size(); ++j) {
                    if ((j+1)%polygon.size()==i) continue;
                    const auto c=polygon[j], d=polygon[(j+1)%polygon.size()];
                    if (cross2(b-a,c-a)*cross2(b-a,d-a)<0 &&
                        cross2(d-c,a-c)*cross2(d-c,b-c)<0) return false;
                }
            }
            return true;
        }

        bool supportedEdge(const GlassDestructionComponent &glass, glm::vec2 a, glm::vec2 b) {
            const auto half = glass.size * 0.5F;
            const unsigned mask = glass.supportMode == GlassSupportMode::Bottom ? 1U :
                glass.supportMode == GlassSupportMode::Frame ? 15U : glass.supportEdges;
            const float tolerance = std::max(glass.size.x, glass.size.y) * Epsilon;
            return ((mask & 1U) && std::abs(a.y + half.y) < tolerance && std::abs(b.y + half.y) < tolerance) ||
                ((mask & 2U) && std::abs(a.x - half.x) < tolerance && std::abs(b.x - half.x) < tolerance) ||
                ((mask & 4U) && std::abs(a.y - half.y) < tolerance && std::abs(b.y - half.y) < tolerance) ||
                ((mask & 8U) && std::abs(a.x + half.x) < tolerance && std::abs(b.x + half.x) < tolerance);
        }

        void appendPrism(Mesh &mesh, const GlassFragment &fragment, float thickness, glm::vec2 origin);

        void curveBoundaries(GlassDestructionComponent &glass);

        void prepare(GlassDestructionComponent &glass, glm::vec2 hit) {
            GE_PROFILE_SCOPE("FractureGenerationCPU");
            const auto generationStart = std::chrono::steady_clock::now();
            // Identical authored patterns share CPU geometry; Scene owns the GPU
            // handle cache so resources cannot leak between Vulkan contexts.
            using TemplateKey = std::array<std::uint32_t, 13>;
            static std::mutex templateMutex;
            static std::map<TemplateKey, std::weak_ptr<const GlassDestructionComponent>> templates;
            const TemplateKey key{std::bit_cast<unsigned>(glass.size.x), std::bit_cast<unsigned>(glass.size.y),
                std::bit_cast<unsigned>(glass.thickness), std::bit_cast<unsigned>(glass.fractureThreshold),
                std::bit_cast<unsigned>(glass.bondStrength), glass.maxFragments, glass.seed, unsigned(glass.type),
                unsigned(glass.supportMode), glass.supportEdges, glass.layers,
                std::bit_cast<unsigned>(hit.x), std::bit_cast<unsigned>(hit.y)};
            const std::lock_guard guard(templateMutex);
            if (const auto entry = templates.find(key); entry != templates.end()) {
                if (const auto pattern = entry->second.lock()) {
                    glass.fragments = pattern->fragments; glass.bonds = pattern->bonds;
                    glass.fractureRenderGeometry = pattern->fractureRenderGeometry;
                    glass.intactRenderGeometry = pattern->intactRenderGeometry;
                    glass.fractureTemplate = pattern;
                    glass.fractureGenerationCpuMs += std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-generationStart).count();
                    return;
                }
            }
            glass.fractureTemplate.reset(); glass.intactRenderGeometry.reset();
            // Radial cracks precede transverse cracks in ordinary plate glass.
            // Keep every leaf convex so collision geometry follows actual holes.
            glass.fractureRenderGeometry.reset();
            glass.fragments.clear();
            glass.bonds.clear();
            std::mt19937 random(glass.seed);
            std::uniform_real_distribution<float> unit(0.0F, 1.0F);
            const glm::vec2 half = glass.size * 0.5F;
            struct Branch { std::vector<glm::vec2> polygon; float angle, curvature; };
            std::vector<Branch> cells{{{{-half.x,-half.y},{half.x,-half.y},
                {half.x,half.y},{-half.x,half.y}}, unit(random)*glm::two_pi<float>(), 0}};
            auto areaOf = [](const std::vector<glm::vec2> &polygon) {
                float area = 0;
                for (std::size_t i=0; i<polygon.size(); ++i) {
                    const auto a=polygon[i], b=polygon[(i+1)%polygon.size()];
                    area += a.x*b.y-a.y*b.x;
                }
                return area*0.5F;
            };
            const float minArea = glass.size.x*glass.size.y / float(glass.maxFragments*64);
            // Reserve one third for new impact nuclei. Primary spokes and
            // shared circumferential cracks consume the rest, rather than
            // unrelated binary cuts which obscure the impact's spiderweb.
            const unsigned initialBudget=glass.type==GlassType::Tempered ? glass.maxFragments :
                std::max(4U,glass.maxFragments*2/3);
            if (glass.type != GlassType::Tempered) {
                const auto rectangle=cells.front().polygon;
                cells.clear();
                const unsigned rays=std::clamp(initialBudget/5,4U,16U);
                const unsigned rings=initialBudget/rays-1;
                const float spacing=glm::two_pi<float>()/float(rays);
                const float phase=unit(random)*spacing;
                const float wavePhase=unit(random)*glm::two_pi<float>();
                std::vector<float> angles;
                for (unsigned i=0; i<rays; ++i)
                    angles.push_back(phase+(float(i)+(unit(random)-0.5F)*0.6F)*spacing);
                const float extent=std::min(glass.size.x,glass.size.y);
                // Logarithmic spacing resolves the crushed core while allowing
                // long pointed shards outside the densely fractured center.
                auto ringPoint=[&](unsigned ray,unsigned ring) {
                    const float angle=angles[ray%rays];
                    const float t=rings>1 ? float(ring)/float(rings-1) : 0;
                    const float radius=extent*0.018F*std::pow(20.0F,t);
                    // Continuous angular variation meets exactly at each spoke;
                    // independent cell offsets would leave disconnected rings.
                    const float variation=1.0F+0.12F*std::sin(angle*3+wavePhase+float(ring)*0.47F)+
                        0.06F*std::sin(angle*5-wavePhase+float(ring)*0.31F);
                    return hit+glm::vec2{std::cos(angle),std::sin(angle)}*radius*variation;
                };
                for (unsigned i=0; i<rays; ++i) {
                    const float a=angles[i], b=i+1<rays ? angles[i+1] : angles[0]+glm::two_pi<float>();
                    const glm::vec2 first{std::sin(a),-std::cos(a)},second{-std::sin(b),std::cos(b)};
                    auto remaining=clip(clip(rectangle,first,glm::dot(hit,first)),second,glm::dot(hit,second));
                    if (areaOf(remaining)<=Epsilon*Epsilon) continue;
                    const float bisector=(a+b)*0.5F;
                    for (unsigned ring=0; ring<rings; ++ring) {
                        const auto start=ringPoint(i,ring),end=ringPoint(i+1,ring),edge=end-start;
                        glm::vec2 normal{edge.y,-edge.x};
                        if (glm::dot(normal,(start+end)*0.5F-hit)<0) normal=-normal;
                        const float offset=glm::dot(start,normal);
                        auto inner=clip(remaining,normal,offset),outer=clip(remaining,-normal,-offset);
                        if (areaOf(inner)<minArea || areaOf(outer)<minArea) continue;
                        cells.push_back({std::move(inner),bisector,0});
                        remaining=std::move(outer);
                    }
                    cells.push_back({std::move(remaining),bisector,0});
                }
            }
            for (unsigned attempt=0; glass.type==GlassType::Tempered && cells.size()<initialBudget && attempt<glass.maxFragments*8; ++attempt) {
                std::size_t selected=0; float best=-1;
                for (std::size_t i=0; i<cells.size(); ++i) {
                    glm::vec2 center{};
                    for (auto point:cells[i].polygon) center+=point;
                    center/=float(cells[i].polygon.size());
                    const float proximity = glass.type == GlassType::Tempered ? 1.0F :
                        std::pow(0.06F+glm::distance(center,hit)/glm::length(glass.size), 1.5F);
                    const float score=areaOf(cells[i].polygon)/proximity*(0.8F+unit(random)*0.4F);
                    if (score>best) {best=score; selected=i;}
                }
                auto branch=cells[selected];
                glm::vec2 center{};
                for (auto point:branch.polygon) center+=point;
                center/=float(branch.polygon.size());
                branch.curvature=branch.curvature*0.85F+(unit(random)-0.5F)*0.3F;
                branch.angle+=branch.curvature;
                if (glass.type != GlassType::Tempered) {
                    const auto radial = center-hit;
                    // Transverse cuts form irregular rings; occasional branching
                    // cuts leave long sharp splinters between the primary rays.
                    branch.angle = std::atan2(radial.y, radial.x) +
                        (unit(random)<0.6F ? glm::half_pi<float>() : 0.0F) +
                        (unit(random)-0.5F)*1.45F;
                }
                const glm::vec2 normal{-std::sin(branch.angle),std::cos(branch.angle)};
                // First cracks start at the actual hit; later branches inherit
                // direction and split at unequal offsets, with denser local chips.
                const auto origin=cells.size()==1 ? hit :
                    glm::mix(center,branch.polygon[unsigned(unit(random)*float(branch.polygon.size()))%branch.polygon.size()],
                        0.12F+unit(random)*0.28F);
                auto left=clip(branch.polygon,normal,glm::dot(origin,normal));
                auto right=clip(branch.polygon,-normal,-glm::dot(origin,normal));
                if (areaOf(left)<minArea || areaOf(right)<minArea) {
                    cells[selected].angle+=0.73F; continue;
                }
                cells[selected]={std::move(left),branch.angle+0.55F+unit(random)*0.6F,branch.curvature};
                cells.push_back({std::move(right),branch.angle-0.55F-unit(random)*0.6F,branch.curvature});
            }
            // Resolve T junctions so both sides of a shared crack use identical
            // segments. This also makes bond edges the visual crack graph.
            std::vector<glm::vec2> nodes;
            const float tolerance=std::max(glass.size.x,glass.size.y)*1.0e-6F;
            for (auto &cell:cells) for (auto &point:cell.polygon) {
                const auto node=std::ranges::find_if(nodes,[&](auto other){return glm::distance(point,other)<tolerance;});
                if (node==nodes.end()) nodes.push_back(point);
                else point=*node;
            }
            for (auto &cell:cells) {
                GlassFragment fragment;
                for (std::size_t i=0; i<cell.polygon.size(); ++i) {
                    const auto a=cell.polygon[i], b=cell.polygon[(i+1)%cell.polygon.size()], edge=b-a;
                    const float length2=glm::dot(edge,edge);
                    if (length2<tolerance*tolerance) continue;
                    fragment.polygon.push_back(a);
                    std::vector<std::pair<float,glm::vec2>> points;
                    for (auto node:nodes) {
                        const float t=glm::dot(node-a,edge)/length2;
                        if (t>0 && t<1 && glm::distance(node,a)>tolerance &&
                            glm::distance(node,b)>tolerance && glm::distance(node,a+edge*t)<tolerance)
                            points.emplace_back(t,node);
                    }
                    std::ranges::sort(points,{},&std::pair<float,glm::vec2>::first);
                    for (const auto &point:points) fragment.polygon.push_back(point.second);
                }
                if (glass.type==GlassType::Armored) fragment.layerDamage.assign(glass.layers,0);
                float twiceArea=0; glm::vec2 centroid{};
                for (std::size_t i=0; i<fragment.polygon.size(); ++i) {
                    const auto a=fragment.polygon[i], b=fragment.polygon[(i+1)%fragment.polygon.size()];
                    const float cross=a.x*b.y-a.y*b.x;
                    twiceArea+=cross; centroid+=(a+b)*cross;
                    fragment.frameSupported|=supportedEdge(glass,a,b);
                }
                if (twiceArea<=Epsilon*Epsilon) continue;
                fragment.area=twiceArea*0.5F; fragment.center=centroid/(3.0F*twiceArea);
                glass.fragments.push_back(std::move(fragment));
            }
            using Key = std::array<std::int64_t, 4>;
            struct Edge { unsigned fragment; glm::vec2 a, b; };
            std::map<Key, Edge> edges;
            const double quantum = double(std::max(glass.size.x, glass.size.y)) * 1.0e-5;
            for (unsigned i = 0; i < glass.fragments.size(); ++i) {
                const auto &polygon = glass.fragments[i].polygon;
                for (std::size_t k = 0; k < polygon.size(); ++k) {
                    const auto a = polygon[k], b = polygon[(k + 1) % polygon.size()];
                    std::array<std::int64_t, 2> qa{std::llround(a.x / quantum), std::llround(a.y / quantum)},
                        qb{std::llround(b.x / quantum), std::llround(b.y / quantum)};
                    if (qb < qa) std::swap(qa, qb);
                    const Key key{qa[0], qa[1], qb[0], qb[1]};
                    auto [edge, inserted] = edges.emplace(key, Edge{i, a, b});
                    if (!inserted) {
                        const float length = glm::distance(a, b);
                        const float reference = std::sqrt(glass.size.x * glass.size.y / float(glass.maxFragments));
                        const float strength = glass.fractureThreshold * glass.bondStrength *
                            (glass.thickness / 0.006F) * length / std::max(reference, 0.001F);
                        glass.bonds.push_back({edge->second.fragment, i, (a + b) * 0.5F, 0.0F,
                            a, b, length, std::max(strength, 1.0e-8F), 0.0F, false});
                    }
                }
            }
            curveBoundaries(glass);
            auto pattern = std::make_shared<GlassDestructionComponent>(glass);
            pattern->fractureTemplate.reset(); pattern->cracks.clear(); pattern->impactCount = 0;
            (void)GlassDestructionSystem::renderMesh(*pattern);
            GlassFragment rectangle;
            rectangle.polygon = {{-half.x,-half.y},{half.x,-half.y},{half.x,half.y},{-half.x,half.y}};
            auto intact = std::make_shared<Mesh>();
            appendPrism(*intact, rectangle, glass.thickness, {});
            intact->recalculateLocalBounds();
            pattern->intactRenderGeometry = intact;
            glass.intactRenderGeometry = intact;
            glass.fragments = pattern->fragments;
            glass.fractureRenderGeometry = pattern->fractureRenderGeometry;
            glass.fractureTemplate = pattern;
            if (templates.size() >= 64) {
                for (auto entry = templates.begin(); entry != templates.end();) {
                    if (entry->second.expired()) entry = templates.erase(entry); else ++entry;
                }
            }
            if (templates.size() < 64 || templates.contains(key)) templates[key] = pattern;
            glass.fractureGenerationCpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-generationStart).count();
        }

        void appendPrism(Mesh &mesh, const GlassFragment &fragment, float thickness, glm::vec2 origin = {}) {
            const auto &polygon = fragment.polygon;
            const float depth = thickness * 0.5F;
            auto vertex = [&](glm::vec3 point, glm::vec3 normal, glm::vec3 tangent) {
                Vertex value{};
                value.position = Vec3{point};
                value.color = {1.0F, 1.0F, 1.0F};
                value.normal = Vec3{normal};
                value.texCoord = Vec2{glm::vec2{point} + origin};
                value.tangent = {tangent.x, tangent.y, tangent.z, 1.0F};
                mesh.vertices.push_back(value);
            };
            const auto start = static_cast<std::uint32_t>(mesh.vertices.size());
            const auto n = static_cast<std::uint32_t>(polygon.size());
            auto point = [&](std::size_t i, float z) { return glm::vec3{polygon[i] - origin, z}; };
            for (unsigned i = 0; i < n; ++i) vertex(point(i, depth), {0,0,1}, {1,0,0});
            for (unsigned i = 0; i < n; ++i) vertex(point(i, -depth), {0,0,-1}, {1,0,0});
            for (const auto &triangle : triangulate(polygon)) {
                mesh.indices.insert(mesh.indices.end(), {start+triangle[0], start+triangle[1], start+triangle[2],
                    start+n+triangle[0], start+n+triangle[2], start+n+triangle[1]});
            }
            for (unsigned i = 0; i < n; ++i) {
                const auto j = (i + 1) % n;
                const auto edge = glm::normalize(glm::vec3{polygon[j] - polygon[i], 0});
                const auto normal = glm::cross(edge, glm::vec3{0,0,1});
                const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
                vertex(point(i, -depth), normal, edge); vertex(point(j, -depth), normal, edge);
                vertex(point(j, depth), normal, edge); vertex(point(i, depth), normal, edge);
                mesh.indices.insert(mesh.indices.end(), {base, base+1, base+2, base, base+2, base+3});
            }
        }

        float polygonArea(const std::vector<glm::vec2> &polygon) {
            float twiceArea = 0;
            for (std::size_t i = 0; i < polygon.size(); ++i) {
                const auto a = polygon[i], b = polygon[(i + 1) % polygon.size()];
                twiceArea += a.x * b.y - a.y * b.x;
            }
            return twiceArea * 0.5F;
        }

        void updateFragmentGeometry(GlassDestructionComponent &glass, GlassFragment &fragment) {
            float twiceArea = 0;
            glm::vec2 centroid{};
            const bool anchored = fragment.frameSupported;
            fragment.frameSupported = false;
            for (std::size_t i = 0; i < fragment.polygon.size(); ++i) {
                const auto a = fragment.polygon[i], b = fragment.polygon[(i + 1) % fragment.polygon.size()];
                const float cross = a.x * b.y - a.y * b.x;
                twiceArea += cross;
                centroid += (a + b) * cross;
                fragment.frameSupported |= anchored && supportedEdge(glass, a, b);
            }
            fragment.area = twiceArea * 0.5F;
            fragment.center = centroid / (3.0F * twiceArea);
            fragment.collisionGeometry.reset();
        }

        void curveBoundaries(GlassDestructionComponent &glass) {
            // Bow each shared segment once, then reuse the exact reversed path
            // in its neighbor. No gaps, overlapping faces or independent noise.
            const auto original=glass.bonds;
            std::vector<std::vector<glm::vec2>> polygons;
            for (const auto &fragment:glass.fragments) polygons.push_back(fragment.polygon);
            glass.bonds.clear();
            for (unsigned index=0; index<original.size(); ++index) {
                const auto &bond=original[index];
                if (glass.fragments[bond.first].polygon.size()>126 ||
                    glass.fragments[bond.second].polygon.size()>126) {
                    glass.bonds.push_back(bond); continue;
                }
                const auto edge=bond.edgeEnd-bond.edgeStart;
                const auto normal=glm::vec2{-edge.y,edge.x}/bond.length;
                float clearance=bond.length*0.09F;
                // The old convex cells provide a safe corridor. This limits
                // bending around slivers, acute junctions and impact nuclei.
                for (auto cell:{bond.first,bond.second}) {
                    const auto &polygon=polygons[cell];
                    for (std::size_t i=0; i<polygon.size(); ++i) {
                        const auto a=polygon[i], d=polygon[(i+1)%polygon.size()]-a;
                        if (glm::length(d)<Epsilon) continue;
                        if (std::abs(cross2(d,bond.center-a))<Epsilon*glm::length(d)) continue;
                        for (float t:{1.0F/3.0F,2.0F/3.0F})
                            clearance=std::min(clearance, std::abs(cross2(d,bond.edgeStart+edge*t-a))/glm::length(d)*0.25F);
                    }
                }
                std::uint32_t hash=glass.seed ^ (index*747796405U+2891336453U);
                hash=(hash^(hash>>16U))*2246822519U; hash^=hash>>13U;
                const float bend=(float(hash&65535U)/65535.0F*2-1)*clearance;
                const float skew=0.65F+float((hash>>16U)&255U)/255.0F*0.7F;
                const std::array<glm::vec2,4> path{bond.edgeStart,
                    bond.edgeStart+edge/3.0F+normal*bend,
                    bond.edgeStart+edge*(2.0F/3.0F)+normal*bend*skew,bond.edgeEnd};
                for (auto cell:{bond.first,bond.second}) {
                    auto &polygon=glass.fragments[cell].polygon;
                    for (std::size_t i=0; i<polygon.size(); ++i) {
                        const auto a=polygon[i], b=polygon[(i+1)%polygon.size()];
                        const bool forward=glm::distance(a,path[0])<Epsilon && glm::distance(b,path[3])<Epsilon;
                        const bool reverse=glm::distance(a,path[3])<Epsilon && glm::distance(b,path[0])<Epsilon;
                        if (!forward && !reverse) continue;
                        const std::array<glm::vec2,2> inner=forward ? std::array{path[1],path[2]} : std::array{path[2],path[1]};
                        polygon.insert(polygon.begin()+static_cast<std::ptrdiff_t>(i+1),inner.begin(),inner.end());
                        break;
                    }
                }
                for (unsigned i=0; i<3; ++i) {
                    auto segment=bond;
                    segment.edgeStart=path[i]; segment.edgeEnd=path[i+1];
                    segment.center=(path[i]+path[i+1])*0.5F;
                    segment.length=glm::distance(path[i],path[i+1]);
                    segment.strength*=segment.length/bond.length;
                    glass.bonds.push_back(segment);
                }
            }
            for (auto &fragment:glass.fragments) updateFragmentGeometry(glass,fragment);
        }

        bool refineImpact(GlassDestructionComponent &glass, glm::vec2 hit, glm::vec2 worldScale,
            float radius) {
            if (glass.type == GlassType::Tempered || glass.fragments.size() >= glass.maxFragments) return false;
            GE_PROFILE_SCOPE("FractureGenerationCPU");
            const auto start = std::chrono::steady_clock::now();
            std::mt19937 random(glass.seed ^ (glass.impactCount * 1664525U));
            std::uniform_real_distribution<float> unit(0, 1);
            const float minArea = glass.size.x * glass.size.y / float(glass.maxFragments * 64);
            bool changed = false;
            // Split only attached material near this shot. Old fragment indices,
            // debris geometry and holes stay valid; new children append at the end.
            for (unsigned attempt = 0; attempt < 12 && glass.fragments.size() < glass.maxFragments; ++attempt) {
                std::size_t selected = glass.fragments.size();
                float best = 0;
                for (std::size_t i = 0; i < glass.fragments.size(); ++i) {
                    const auto &fragment = glass.fragments[i];
                    if (!fragment.attached || fragment.area < minArea * 4) continue;
                    const float distance = glm::length((fragment.center - hit) * worldScale);
                    const float score = fragment.area / (0.01F + distance * distance);
                    if (distance < radius * 2.5F && score > best) { best = score; selected = i; }
                }
                if (selected == glass.fragments.size()) break;
                auto parent = glass.fragments[selected];
                const auto radial = parent.center - hit;
                const float angle = std::atan2(radial.y, radial.x) +
                    (attempt % 3 == 0 ? 0.0F : glm::half_pi<float>()) + (unit(random) - 0.5F) * 1.2F;
                const glm::vec2 normal{-std::sin(angle), std::cos(angle)};
                const auto origin = glm::mix(parent.center, hit, attempt == 0 ? 1.0F : 0.25F + unit(random) * 0.35F);
                unsigned crossings=0;
                int firstSide=0, previousSide=0;
                for (auto point:parent.polygon) {
                    const float distance=glm::dot(point-origin,normal);
                    if (std::abs(distance)<1.0e-7F) continue;
                    const int side=distance>0 ? 1 : -1;
                    if (!firstSide) firstSide=side;
                    if (previousSide && side!=previousSide) ++crossings;
                    previousSide=side;
                }
                if (previousSide!=firstSide) ++crossings;
                if (crossings>2) continue;
                auto left = clip(parent.polygon, normal, glm::dot(origin, normal));
                auto right = clip(parent.polygon, -normal, -glm::dot(origin, normal));
                if (polygonArea(left) < minArea || polygonArea(right) < minArea ||
                    !simplePolygon(left) || !simplePolygon(right)) continue;
                auto child = parent;
                parent.polygon = std::move(left); child.polygon = std::move(right);
                updateFragmentGeometry(glass, parent); updateFragmentGeometry(glass, child);
                glass.fragments[selected] = std::move(parent);
                glass.fragments.push_back(std::move(child));
                changed = true;
            }
            if (!changed) return false;
            const auto oldBonds = glass.bonds;
            // Resolve new T junctions on attached cells only. Detached polygons
            // are immutable because live rigid bodies reference their centroids.
            const float tolerance = std::max(glass.size.x, glass.size.y) * 1.0e-6F;
            std::vector<glm::vec2> nodes;
            for (auto &fragment : glass.fragments) {
                if (!fragment.attached) continue;
                for (auto &point : fragment.polygon) {
                    const auto found = std::ranges::find_if(nodes, [&](auto node) { return glm::distance(node, point) < tolerance; });
                    if (found == nodes.end()) nodes.push_back(point); else point = *found;
                }
            }
            for (auto &fragment : glass.fragments) {
                if (!fragment.attached) continue;
                std::vector<glm::vec2> polygon;
                for (std::size_t i = 0; i < fragment.polygon.size(); ++i) {
                    const auto a = fragment.polygon[i], b = fragment.polygon[(i + 1) % fragment.polygon.size()], edge = b - a;
                    const float length2 = glm::dot(edge, edge);
                    if (length2 < tolerance * tolerance) continue;
                    polygon.push_back(a);
                    std::vector<std::pair<float, glm::vec2>> points;
                    for (auto node : nodes) {
                        const float t = glm::dot(node - a, edge) / length2;
                        if (t > 0 && t < 1 && glm::distance(node, a) > tolerance && glm::distance(node, b) > tolerance &&
                            glm::distance(node, a + edge * t) < tolerance) points.emplace_back(t, node);
                    }
                    std::ranges::sort(points, {}, &std::pair<float, glm::vec2>::first);
                    for (const auto &point : points) polygon.push_back(point.second);
                }
                if (polygon != fragment.polygon) {
                    fragment.polygon = std::move(polygon);
                    updateFragmentGeometry(glass, fragment);
                }
            }
            using Key = std::array<std::int64_t, 4>;
            std::map<Key, unsigned> edges;
            glass.bonds.clear();
            const double quantum = double(std::max(glass.size.x, glass.size.y)) * 1.0e-5;
            const float reference = std::sqrt(glass.size.x * glass.size.y / float(glass.maxFragments));
            for (unsigned i = 0; i < glass.fragments.size(); ++i) {
                if (!glass.fragments[i].attached) continue;
                const auto &polygon = glass.fragments[i].polygon;
                for (std::size_t k = 0; k < polygon.size(); ++k) {
                    const auto a = polygon[k], b = polygon[(k + 1) % polygon.size()];
                    std::array<std::int64_t, 2> qa{std::llround(a.x / quantum), std::llround(a.y / quantum)},
                        qb{std::llround(b.x / quantum), std::llround(b.y / quantum)};
                    if (qb < qa) std::swap(qa, qb);
                    const auto [edge, inserted] = edges.emplace(Key{qa[0], qa[1], qb[0], qb[1]}, i);
                    if (inserted) continue;
                    const float length = glm::distance(a, b);
                    GlassBond bond{edge->second, i, (a + b) * 0.5F, 0, a, b, length,
                        std::max(1.0e-8F, glass.fractureThreshold * glass.bondStrength * glass.thickness / 0.006F * length / reference), 0, false};
                    for (const auto &old : oldBonds) {
                        const auto d = old.edgeEnd - old.edgeStart;
                        const float length2 = glm::dot(d, d);
                        if (length2 < tolerance * tolerance) continue;
                        auto onEdge = [&](glm::vec2 point) {
                            const float t = glm::dot(point - old.edgeStart, d) / length2;
                            return t >= -Epsilon && t <= 1 + Epsilon && glm::distance(point, old.edgeStart + d * t) < tolerance * 4;
                        };
                        if (onEdge(a) && onEdge(b)) {
                            bond.damage = old.damage; bond.broken = old.broken;
                            bond.accumulatedDamage = old.accumulatedDamage * length / old.length;
                            break;
                        }
                    }
                    glass.bonds.push_back(bond);
                }
            }
            glass.fractureTemplate.reset();
            glass.fractureRenderGeometry.reset();
            glass.fractureGenerationCpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            return true;
        }

        std::shared_ptr<Mesh> shardGeometry(const GlassFragment &fragment, float thickness) {
            auto shard = std::make_shared<Mesh>();
            appendPrism(*shard, fragment, thickness, fragment.center);
            shard->recalculateLocalBounds();
            bool convex=true;
            for (std::size_t i=0; i<fragment.polygon.size(); ++i) {
                const auto a=fragment.polygon[i], b=fragment.polygon[(i+1)%fragment.polygon.size()],
                    c=fragment.polygon[(i+2)%fragment.polygon.size()];
                convex &= cross2(b-a,c-b)>=-1.0e-10F;
            }
            if (!convex) for (auto polygon:convexPartition(fragment.polygon)) {
                GlassFragment part;
                part.polygon=std::move(polygon);
                auto convexPart=std::make_shared<Mesh>();
                appendPrism(*convexPart,part,thickness,fragment.center);
                convexPart->recalculateLocalBounds();
                shard->convexCollisionParts.push_back(std::move(convexPart));
            }
            return shard;
        }

        std::shared_ptr<Mesh> attachedMesh(const GlassDestructionComponent &glass) {
            auto mesh = std::make_shared<Mesh>();
            for (const auto &fragment : glass.fragments)
                if (fragment.attached) appendPrism(*mesh, fragment, glass.thickness);
            mesh->recalculateLocalBounds();
            return mesh;
        }
        void detachUnsupported(Scene &scene, Registry &registry, Entity entity, const Transform &world,
            const GlassImpact &impact, glm::vec2 hit, const std::vector<bool> &wasAttached,
            bool forceUnsupported = false) {
            GE_PROFILE_SCOPE("BondSolverCPU");
            const auto solverStart = std::chrono::steady_clock::now();
            const auto simulation = scene.glassSimulationSettings();
            if (!std::isfinite(simulation.minDynamicFragmentArea) || simulation.minDynamicFragmentArea < 0 ||
                !std::isfinite(simulation.kineticEnergyFraction) || simulation.kineticEnergyFraction < 0 || simulation.kineticEnergyFraction > 1 ||
                !std::isfinite(simulation.maxLinearSpeed) || simulation.maxLinearSpeed < 0 ||
                !std::isfinite(simulation.maxAngularSpeed) || simulation.maxAngularSpeed < 0 ||
                !std::isfinite(simulation.targetPhysicsCpuMs) || simulation.targetPhysicsCpuMs <= 0)
                throw std::invalid_argument("Invalid glass simulation budget");

            auto &glass = registry.get<GlassDestructionComponent>(entity);
            const auto matrix = world.matrix().native();
            auto influence = [&](glm::vec2 point) {
                const auto displacement = glm::vec3{matrix * glm::vec4{point - hit, 0.0F, 0.0F}};
                return std::exp(-glm::dot(displacement, displacement) / (2.0F * std::max(impact.radius * impact.radius, 1.0e-12F)));
            };
            std::vector<bool> supported(glass.fragments.size());
            std::vector<std::uint32_t> queue;
            for (std::uint32_t i = 0; i < glass.fragments.size(); ++i)
                if (!forceUnsupported && glass.fragments[i].attached && glass.fragments[i].frameSupported) { supported[i] = true; queue.push_back(i); }
            std::vector<std::vector<std::uint32_t>> neighbors(glass.fragments.size());
            for (const auto &bond : glass.bonds) {
                if (bond.broken || bond.damage >= 1.0F) continue;
                neighbors[bond.first].push_back(bond.second);
                neighbors[bond.second].push_back(bond.first);
            }
            for (std::size_t next = 0; next < queue.size(); ++next)
                for (auto neighbor : neighbors[queue[next]])
                    if (!supported[neighbor] && glass.fragments[neighbor].attached) {
                        supported[neighbor] = true;
                        queue.push_back(neighbor);
                    }
            // Keep the runtime state independent of ECS component storage while creating actors.
            std::vector<std::uint32_t> detached;
            for (std::size_t i = 0; i < glass.fragments.size(); ++i) {
                auto &fragment = glass.fragments[i];
                if (fragment.attached && !supported[i]) fragment.attached = false;
                if (!fragment.attached && wasAttached[i]) detached.push_back(static_cast<std::uint32_t>(i));
            }
            glass.fullyDestroyed = std::ranges::none_of(glass.fragments, [](const auto &fragment) { return fragment.attached; });
            // Keep render slots and geometry stable. PhysX reuses cached convex
            // cell shapes for the remaining static compound and dynamic shards.
            if (!detached.empty()) glass.renderState = GlassRenderState::Fractured;
            if (registry.get<MeshRendererComponent>(entity).mesh.resource()->metadata.sections.size() != glass.fragments.size() + 1)
                registry.modify<MeshRendererComponent>(entity, [&](auto &renderer) {
                    renderer.mesh = scene.glassGeometryHandle(GlassDestructionSystem::renderMesh(glass));
                });
            const GlassDestructionComponent settings{
                .size = glass.size, .thickness = glass.thickness, .density = glass.density,
                .maxDynamicFragments = glass.maxDynamicFragments, .fragmentLifetime = glass.fragmentLifetime};
            if (glass.fullyDestroyed) registry.remove<ColliderComponent>(entity);
            else {
                if (!registry.has<ColliderComponent>(entity)) registry.add<ColliderComponent>(entity);
                registry.modify<ColliderComponent>(entity, [&](auto &collider) { collider.shape = MeshCollider{registry.get<MeshRendererComponent>(entity).mesh.source()}; collider.offset = {}; });
            }
            registry.markChanged<GlassDestructionComponent>(entity);
            std::uint32_t active = 0, paneActive = 0;
            registry.view<GlassDebrisComponent>([&](Entity candidate, const auto &) {
                ++active;
                if (registry.has<GlassFragmentBindingComponent>(candidate) &&
                    registry.get<GlassFragmentBindingComponent>(candidate).paneEntity == entity) ++paneActive;
            });
            // Largest relevant shards take the rigid-body budget before small slivers.
            std::ranges::sort(detached, [&](auto a, auto b) { return
                registry.get<GlassDestructionComponent>(entity).fragments[a].area >
                registry.get<GlassDestructionComponent>(entity).fragments[b].area; });
            const float volumeScale = std::abs(world.scale.x() * world.scale.y() * world.scale.z());
            auto launchWeight = [&](const GlassFragment &fragment) {
                float exposure = influence(fragment.center);
                for (std::size_t i = 0; i < fragment.polygon.size(); ++i) {
                    const auto a = fragment.polygon[i], edge = fragment.polygon[(i + 1) % fragment.polygon.size()] - a;
                    const float length2 = glm::dot(edge, edge);
                    if (length2 <= Epsilon * Epsilon) continue;
                    const auto point = a + edge * std::clamp(glm::dot(hit - a, edge) / length2, 0.0F, 1.0F);
                    exposure = std::max(exposure, influence(point));
                }
                // Released splinters absorb impulse at the nearest surface,
                // including long shards whose centroids lie far from the shot.
                // A small transmitted share clears secondary releases from the
                // pane; normalization below keeps all debris within its budget.
                return 0.015F + 0.985F * exposure;
            };
            float weightSum = 0.0F;
            for (auto index : detached) {
                const auto &fragment = registry.get<GlassDestructionComponent>(entity).fragments[index];
                weightSum += std::max(0.001F, fragment.area * settings.thickness * settings.density * volumeScale) *
                    launchWeight(fragment) * launchWeight(fragment);
            }
            const float kineticBudget = std::max(0.0F, impact.energy *
                std::clamp(simulation.kineticEnergyFraction, 0.0F, 1.0F));
            const float velocityScale = weightSum > Epsilon ? std::sqrt(2.0F * kineticBudget / weightSum) : 0.0F;
            const auto launchSeed = glass.seed ^ (glass.impactCount * 1664525U);
            const auto glassType = glass.type;
            auto launchVelocity = [&](const GlassFragment &fragment, std::uint32_t index) {
                const float directionLength = impact.worldDirection.length();
                if (directionLength <= Epsilon) return glm::vec3{};
                const auto forward = impact.worldDirection.native() / directionLength;
                auto lateral = glm::vec3{matrix * glm::vec4{fragment.center-hit, 0, 0}};
                lateral -= forward * glm::dot(lateral, forward);
                const float distance = glm::length(lateral);
                if (distance > Epsilon) lateral /= distance;
                // Stable asymmetry avoids identical flight paths while preserving
                // the allocated kinetic energy for both rigid and GPU debris.
                std::uint32_t hash = launchSeed ^ (index * 747796405U + 2891336453U);
                hash = (hash ^ (hash >> 16U)) * 2246822519U;
                hash ^= hash >> 13U;
                const float variation = float(hash & 0xffffU) / 65535.0F;
                const float spread = (glassType == GlassType::Tempered ? 0.45F : 0.28F) *
                    (0.65F + variation*0.7F) * std::min(distance / impact.radius, 1.0F);
                const auto direction = glm::normalize(forward + lateral*spread +
                    glm::cross(forward, lateral)*spread*(variation-0.5F)*0.3F);
                const float speed = std::min(simulation.maxLinearSpeed, velocityScale*launchWeight(fragment));
                return direction*speed;
            };
            registry.get<GlassDestructionComponent>(entity).bondSolverCpuMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-solverStart).count();
            GE_PROFILE_SCOPE("DebrisSpawnCPU");
            const auto debrisStart = std::chrono::steady_clock::now();
            std::uint32_t ordinal = 0;
            for (auto fragmentIndex : detached) {
                const auto fragment = registry.get<GlassDestructionComponent>(entity).fragments[fragmentIndex];
                if (paneActive >= settings.maxDynamicFragments || active >= scene.glassDynamicFragmentBudget() ||
                    fragment.area * world.scale.x() * world.scale.y() < simulation.minDynamicFragmentArea ||
                    settings.maxDynamicFragments == 0 || !scene.reserveGlassDynamicFragment()) {
                    if (scene.glassParticleBursts().size() < 1024) {
                        const auto position = glm::vec3{matrix * glm::vec4{fragment.center, 0, 1}};
                        const auto velocity = launchVelocity(fragment, fragmentIndex);
                        scene.queueGlassParticleBurst({position, velocity,
                            std::clamp(std::sqrt(fragment.area * world.scale.x() * world.scale.y()), 0.008F, 0.12F),
                            settings.fragmentLifetime, registry.get<GlassDestructionComponent>(entity).type == GlassType::Tempered ? 4U : 1U,
                            registry.get<GlassDestructionComponent>(entity).seed + fragmentIndex});
                    }
                    continue;
                }
                auto shard = fragment.collisionGeometry;
                if (!shard) {
                    auto source = std::make_shared<Mesh>();
                    appendPrism(*source, fragment, settings.thickness, fragment.center);
                    source->recalculateLocalBounds();
                    shard = source;
                }
                const auto debris = scene.createActor("Glass debris " + std::to_string(entity) + " " + std::to_string(++ordinal));
                const Entity debrisEntity = scene.findEntity(debris.id());
                Transform pose = world;
                pose.position = Vec3{glm::vec3{matrix * glm::vec4{fragment.center, 0.0F, 1.0F}}};
                registry.modify<Transform>(debrisEntity, [&](auto &transform) { transform = pose; });
                const float mass = std::max(0.001F, fragment.area * settings.thickness * settings.density * volumeScale);
                registry.add<ColliderComponent>(debrisEntity, ColliderComponent{.shape = MeshCollider{shard}, .offset = {}, .friction = 0.4F, .restitution = 0.15F});
                registry.add<RigidbodyComponent>(debrisEntity, RigidbodyComponent{.mass = mass});
                RigidbodyState state;
                const auto velocity = launchVelocity(fragment, fragmentIndex);
                // Impulse applied at the nearest point on the shard to the hit.
                glm::vec2 application = hit;
                float nearest = std::numeric_limits<float>::max();
                const bool inside = containsPoint(fragment.polygon, hit);
                for (std::size_t i = 0; i < fragment.polygon.size(); ++i) {
                    const auto a = fragment.polygon[i], b = fragment.polygon[(i+1) % fragment.polygon.size()];
                    const auto edge = b-a, delta = hit-a;
                    const auto point = a + edge * std::clamp(glm::dot(delta, edge) / glm::dot(edge,edge), 0.0F, 1.0F);
                    const float distance = glm::dot(point-hit, point-hit);
                    if (distance < nearest) { nearest = distance; application = point; }
                }
                if (inside) application = hit;
                const glm::vec3 r = glm::vec3{matrix * glm::vec4{application-fragment.center, 0, 0}};
                // Exact planar second moments, plus the extrusion thickness term.
                float xx = 0, yy = 0, xy = 0;
                for (std::size_t i = 0; i < fragment.polygon.size(); ++i) {
                    const auto a = (fragment.polygon[i]-fragment.center) * glm::vec2{world.scale.native()};
                    const auto b = (fragment.polygon[(i+1)%fragment.polygon.size()]-fragment.center) * glm::vec2{world.scale.native()};
                    const float cross = a.x*b.y-a.y*b.x;
                    xx += cross * (a.x*a.x+a.x*b.x+b.x*b.x) / 12.0F;
                    yy += cross * (a.y*a.y+a.y*b.y+b.y*b.y) / 12.0F;
                    xy += cross * (2*a.x*a.y+a.x*b.y+b.x*a.y+2*b.x*b.y) / 24.0F;
                }
                const float area = fragment.area * world.scale.x() * world.scale.y();
                const float depth2 = std::pow(settings.thickness * world.scale.z(), 2) / 12.0F;
                glm::mat3 inertia{0.0F};
                inertia[0][0] = mass * (yy/area + depth2);
                inertia[1][1] = mass * (xx/area + depth2);
                inertia[2][2] = mass * (xx+yy)/area;
                inertia[0][1] = inertia[1][0] = -mass * xy/area;
                glm::mat3 rotation{matrix};
                for (unsigned i=0; i<3; ++i) rotation[i] = glm::normalize(rotation[i]);
                const auto worldInertia = rotation * inertia * glm::transpose(rotation);
                glm::vec3 angular{};
                if (std::abs(glm::determinant(worldInertia)) > 1.0e-15F)
                    angular = glm::inverse(worldInertia) * glm::cross(r, mass * velocity);
                const float angularSpeed = glm::length(angular);
                if (angularSpeed > simulation.maxAngularSpeed && angularSpeed > Epsilon)
                    angular *= std::max(simulation.maxAngularSpeed, 0.0F) / angularSpeed;
                // Rotation shares the same shard energy allocation as translation.
                const float translation = mass * glm::dot(velocity, velocity);
                const float rotationEnergy = glm::dot(angular, worldInertia * angular);
                const float normalization = translation > 0 ? std::sqrt(translation / (translation + rotationEnergy)) : 0;
                state.linearVelocity = Vec3{velocity * normalization};
                state.angularVelocity = Vec3{glm::degrees(angular * normalization)};
                registry.add<RigidbodyState>(debrisEntity, state);
                registry.add<GlassDebrisComponent>(debrisEntity, GlassDebrisComponent{settings.fragmentLifetime});
                registry.add<GlassFragmentBindingComponent>(debrisEntity, GlassFragmentBindingComponent{
                    registry.get<UUIDComponent>(entity).value, fragmentIndex, entity});
                registry.get<GlassDestructionComponent>(entity).fragments[fragmentIndex].debrisEntity = debrisEntity;
                ++active;
                ++paneActive;
            }
            registry.get<GlassDestructionComponent>(entity).debrisSpawnCpuMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-debrisStart).count();
        }

    }

    bool GlassDestructionSystem::edgeExposed(const GlassDestructionComponent &glass,
        unsigned fragmentIndex, unsigned edgeIndex) {
        const auto &fragment=glass.fragments[fragmentIndex];
        if (!fragment.attached) return true;
        const auto a=fragment.polygon[edgeIndex], b=fragment.polygon[(edgeIndex+1)%fragment.polygon.size()];
        const float tolerance=std::max(glass.size.x,glass.size.y)*Epsilon*2;
        for (const auto &bond:glass.bonds) {
            if (bond.first!=fragmentIndex && bond.second!=fragmentIndex) continue;
            if ((glm::distance(a,bond.edgeStart)<tolerance && glm::distance(b,bond.edgeEnd)<tolerance) ||
                (glm::distance(b,bond.edgeStart)<tolerance && glm::distance(a,bond.edgeEnd)<tolerance)) {
                const auto neighbor=bond.first==fragmentIndex ? bond.second : bond.first;
                return bond.broken || !glass.fragments[neighbor].attached;
            }
        }
        // An unmatched boundary borders air (pane frame or an existing hole).
        return true;
    }

    std::vector<GlassCrackSegment> GlassDestructionSystem::crackSegments(
        const GlassDestructionComponent &glass, const GlassCrack &crack) {
        std::vector<GlassCrackSegment> segments;
        const bool primaryPattern=!glass.cracks.empty() &&
            glm::distance(crack.position,glass.cracks.front().position)<Epsilon;
        // Clip graph edges to the impact's energy footprint. Broken bonds are
        // always visible, even if the solver released a long edge outside it.
        for (const auto &bond:glass.bonds) {
            if (!bond.broken && bond.damage <= 0.0F && !primaryPattern) continue;
            auto a=bond.edgeStart, b=bond.edgeEnd;
            if (!bond.broken) {
                const auto d=b-a, offset=a-crack.position;
                const float aa=glm::dot(d,d), bb=glm::dot(offset,d);
                const float discriminant=bb*bb-aa*(glm::dot(offset,offset)-crack.radius*crack.radius);
                if (aa<Epsilon*Epsilon || discriminant<0) continue;
                const float root=std::sqrt(discriminant);
                const float low=std::max(0.0F,(-bb-root)/aa), high=std::min(1.0F,(-bb+root)/aa);
                if (high<=low) continue;
                b=a+d*high; a+=d*low;
            }
            const float coverage=primaryPattern ? std::max(0.65F,bond.damage) : bond.damage;
            segments.push_back({a,b,bond.broken ? 1.0F : crack.intensity*std::clamp(coverage,0.0F,1.0F)});
        }
        // Surface-only tips can die before closing a physical cell. Correlated
        // curvature avoids per-point zigzags; intersections terminate propagation.
        struct Tip { glm::vec2 position, direction; float energy, curvature; unsigned generation; };
        std::mt19937 random(crack.seed);
        std::uniform_real_distribution<float> unit(0,1);
        std::vector<Tip> tips;
        const unsigned rayCount = 7 + crack.seed % 5;
        const float phase = unit(random) * glm::two_pi<float>();
        for (unsigned i=0; i<rayCount; ++i) {
            const float angle=phase + (float(i) + (unit(random)-0.5F)*0.75F) * glm::two_pi<float>() / float(rayCount);
            tips.push_back({crack.position,{std::cos(angle),std::sin(angle)},
                crack.radius*(0.45F+unit(random)*0.55F),0,0});
        }
        const auto half=glass.size*0.5F;
        if (!primaryPattern) {
            const unsigned ringCount=3+unsigned(crack.intensity*4);
            for (unsigned ring=0; ring<ringCount; ++ring) {
                const float radius=crack.radius*0.055F*std::pow(12.0F,float(ring)/float(std::max(1U,ringCount-1)));
                const float ringPhase=unit(random)*glm::two_pi<float>();
                auto ringPoint=[&](unsigned ray) {
                    const auto direction=tips[ray%rayCount].direction;
                    const float angle=std::atan2(direction.y,direction.x);
                    const float wave=radius*(1+0.13F*std::sin(angle*3+ringPhase)+0.06F*std::sin(angle*5-ringPhase));
                    return crack.position+direction*wave;
                };
                for (unsigned ray=0; ray<rayCount; ++ray) {
                    const auto a=ringPoint(ray),b=ringPoint(ray+1);
                    if (std::abs(a.x)>half.x || std::abs(a.y)>half.y ||
                        std::abs(b.x)>half.x || std::abs(b.y)>half.y) continue;
                    if (unit(random)<0.18F) continue; // Arrested, incomplete secondary arcs.
                    const auto mid=(a+b)*0.5F+glm::normalize((a+b)*0.5F-crack.position)*radius*0.06F;
                    segments.push_back({a,mid,crack.intensity*0.65F});
                    segments.push_back({mid,b,crack.intensity*0.65F});
                }
            }
        }
        auto cross=[](glm::vec2 a,glm::vec2 b){return a.x*b.y-a.y*b.x;};
        for (std::size_t i=0; i<tips.size() && i<24; ++i) {
            auto tip=tips[i];
            for (unsigned step=0; tip.energy>0.001F && step<48; ++step) {
                tip.curvature=tip.curvature*0.85F+(unit(random)-0.5F)*0.15F;
                const float angle=tip.curvature*0.35F;
                tip.direction={tip.direction.x*std::cos(angle)-tip.direction.y*std::sin(angle),
                    tip.direction.x*std::sin(angle)+tip.direction.y*std::cos(angle)};
                const float length=std::min(tip.energy,crack.radius*(0.02F+unit(random)*0.025F));
                const auto delta=tip.direction*length;
                float stop=1;
                for (unsigned axis=0; axis<2; ++axis)
                    if (std::abs(delta[axis])>Epsilon) stop=std::min(stop,
                        ((delta[axis]>0 ? half[axis] : -half[axis])-tip.position[axis])/delta[axis]);
                for (const auto &segment:segments) {
                    const auto edge=segment.end-segment.start, offset=segment.start-tip.position;
                    const float determinant=cross(delta,edge);
                    if (std::abs(determinant)<Epsilon*Epsilon) continue;
                    const float t=cross(offset,edge)/determinant, u=cross(offset,delta)/determinant;
                    if (t>0.001F && t<stop && u>=0 && u<=1) stop=t;
                }
                stop=std::clamp(stop,0.0F,1.0F);
                const auto next=tip.position+delta*stop;
                if (glm::distance(next,tip.position)>Epsilon)
                    segments.push_back({tip.position,next,crack.intensity * (tip.generation == 0 ? 0.9F : 0.45F)});
                tip.position=next; tip.energy=(tip.energy-length)*0.97F;
                if (stop<1) break;
                if (step>2 && tip.generation<2 && tips.size()<24 && unit(random)<0.07F) {
                    const float turn=(unit(random)<0.5F ? -1.0F : 1.0F)*(0.45F+unit(random)*0.55F);
                    const glm::vec2 direction{tip.direction.x*std::cos(turn)-tip.direction.y*std::sin(turn),
                        tip.direction.x*std::sin(turn)+tip.direction.y*std::cos(turn)};
                    tips.push_back({next,direction,tip.energy*0.45F,tip.curvature,tip.generation+1});
                    tip.energy*=0.8F;
                }
            }
        }
        return segments;
    }

    std::shared_ptr<const Mesh> GlassDestructionSystem::restoreDebrisCollision(std::shared_ptr<const Mesh> mesh) {
        if (!mesh || !mesh->convexCollisionParts.empty() || mesh->vertices.size()<18 || mesh->vertices.size()%6!=0)
            return mesh;
        const auto count=mesh->vertices.size()/6;
        const float front=mesh->vertices.front().position.z(), back=mesh->vertices[count].position.z();
        if (front<=back) return mesh;
        GlassFragment fragment;
        for (std::size_t i=0; i<count; ++i) {
            const auto &a=mesh->vertices[i], &b=mesh->vertices[count+i];
            if (a.normal.z()<0.99F || b.normal.z()>-0.99F ||
                std::abs(a.position.z()-front)>Epsilon || std::abs(b.position.z()-back)>Epsilon ||
                glm::distance(glm::vec2{a.position.native()},glm::vec2{b.position.native()})>Epsilon)
                return mesh;
            fragment.polygon.push_back(glm::vec2{a.position.native()});
        }
        // Canonical prism vertices survive scene serialization. Rebuild only
        // runtime collision parts, leaving the saved renderer mesh untouched.
        if (std::abs(front+back)>Epsilon) return mesh;
        return shardGeometry(fragment,front-back);
    }

    std::shared_ptr<Mesh> GlassDestructionSystem::collisionMesh(const GlassDestructionComponent &glass) {
        return attachedMesh(glass);
    }

    std::shared_ptr<Mesh> GlassDestructionSystem::renderMesh(const GlassDestructionComponent &glass) {
        if (glass.fractureRenderGeometry) return glass.fractureRenderGeometry;
        auto mesh = std::make_shared<Mesh>();
        for (const auto &fragment : glass.fragments) {
            if (!fragment.collisionGeometry) {
                fragment.collisionGeometry = shardGeometry(fragment,glass.thickness);
            }
            const auto first = mesh->indexCount();
            const auto firstVertex = mesh->vertices.size();
            appendPrism(*mesh, fragment, glass.thickness);
            const auto fragmentIndex = static_cast<unsigned>(&fragment-glass.fragments.data());
            for (std::size_t edge=0; edge<fragment.polygon.size(); ++edge)
                for (unsigned corner=0; corner<4; ++corner)
                    mesh->vertices[firstVertex+fragment.polygon.size()*2+edge*4+corner].texCoord1 =
                        Vec2{float(edge+1),float(fragmentIndex+1)};
            glm::vec3 low{std::numeric_limits<float>::max()}, high{std::numeric_limits<float>::lowest()};
            for (auto i = firstVertex; i < mesh->vertices.size(); ++i) {
                low = glm::min(low, mesh->vertices[i].position.native());
                high = glm::max(high, mesh->vertices[i].position.native());
            }
            mesh->renderSections.push_back({.firstIndex = first, .indexCount = mesh->indexCount() - first,
                .localBounds = {Vec3{low}, Vec3{high}}});
        }
        // Reserve the intact draw alongside fragment draws. Its stable final
        // section is the only visible one until fracture; impacts never swap meshes.
        GlassFragment rectangle;
        const auto half = glass.size * 0.5F;
        rectangle.polygon = {{-half.x,-half.y},{half.x,-half.y},{half.x,half.y},{-half.x,half.y}};
        const auto intactFirst = mesh->indexCount();
        appendPrism(*mesh, rectangle, glass.thickness);
        mesh->renderSections.push_back({.firstIndex = intactFirst,
            .indexCount = mesh->indexCount() - intactFirst,
            .localBounds = {Vec3{-half.x,-half.y,-glass.thickness*0.5F}, Vec3{half.x,half.y,glass.thickness*0.5F}}});
        mesh->recalculateLocalBounds();
        glass.fractureRenderGeometry = mesh;
        return mesh;
    }

    Actor GlassDestructionSystem::createPane(Scene &scene, std::string name,
        GlassDestructionComponent settings, PBRMaterial material) {
        validate(settings);
        settings.fractured = settings.fullyDestroyed = settings.hadPhysicalSupport = false;
        settings.cracks.clear(); settings.impactCount = 0; settings.accumulatedEnergy = 0;
        settings.renderState = GlassRenderState::Intact;
        settings.fragments.clear();
        settings.bonds.clear();
        prepare(settings, {});
        settings.fractured = false;
        // Populate immutable convex collision payloads before gameplay.
        (void)renderMesh(settings);
        const auto half = settings.size * 0.5F;
        auto mesh = renderMesh(settings);
        material.transmission = std::max(material.transmission, 0.96F);
        material.thickness = settings.thickness;
        material.doubleSided = true;
        const Actor pane = scene.createMesh(std::move(name), mesh, material);
        const Entity entity = scene.findEntity(pane.id());
        scene.registry().modify<MeshRendererComponent>(entity, [&](auto &renderer) { renderer.mesh = scene.glassGeometryHandle(mesh); });
        scene.registry().add<GlassDestructionComponent>(entity, std::move(settings));
        scene.registry().add<ColliderComponent>(entity, ColliderComponent{.shape = BoxCollider{Vec3{half.x, half.y, settings.thickness * 0.5F}}, .offset = {}});
        return pane;
    }

    bool GlassDestructionSystem::prepareLegacyPane(Registry &registry, Entity entity) {
        if (registry.has<GlassDestructionComponent>(entity) || !enableLegacyPane(registry, entity)) return false;
        auto &glass = registry.get<GlassDestructionComponent>(entity);
        prepare(glass, {});
        glass.fractured = false;
        registry.modify<MeshRendererComponent>(entity, [&](auto &renderer) {
            renderer.mesh = MeshHandle{renderMesh(glass)};
        });
        return true;
    }

    void GlassDestructionSystem::restoreGeometryCache(GlassDestructionComponent &glass) {
        if (glass.fragments.empty()) return;
        auto prepared = glass;
        prepare(prepared, glass.cracks.empty() ? glm::vec2{} : glass.cracks.front().position);
        if (prepared.fragments.size() != glass.fragments.size()) return;
        for (std::size_t i=0; i<glass.fragments.size(); ++i)
            if (glass.fragments[i].polygon != prepared.fragments[i].polygon) return;
        glass.fractureRenderGeometry = prepared.fractureRenderGeometry;
        glass.intactRenderGeometry = prepared.intactRenderGeometry;
        glass.fractureTemplate = prepared.fractureTemplate;
        for (std::size_t i=0; i<glass.fragments.size(); ++i)
            glass.fragments[i].collisionGeometry = prepared.fragments[i].collisionGeometry;
    }

    bool GlassDestructionSystem::onImpact(Scene &scene, Entity entity, const GlassImpact &impact) {
        auto &registry = scene.registry();
        if (!registry.valid(entity) || !registry.has<Transform>(entity) ||
            !registry.has<MeshRendererComponent>(entity)) return false;
        if (!registry.has<GlassDestructionComponent>(entity)) {
            // Existing saved Glass presets predate the component. Register only
            // compatible thin boxes, and leave weak/invalid shots read-only.
            if (!finite(impact.worldPosition.native()) || !finite(impact.worldDirection.native()) ||
                !std::isfinite(impact.energy) || impact.energy < GlassDestructionComponent{}.fractureThreshold ||
                !std::isfinite(impact.radius) || impact.radius <= 0.0F ||
                !enableLegacyPane(registry, entity)) return false;
        }
        auto &glass = registry.get<GlassDestructionComponent>(entity);
        validate(glass);
        if (glass.fullyDestroyed || !finite(impact.worldPosition.native()) || !finite(impact.worldDirection.native()) ||
            !std::isfinite(impact.energy) || !std::isfinite(impact.radius) || impact.radius <= 0.0F ||
            impact.energy < glass.fractureThreshold * 0.05F) return false;
        // Direct gameplay calls may precede the transform stage. Root TRS is
        // authoritative; resolve a hierarchy only when the pane has a parent.
        if (registry.has<ParentComponent>(entity)) TransformSystem::updateDirty(registry);
        const Transform world = registry.has<ParentComponent>(entity)
            ? TransformSystem::worldTransform(registry, entity) : registry.get<Transform>(entity);
        const glm::mat4 matrix = world.matrix().native();
        const float determinant = glm::determinant(matrix);
        if (!std::isfinite(determinant) || std::abs(determinant) < Epsilon) return false;
        // Attached cells form a static compound; a single dynamic convex hull would fill its holes.
        if (registry.has<RigidbodyComponent>(entity) &&
            registry.get<RigidbodyComponent>(entity).type != RigidbodyType::Static) return false;
        // Thin physical prisms under mirrored transforms are outside this prototype's contract.
        if (world.scale.x() <= 0.0F || world.scale.y() <= 0.0F || world.scale.z() <= 0.0F) return false;
        const auto local = glm::vec3{glm::inverse(matrix) * glm::vec4{impact.worldPosition.native(), 1.0F}};
        if (std::abs(local.x) > glass.size.x * 0.5F + Epsilon ||
            std::abs(local.y) > glass.size.y * 0.5F + Epsilon ||
            std::abs(local.z) > glass.thickness * 0.5F + 0.001F) return false;
        glm::vec2 hit{local};
        // Inverse transforms can move a mathematically central hit by a few ulps.
        // Canonicalize that case so it reuses the centered preview template.
        for (unsigned axis=0; axis<2; ++axis)
            if (std::abs(hit[axis])<glass.size[axis]*Epsilon) hit[axis]=0;

        auto contains = [&](const GlassFragment &fragment) { return containsPoint(fragment.polygon, hit); };
        if (glass.fractured && std::ranges::none_of(glass.fragments,
            [&](const auto &fragment) { return fragment.attached && contains(fragment); })) return false;
        if (glass.impactCount == 0 && !glass.fractured) {
            prepare(glass, hit);
            registry.modify<MeshRendererComponent>(entity, [&](auto &renderer) {
                renderer.mesh = scene.glassGeometryHandle(renderMesh(glass));
            });
            registry.markChanged<ColliderComponent>(entity);
        }
        ++glass.impactCount;
        if (glass.cracks.size() == 128) glass.cracks.erase(glass.cracks.begin());
        const float scale = std::max(Epsilon, std::min(world.scale.x(), world.scale.y()));
        glass.cracks.push_back({hit, std::max(impact.radius / scale, 0.05F) *
            (2.0F + std::sqrt(impact.energy / glass.fractureThreshold)),
            std::min(1.0F, impact.energy / glass.fractureThreshold),
            glass.seed + glass.impactCount * 1664525U});
        glass.renderState = glass.fractured ? glass.renderState : GlassRenderState::Cracked;
        glass.accumulatedEnergy = std::min(glass.accumulatedEnergy, std::numeric_limits<float>::max()-impact.energy) + impact.energy;
        registry.markChanged<GlassDestructionComponent>(entity);
        const float effectiveThreshold = glass.fractureThreshold *
            (glass.type == GlassType::Armored ? float(glass.layers) : 1.0F);
        if (glass.type == GlassType::Armored) {
            bool penetrated = false;
            for (auto &fragment : glass.fragments) {
                if (!fragment.attached || !contains(fragment)) continue;
                if (fragment.layerDamage.size() != glass.layers) fragment.layerDamage.assign(glass.layers, 0);
                float remaining = impact.energy;
                for (auto &damage : fragment.layerDamage) {
                    const float absorbed = std::min(remaining, std::max(0.0F, glass.fractureThreshold-damage));
                    damage += absorbed; remaining -= absorbed;
                }
                penetrated |= std::ranges::all_of(fragment.layerDamage, [&](float damage) { return damage >= glass.fractureThreshold; });
            }
            if (!penetrated) return true;
        } else if (impact.energy < effectiveThreshold && glass.accumulatedEnergy < effectiveThreshold) return true;
        // Laminated fragments remain retained by the interlayer until it ruptures.
        if (glass.type == GlassType::Laminated && glass.accumulatedEnergy < effectiveThreshold * 8.0F) return true;
        if (glass.impactCount > 1 && refineImpact(glass, hit, glm::vec2{world.scale.native()}, impact.radius)) {
            registry.modify<MeshRendererComponent>(entity, [&](auto &renderer) {
                renderer.mesh = scene.glassGeometryHandle(renderMesh(glass));
            });
            registry.markChanged<ColliderComponent>(entity);
        }
        if (!glass.fractured) {
            // Each energetic hit adds a local fracture nucleus to surviving glass.
            if (glass.fragments.empty()) prepare(glass, hit);
            glass.fractured = true;
        }
        const float strength = impact.energy / glass.fractureThreshold;
        auto influence = [&](glm::vec2 point) {
            const auto displacement = glm::vec3{matrix * glm::vec4{point - hit, 0.0F, 0.0F}};
            return std::exp(-glm::dot(displacement, displacement) / (2.0F * std::max(impact.radius * impact.radius, 1.0e-12F)));
        };
        std::vector<bool> wasAttached;
        for (const auto &fragment : glass.fragments) wasAttached.push_back(fragment.attached);
        std::vector<bool> hitCells(glass.fragments.size());
        for (std::size_t i = 0; i < glass.fragments.size(); ++i) {
            auto &fragment = glass.fragments[i];
            if (!fragment.attached) continue;
            hitCells[i] = contains(fragment);
            fragment.damage += strength * influence(fragment.center);
            if (fragment.frameSupported) {
                float capacity = 0, exposure = 0;
                const float reference = std::sqrt(glass.size.x*glass.size.y/float(glass.maxFragments));
                for (std::size_t edgeIndex=0; edgeIndex<fragment.polygon.size(); ++edgeIndex) {
                    const auto a = fragment.polygon[edgeIndex], b = fragment.polygon[(edgeIndex+1)%fragment.polygon.size()];
                    if (!supportedEdge(glass, a, b)) continue;
                    const auto edge = b-a;
                    const float length = glm::length(glm::vec3{matrix*glm::vec4{edge,0,0}});
                    const float edgeCapacity = glass.fractureThreshold*glass.bondStrength*glass.thickness*world.scale.z()/0.006F *
                        length/std::max(reference,0.001F);
                    const auto point = a+edge*std::clamp(glm::dot(hit-a,edge)/glm::dot(edge,edge),0.0F,1.0F);
                    capacity += edgeCapacity; exposure += edgeCapacity*influence(point);
                }
                if (capacity > 0) fragment.anchorDamage += impact.energy*exposure/(capacity*capacity);
            }
            if (hitCells[i] || fragment.anchorDamage >= 1.0F || glass.type == GlassType::Tempered) fragment.frameSupported = false;
        }
        for (auto &bond : glass.bonds) {
            if (bond.broken) continue;
            const glm::vec2 edge = bond.edgeEnd-bond.edgeStart;
            const float length2 = glm::dot(edge,edge);
            const auto closest = length2 > Epsilon * Epsilon ? bond.edgeStart + edge *
                std::clamp(glm::dot(hit-bond.edgeStart,edge)/length2, 0.0F, 1.0F) : bond.center;
            bond.accumulatedDamage += impact.energy * influence(closest);
            const float worldLength = glm::length(glm::vec3{matrix*glm::vec4{edge,0,0}});
            const float strengthScale = bond.length > Epsilon ? worldLength/bond.length * world.scale.z() : 1.0F;
            bond.damage = bond.accumulatedDamage / std::max(bond.strength*strengthScale, 0.001F);
            bond.broken = bond.damage >= 1.0F || hitCells[bond.first] || hitCells[bond.second] ||
                glass.type == GlassType::Tempered;
            if (bond.broken) bond.damage = std::max(1.0F, bond.damage);
        }
        // Isolated hit cells have no bonds; their anchor is removed above.
        detachUnsupported(scene, registry, entity, world, impact, hit, wasAttached);
        return true;
    }

    void GlassDestructionSystem::onActorDestroyed(Scene &scene, Entity entity) {
        auto &registry = scene.registry();
        if (registry.has<GlassFragmentBindingComponent>(entity)) {
            const auto binding = registry.get<GlassFragmentBindingComponent>(entity);
            if (registry.valid(binding.paneEntity) && registry.has<GlassDestructionComponent>(binding.paneEntity)) {
                auto &glass = registry.get<GlassDestructionComponent>(binding.paneEntity);
                if (binding.fragmentIndex < glass.fragments.size()) glass.fragments[binding.fragmentIndex].debrisEntity = NullEntity;
                registry.markChanged<GlassDestructionComponent>(binding.paneEntity);
            }
        }
        if (!registry.has<GlassDestructionComponent>(entity) || !registry.has<MeshRendererComponent>(entity)) return;
        const auto material = registry.get<MeshRendererComponent>(entity).material;
        std::vector<Entity> debris;
        for (const auto &fragment : registry.get<GlassDestructionComponent>(entity).fragments)
            if (registry.valid(fragment.debrisEntity)) debris.push_back(fragment.debrisEntity);
        // Removing a pane is rare. Promote surviving shards to standalone renderers
        // here so their lifetime stays independent of the pane.
        for (auto shard : debris) {
            const auto *collider = std::get_if<MeshCollider>(&registry.get<ColliderComponent>(shard).shape);
            if (collider && collider->mesh) {
                MeshRendererComponent renderer;
                renderer.mesh = MeshHandle{collider->mesh};
                renderer.material = material;
                renderer.materialOverride = true;
                registry.add<MeshRendererComponent>(shard, std::move(renderer));
            }
            registry.remove<GlassFragmentBindingComponent>(shard);
        }
    }

    void GlassDestructionSystem::update(Scene &scene, float deltaTime, const PhysicsSystem *physics) {
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0F) return;
        scene.beginGlassSimulationStep();
        // Observe physical supports before creating actors (ECS storage may move).
        // A declared bottom mount is used until a real support has been observed.
        // Once observed, removing/moving it releases the whole remaining pane.
        if (physics) {
            std::vector<Entity> panes;
            scene.registry().view<GlassDestructionComponent, Transform, MeshRendererComponent>(
                [&](Entity entity, const auto &glass, const auto &, const auto &) {
                if (!glass.fullyDestroyed) panes.push_back(entity);
            });
            const auto colliderRevision = scene.registry().componentRevision<ColliderComponent>();
            const auto transformRevision = scene.registry().componentRevision<Transform>();
            const auto structureRevision = scene.registry().structuralRevision();
            for (auto entity : panes) {
                auto &registry = scene.registry();
                const auto world = TransformSystem::worldTransform(registry, entity);
                auto &glass = registry.get<GlassDestructionComponent>(entity);
                if (glass.supportMode != GlassSupportMode::Bottom) continue;
                bool supported = false;
                const auto gravity = physics->gravity();
                if (gravity.length() <= Epsilon) continue;
                bool needsCheck = glass.supportColliderRevision == ~std::uint64_t{0} ||
                    glass.supportPaneMatrix != registry.get<Transform>(entity).worldMatrix().native() ||
                    glass.supportGravity != gravity.native();
                if (!glass.physicalSupports.empty()) {
                    const auto changed = registry.componentEntitiesChangedSince<ColliderComponent>(glass.supportColliderRevision);
                    for (const auto &support : glass.physicalSupports)
                        needsCheck |= !registry.valid(support.entity) || !registry.has<Transform>(support.entity) ||
                            !registry.has<ColliderComponent>(support.entity) ||
                            (registry.get<Transform>(support.entity).worldRevision() != support.worldRevision &&
                             registry.get<Transform>(support.entity).worldMatrix().native() != support.worldMatrix) ||
                            std::ranges::find(changed, support.entity) != changed.end();
                } else {
                    // An implicit mount observes newly added/moved static supports.
                    // Moving debris cannot invalidate every pane's support cache.
                    auto relevant = [&](Entity candidate) {
                        return registry.valid(candidate) && registry.has<ColliderComponent>(candidate) &&
                            !registry.has<GlassDebrisComponent>(candidate) &&
                            (!registry.has<RigidbodyComponent>(candidate) ||
                             registry.get<RigidbodyComponent>(candidate).type != RigidbodyType::Dynamic);
                    };
                    for (auto candidate : registry.componentEntitiesChangedSince<ColliderComponent>(glass.supportColliderRevision))
                        needsCheck |= relevant(candidate);
                    for (auto candidate : registry.componentEntitiesChangedSince<Transform>(glass.supportTransformRevision))
                        needsCheck |= relevant(candidate);
                }
                glass.supportColliderRevision = colliderRevision;
                glass.supportTransformRevision = transformRevision;
                glass.supportPaneWorldRevision = registry.get<Transform>(entity).worldRevision();
                glass.supportPaneMatrix = registry.get<Transform>(entity).worldMatrix().native();
                glass.supportGravity = gravity.native();
                if (!needsCheck) continue;
                glass.physicalSupports.clear();
                const auto down = gravity * (1.0F / gravity.length());
                for (float x : {-0.8F, 0.0F, 0.8F}) {
                    const auto point = glm::vec3{world.matrix().native() *
                        glm::vec4{x * glass.size.x * 0.5F, -glass.size.y * 0.5F, 0.0F, 1.0F}};
                    ++glass.supportRaycasts;
                    const auto hit = physics->raycastPrepared(scene, Vec3{point} - down * 0.01F,
                        down, 0.06F, scene.findByEntity(entity)->objectId());
                    if (hit && !registry.has<GlassDebrisComponent>(scene.findEntity(hit->actor.id()))) {
                        supported = true;
                        const auto support = scene.findEntity(hit->actor.id());
                        glass.physicalSupports.push_back({support, registry.get<Transform>(support).worldRevision(),
                            registry.get<Transform>(support).worldMatrix().native()});
                    }
                }
                glass.supportColliderRevision = colliderRevision;
                glass.supportTransformRevision = transformRevision;
                glass.supportStructureRevision = structureRevision;
                glass.supportGravity = gravity.native();
                if (supported && !glass.hadPhysicalSupport) {
                    glass.hadPhysicalSupport = true;
                    registry.markChanged<GlassDestructionComponent>(entity);
                }
                if (!supported && glass.hadPhysicalSupport) {
                    if (!glass.fractured) {
                        if (glass.fragments.empty()) prepare(glass, {0.0F, -glass.size.y * 0.5F});
                        glass.fractured = true;
                    }
                    std::vector<bool> attached;
                    for (const auto &fragment : glass.fragments) attached.push_back(fragment.attached);
                    detachUnsupported(scene, registry, entity, world, GlassImpact{.worldDirection = {}, .energy = 0.0F},
                        {}, attached, true);
                }
            }
        }
        std::vector<Entity> expired;
        scene.registry().view<GlassDebrisComponent>([&](Entity entity, auto &debris) {
            debris.remainingLifetime -= deltaTime;
            if (debris.remainingLifetime <= 0.0F) expired.push_back(entity);
        });
        for (Entity entity : expired) scene.destroy(entity);
    }
}
