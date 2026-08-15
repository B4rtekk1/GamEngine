#include "Engine/UI/Vulkan/UIFontAtlas.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace Engine::UI {
namespace {

struct Table { std::size_t offset{}; std::size_t length{}; };
struct Point { double x{}; double y{}; bool onCurve = true; };
struct Edge { Point a; Point b; };
struct Outline { std::vector<std::vector<Point>> contours; };

class TrueType final {
public:
    bool load(const std::string& path, std::string& error) {
        std::ifstream file(path, std::ios::binary);
        if (!file) { error = "Could not load font: " + path; return false; }
        file.seekg(0, std::ios::end);
        const auto size = file.tellg();
        if (size <= 0) { error = "Font file is empty: " + path; return false; }
        data_.resize(static_cast<std::size_t>(size));
        file.seekg(0); file.read(reinterpret_cast<char*>(data_.data()), size);
        if (!file || data_.size() < 12) { error = "Invalid TrueType font: " + path; return false; }
        const auto count = u16(4);
        if (12u + static_cast<std::size_t>(count) * 16u > data_.size()) { error = "Invalid TrueType table directory"; return false; }
        for (std::uint16_t i = 0; i < count; ++i) {
            const auto p = 12u + static_cast<std::size_t>(i) * 16u;
            tables_[tag(p)] = {u32(p + 8), u32(p + 12)};
        }
        const auto head = table("head"); const auto maxp = table("maxp");
        const auto hhea = table("hhea"); const auto hmtx = table("hmtx");
        const auto loca = table("loca"); const auto glyf = table("glyf");
        const auto cmap = table("cmap");
        if (!head.length || !maxp.length || !hhea.length || !hmtx.length ||
            !loca.length || !glyf.length || !cmap.length) { error = "TrueType font is missing required tables"; return false; }
        units_ = u16(head.offset + 18); indexFormat_ = s16(head.offset + 50);
        glyphCount_ = u16(maxp.offset + 4); ascender_ = s16(hhea.offset + 4);
        descender_ = s16(hhea.offset + 6); lineGap_ = s16(hhea.offset + 8);
        metricCount_ = u16(hhea.offset + 34); loca_ = loca; glyf_ = glyf; hmtx_ = hmtx;
        return readCmap(cmap, error);
    }
    std::uint16_t glyph(std::uint32_t cp) const {
        if (cmapFormat_ == 4) {
            for (std::size_t i = 0; i < segments_.size(); ++i) if (cp <= segments_[i].end) {
                if (cp < segments_[i].start) return 0;
                const auto delta = segments_[i].delta;
                if (segments_[i].rangeOffset == 0) return static_cast<std::uint16_t>((cp + delta) & 0xffffu);
                const auto at = segments_[i].rangeAddress + segments_[i].rangeOffset + 2u * (cp - segments_[i].start);
                if (at + 2 > data_.size()) return 0;
                const auto id = u16(at); return id == 0 ? 0 : static_cast<std::uint16_t>((id + delta) & 0xffffu);
            }
        }
        return 0;
    }
    std::uint16_t units() const { return units_; }
    float ascent() const { return static_cast<float>(ascender_); }
    float lineHeight() const { return static_cast<float>(ascender_ - descender_ + lineGap_); }
    float advance(std::uint16_t id) const {
        const auto n = std::min<std::uint16_t>(id, metricCount_ ? metricCount_ - 1 : 0);
        return static_cast<float>(u16(hmtx_.offset + 4u * n));
    }
    bool outline(std::uint16_t id, Outline& out) const {
        if (id >= glyphCount_) return false;
        const auto off = glyphOffset(id), next = glyphOffset(id + 1);
        if (next <= off || glyf_.offset + next > data_.size()) return true;
        const auto p = glyf_.offset + off; const auto contours = s16(p);
        if (contours < 0) return false; // Composite glyphs are intentionally skipped for this small rasterizer.
        if (contours == 0) return true;
        auto end = std::vector<std::uint16_t>(contours); const auto endBase = p + 10;
        for (int i = 0; i < contours; ++i) end[i] = u16(endBase + 2u * i);
        const auto instructionLength = u16(endBase + 2u * contours);
        auto pos = endBase + 2u * contours + 2u + instructionLength;
        const auto count = static_cast<std::size_t>(end.back()) + 1u;
        std::vector<std::uint8_t> flags; flags.reserve(count);
        while (flags.size() < count) { const auto f = data_.at(pos++); flags.push_back(f); if (f & 8) { const auto n = data_.at(pos++); for (int i=0;i<n;++i) flags.push_back(f); } }
        std::vector<std::int16_t> xs(count), ys(count); decodeCoordinates(flags, pos, xs, true); decodeCoordinates(flags, pos, ys, false);
        std::size_t first = 0;
        for (const auto last : end) { std::vector<Point> contour; for (std::size_t i=first; i<=last; ++i) contour.push_back({static_cast<double>(xs[i]), static_cast<double>(ys[i]), (flags[i] & 1) != 0}); out.contours.push_back(std::move(contour)); first = last + 1; }
        return true;
    }
private:
    struct Segment { std::uint16_t start{}, end{}; std::int16_t delta{}; std::uint16_t rangeOffset{}; std::size_t rangeAddress{}; };
    static std::uint32_t makeTag(const char* s) { return (std::uint32_t(std::uint8_t(s[0]))<<24)|(std::uint32_t(std::uint8_t(s[1]))<<16)|(std::uint32_t(std::uint8_t(s[2]))<<8)|std::uint8_t(s[3]); }
    std::uint32_t tag(std::size_t p) const { return (std::uint32_t(data_[p])<<24)|(std::uint32_t(data_[p+1])<<16)|(std::uint32_t(data_[p+2])<<8)|data_[p+3]; }
    std::uint16_t u16(std::size_t p) const { return std::uint16_t(data_.at(p)<<8 | data_.at(p+1)); }
    std::int16_t s16(std::size_t p) const { return static_cast<std::int16_t>(u16(p)); }
    std::uint32_t u32(std::size_t p) const { return (std::uint32_t(u16(p))<<16)|u16(p+2); }
    Table table(const char* name) const { const auto i = tables_.find(makeTag(name)); return i == tables_.end() ? Table{} : i->second; }
    std::size_t glyphOffset(std::uint16_t id) const { return indexFormat_ == 0 ? 2u * u16(loca_.offset + 2u * id) : u32(loca_.offset + 4u * id); }
    void decodeCoordinates(const std::vector<std::uint8_t>& flags, std::size_t& pos, std::vector<std::int16_t>& values, bool x) const {
        std::int32_t value = 0;
        for (std::size_t i=0;i<flags.size();++i) { const auto f=flags[i]; const bool shortVec=x?(f&2):(f&4); const bool same=x?(f&16):(f&32); std::int32_t delta=0; if(shortVec) delta=data_.at(pos++); else if(!same) { delta=s16(pos); pos+=2; } if(shortVec && !same) delta=-delta; value+=delta; values[i]=static_cast<std::int16_t>(value); }
    }
    bool readCmap(Table cmap, std::string& error) {
        const auto n=u16(cmap.offset+2); std::size_t chosen=0;
        for(std::uint16_t i=0;i<n;++i){const auto p=cmap.offset+4u+8u*i; const auto platform=u16(p); const auto encoding=u16(p+2); const auto off=u32(p+4); if((platform==3 && (encoding==1||encoding==10))||platform==0){if(u16(cmap.offset+off)==4){chosen=cmap.offset+off;break;}}}
        if(!chosen){error="TrueType cmap format 4 is required";return false;} cmapFormat_=4; const auto segCount=u16(chosen+6)/2; const auto endBase=chosen+14; const auto startBase=endBase+2u*segCount+2; const auto deltaBase=startBase+2u*segCount; const auto rangeBase=deltaBase+2u*segCount; segments_.resize(segCount); for(std::size_t i=0;i<segCount;++i) segments_[i]={u16(startBase+2u*i),u16(endBase+2u*i),s16(deltaBase+2u*i),u16(rangeBase+2u*i),rangeBase+2u*i}; return true;
    }
    std::vector<std::uint8_t> data_; std::unordered_map<std::uint32_t,Table> tables_; std::vector<Segment> segments_; Table loca_{},glyf_{},hmtx_{}; std::uint16_t units_{},glyphCount_{},metricCount_{}; std::int16_t ascender_{},descender_{},lineGap_{},indexFormat_{}; int cmapFormat_{};
};

std::vector<Point> flatten(const std::vector<Point>& in) {
    std::vector<Point> result;
    if (in.empty()) return result;
    const auto midpoint = [](const Point& a, const Point& b) { return Point{(a.x + b.x) * .5, (a.y + b.y) * .5, true}; };
    const std::size_t n = in.size();
    Point current = in[0].onCurve ? in[0] : (in[n - 1].onCurve ? in[n - 1] : midpoint(in[n - 1], in[0]));
    result.push_back(current);
    std::size_t i = in[0].onCurve ? 1 : 0;
    while (i < n + (in[0].onCurve ? 0 : 1)) {
        const Point& point = in[i % n];
        if (point.onCurve) {
            result.push_back(point);
            current = point;
            ++i;
        } else {
            const Point& next = in[(i + 1) % n];
            const Point end = next.onCurve ? next : midpoint(point, next);
            for (int step = 1; step <= 8; ++step) {
                const double t = step / 8.0, u = 1.0 - t;
                result.push_back({u*u*current.x + 2*u*t*point.x + t*t*end.x,
                                  u*u*current.y + 2*u*t*point.y + t*t*end.y, true});
            }
            current = end;
            i += next.onCurve ? 2 : 1;
        }
    }
    return result;
}

float coverage(const std::vector<std::vector<Point>>& contours, double x, double y) {
    bool inside=false; for(const auto& contour:contours){auto poly=flatten(contour); for(std::size_t i=0,j=poly.size()-1;i<poly.size();j=i++){const auto&a=poly[i];const auto&b=poly[j];if(((a.y>y)!=(b.y>y))&&(x<(b.x-a.x)*(y-a.y)/(b.y-a.y)+a.x))inside=!inside;}} return inside?1.0f:0.0f;
}

} // namespace

std::string UIFontAtlas::build(const std::string& fontPath, std::uint32_t pixelSize, std::uint32_t firstCodepoint, std::uint32_t lastCodepoint) {
    if(fontPath.empty()||pixelSize==0||firstCodepoint>lastCodepoint)return "Invalid font atlas parameters";
    TrueType font; std::string error; if(!font.load(fontPath,error))return error;
    constexpr std::uint32_t padding=2, atlasWidth=1024, samples=4; struct Pending{std::uint32_t cp; Glyph glyph; Outline outline;}; std::vector<Pending> pending; std::uint32_t x=padding,y=padding,row=0,required=padding;
    const double scale=static_cast<double>(pixelSize)/font.units();
    for(std::uint32_t cp=firstCodepoint;cp<=lastCodepoint;++cp){const auto id=font.glyph(cp);Outline o;if(!font.outline(id,o))continue;double minX=0,minY=0,maxX=0,maxY=0;bool any=false;for(const auto& c:o.contours)for(const auto&p:c){minX=any?std::min(minX,p.x):p.x;maxX=any?std::max(maxX,p.x):p.x;minY=any?std::min(minY,p.y):p.y;maxY=any?std::max(maxY,p.y):p.y;any=true;}const auto w=static_cast<std::uint32_t>(std::ceil((maxX-minX)*scale));const auto h=static_cast<std::uint32_t>(std::ceil((maxY-minY)*scale));if(x+w+padding>atlasWidth){x=padding;y+=row+padding;row=0;}Pending q{cp,{},{},};q.outline=std::move(o);q.glyph.width=float(w);q.glyph.height=float(h);q.glyph.bearingX=float(minX*scale);q.glyph.bearingY=float(maxY*scale);q.glyph.advance=font.advance(id)*float(scale);q.glyph.uv.min[0]=float(x)/atlasWidth;q.glyph.uv.max[0]=float(x+w)/atlasWidth;q.glyph.uv.min[1]=float(y);q.glyph.uv.max[1]=float(y+h);pending.push_back(std::move(q));x+=w+padding;row=std::max(row,h);required=std::max(required,y+row+padding);}
    m_width=atlasWidth;m_height=std::max(required,pixelSize+padding*2);m_pixelSize=pixelSize;m_ascent=font.ascent()*float(scale);m_lineHeight=font.lineHeight()*float(scale);m_pixels.assign(std::size_t(m_width)*m_height,0);x=padding;y=padding;row=0;
    for(auto& q:pending){const auto w=static_cast<std::uint32_t>(q.glyph.width),h=static_cast<std::uint32_t>(q.glyph.height);if(x+w+padding>atlasWidth){x=padding;y+=row+padding;row=0;}for(std::uint32_t py=0;py<h;++py)for(std::uint32_t px=0;px<w;++px){float sum=0;for(std::uint32_t sy=0;sy<samples;++sy)for(std::uint32_t sx=0;sx<samples;++sx){const double fx=(px+(sx+.5)/samples)/scale+q.glyph.bearingX/scale;const double fy=q.glyph.bearingY/scale-(h-py-(sy+.5)/samples)/scale;sum+=coverage(q.outline.contours,fx,fy);}m_pixels[std::size_t(y+py)*m_width+x+px]=static_cast<std::uint8_t>(sum*255/(samples*samples));}q.glyph.uv.min[1]=float(y)/m_height;q.glyph.uv.max[1]=float(y+h)/m_height;m_glyphs.emplace(q.cp,q.glyph);x+=w+padding;row=std::max(row,h);}return {};
}
} // namespace Engine::UI
