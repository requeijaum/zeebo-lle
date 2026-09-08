// soft_rasterizer.cpp — Backend 1: portable software rasterizer (headless/CI).
// FASE 1 skeleton: implements clear + a minimal barycentric triangle fill so the
// interface is provably exercised WITHOUT a host GPU. This is the "correctness
// oracle" backend (Citra's software renderer analog). Not optimized.
#include "igpu_rasterizer.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace zeebo::gpu {

class SoftRasterizer final : public IGpuRasterizer {
public:
    bool init() override {
        fb_.assign(kFbWidth*kFbHeight,0);
        depth_.assign(kFbWidth*kFbHeight,1.0f);
        return true;
    }
    void begin_frame() override {}

    void set_viewport(int x,int y,int w,int h) override {
        vx_=std::clamp(x,-kFbWidth,kFbWidth);
        vy_=std::clamp(y,-kFbHeight,kFbHeight);
        vw_=std::clamp(w,0,kFbWidth*2);
        vh_=std::clamp(h,0,kFbHeight*2);
    }
    void clear_color(f32 r,f32 g,f32 b,f32 a) override { cr_=r; cg_=g; cb_=b; (void)a; }
    void clear(u32 mask) override {
        if(mask==0 || (mask&0x4000)){
            const u16 c=pack565(cr_,cg_,cb_);
            std::fill(fb_.begin(),fb_.end(),c);
        }
        if(mask&0x0100) std::fill(depth_.begin(),depth_.end(),1.0f);
    }
    void set_state(const RenderState& s) override { st_=s; }
    void set_mvp(const Mat4& m) override { mvp_=m; }

    void tex_image_2d(u32 id,int w,int h,const void* rgba8) override {
        if (w<=0 || h<=0 || w>4096 || h>4096 || !rgba8) return;
        Texture t=textures_.count(id)?textures_[id]:Texture{}; // keep sampler params
        t.w=w; t.h=h;
        const size_t bytes=static_cast<size_t>(w)*static_cast<size_t>(h)*4;
        const auto* src=static_cast<const u8*>(rgba8);
        t.rgba.assign(src,src+bytes);
        textures_[id]=std::move(t);
    }
    void tex_parameter(u32 unit,u32 pname,u32 param) override {
        if(unit>=bound_tex_.size()) return;
        const u32 id=bound_tex_[unit];
        if(id==0) return;
        Texture& t=textures_[id]; // default-constructs an entry if absent (bind order)
        switch(pname){
            case 0x2800: t.mag_nearest=(param==0x2600); break; // MAG_FILTER
            case 0x2801: t.min_nearest=(param==0x2600); break; // MIN_FILTER (state-only: no minification/LOD path yet)
            case 0x2802: t.clamp_s=(param==0x812F); break;     // WRAP_S / CLAMP_TO_EDGE
            case 0x2803: t.clamp_t=(param==0x812F); break;     // WRAP_T
            default: break; // unknown pname: ignore safely
        }
    }
    void bind_texture(u32 unit,u32 id) override {
        if(unit<bound_tex_.size()) bound_tex_[unit]=id;
    }
    void delete_texture(u32 id) override {
        textures_.erase(id);
        for(auto& bound:bound_tex_) if(bound==id) bound=0;
    }

    void draw(Prim p, const std::vector<Vertex>& v) override {
        if (p==Prim::Triangles) {
            for (size_t i=0;i+2<v.size();i+=3) fill_tri(v[i],v[i+1],v[i+2]);
        } else if (p==Prim::TriStrip) {
            for (size_t i=0;i+2<v.size();++i)
                if ((i&1u)==0) fill_tri(v[i],v[i+1],v[i+2]);
                else            fill_tri(v[i+1],v[i],v[i+2]);
        } else if (p==Prim::TriFan && v.size()>=3) {
            for (size_t i=1;i+1<v.size();++i) fill_tri(v[0],v[i],v[i+1]);
        }
    }
    void draw_indexed(Prim p, const std::vector<Vertex>& v,
                      const std::vector<u32>& idx) override {
        auto valid=[&](size_t i){ return i<idx.size() && idx[i]<v.size(); };
        if (p==Prim::Triangles) {
            for (size_t i=0;i+2<idx.size();i+=3)
                if(valid(i)&&valid(i+1)&&valid(i+2)) fill_tri(v[idx[i]],v[idx[i+1]],v[idx[i+2]]);
        } else if (p==Prim::TriStrip) {
            for (size_t i=0;i+2<idx.size();++i) {
                if (!valid(i) || !valid(i+1) || !valid(i+2)) continue;
                if ((i&1)==0) fill_tri(v[idx[i]],v[idx[i+1]],v[idx[i+2]]);
                else fill_tri(v[idx[i+1]],v[idx[i]],v[idx[i+2]]);
            }
        } else if (p==Prim::TriFan && !idx.empty()) {
            for (size_t i=1;i+1<idx.size();++i) if(valid(0)&&valid(i)&&valid(i+1))
                fill_tri(v[idx[0]],v[idx[i]],v[idx[i+1]]);
        }
    }

    void end_frame() override {}
    const u16* framebuffer_rgb565() const override { return fb_.data(); }

private:
    struct Texture { int w=0,h=0; std::vector<u8> rgba;
                     bool mag_nearest=false,min_nearest=false,
                          clamp_s=false,clamp_t=false; };

    static u16 pack565(f32 r,f32 g,f32 b){
        auto c=[](f32 x){ return (u32)std::lround(std::clamp(x,0.f,1.f)*255.f); };
        u32 R=c(r)>>3, G=c(g)>>2, B=c(b)>>3; return (u16)((R<<11)|(G<<5)|B);
    }
    std::array<f32,4> sample_texture(f32 u,f32 v) const {
        const u32 unit=std::min<u32>(st_.active_unit,1);
        const auto it=textures_.find(bound_tex_[unit]);
        if(it==textures_.end() || it->second.rgba.empty()) return {1,1,1,1};
        const Texture& t=it->second;
        auto addr=[](int i,int n,bool clamp){
            if(clamp) return std::clamp(i,0,n-1);
            i%=n; return i<0?i+n:i; };
        auto texel=[&](int x,int y){
            x=addr(x,t.w,t.clamp_s); y=addr(y,t.h,t.clamp_t);
            const size_t p=(static_cast<size_t>(y)*t.w+x)*4;
            return std::array<f32,4>{t.rgba[p]/255.f,t.rgba[p+1]/255.f,
                                     t.rgba[p+2]/255.f,t.rgba[p+3]/255.f};
        };
        // NEAREST: snap sample point to nearest texel center (no interpolation).
        if(t.mag_nearest){
            const int xi=static_cast<int>(std::floor(u*t.w));
            const int yi=static_cast<int>(std::floor(v*t.h));
            return texel(xi,yi);
        }
        const f32 x=u*t.w-0.5f, y=v*t.h-0.5f;
        const int x0=static_cast<int>(std::floor(x)), y0=static_cast<int>(std::floor(y));
        const f32 fx=x-x0, fy=y-y0;
        const auto p00=texel(x0,y0), p10=texel(x0+1,y0);
        const auto p01=texel(x0,y0+1), p11=texel(x0+1,y0+1);
        std::array<f32,4> out{};
        for(size_t c=0;c<4;++c){
            const f32 top=p00[c]+(p10[c]-p00[c])*fx;
            const f32 bottom=p01[c]+(p11[c]-p01[c])*fx;
            out[c]=top+(bottom-top)*fy;
        }
        return out;
    }

    // GLES depth-comparison: does incoming z pass against stored depth?
    static bool depth_pass(u32 func,f32 z,f32 stored){
        switch(func){
            case 0x0200: return false;          // NEVER
            case 0x0201: return z<stored;       // LESS
            case 0x0202: return z==stored;      // EQUAL
            case 0x0203: return z<=stored;      // LEQUAL
            case 0x0204: return z>stored;       // GREATER
            case 0x0205: return z!=stored;      // NOTEQUAL
            case 0x0206: return z>=stored;      // GEQUAL
            case 0x0207: return true;           // ALWAYS
            default:     return z<stored;       // default GL_LESS
        }
    }
    // GLES alpha-test: does the fragment alpha pass the reference comparison?
    static bool alpha_pass(u32 func,f32 a,f32 ref){
        switch(func){
            case 0x0200: return false; case 0x0201: return a<ref;
            case 0x0202: return a==ref; case 0x0203: return a<=ref;
            case 0x0204: return a>ref;  case 0x0205: return a!=ref;
            case 0x0206: return a>=ref; case 0x0207: return true;
            default:     return true;  // default GL_ALWAYS
        }
    }
    // GLES blend factor -> per-channel scale, given src/dst color+alpha.
    static f32 blend_factor(u32 f,int c,f32 sr,f32 sg,f32 sb,f32 sa,
                            f32 dr,f32 dg,f32 db,f32 da){
        auto pick=[&](f32 r,f32 g,f32 b,f32 a){ return c==0?r:c==1?g:c==2?b:a; };
        switch(f){
            case 0x0000: return 0.f;                                   // ZERO
            case 0x0001: return 1.f;                                   // ONE
            case 0x0300: return pick(sr,sg,sb,sa);                     // SRC_COLOR
            case 0x0301: return 1.f-pick(sr,sg,sb,sa);                 // ONE_MINUS_SRC_COLOR
            case 0x0302: return sa;                                    // SRC_ALPHA
            case 0x0303: return 1.f-sa;                                // ONE_MINUS_SRC_ALPHA
            case 0x0304: return da;                                    // DST_ALPHA
            case 0x0305: return 1.f-da;                                // ONE_MINUS_DST_ALPHA
            case 0x0306: return pick(dr,dg,db,da);                     // DST_COLOR
            case 0x0307: return 1.f-pick(dr,dg,db,da);                 // ONE_MINUS_DST_COLOR
            case 0x0308: return std::min(sa,1.f-da);                   // SRC_ALPHA_SATURATE
            default:     return 1.f;
        }
    }

    // Mapeia NDC [-1,1] para o viewport (vx,vy,vw,vh), y-flip p/ tela top-down.
    void fill_tri(const Vertex&a,const Vertex&b,const Vertex&c){
        auto finite_ndc=[](f32 v){ return std::isfinite(v) ? std::clamp(v,-4.0f,4.0f) : 0.0f; };
        auto sx=[&](f32 x){ return vx_ + static_cast<int>((finite_ndc(x)*0.5f+0.5f)*static_cast<f32>(vw_)); };
        auto sy=[&](f32 y){ return vy_ + static_cast<int>((1.f-(finite_ndc(y)*0.5f+0.5f))*static_cast<f32>(vh_)); };
        int x0=sx(a.x),y0=sy(a.y),x1=sx(b.x),y1=sy(b.y),x2=sx(c.x),y2=sy(c.y);
        int minx=std::max(0,std::min({x0,x1,x2})), maxx=std::min(kFbWidth-1,std::max({x0,x1,x2}));
        int miny=std::max(0,std::min({y0,y1,y2})), maxy=std::min(kFbHeight-1,std::max({y0,y1,y2}));
        const int64_t area=int64_t(x1-x0)*(y2-y0)-int64_t(x2-x0)*(y1-y0); if(area==0) return;
        // Face culling (GLES): default front face = CCW. In screen space y is
        // flipped, so a CCW-in-NDC front face has NEGATIVE signed screen area.
        if(st_.cull){
            const bool is_front = area<0;           // screen-space front (CCW in NDC)
            const bool cull_back  = st_.cull_face==0x0405 || st_.cull_face==0x0408; // BACK/F&B
            const bool cull_front = st_.cull_face==0x0404 || st_.cull_face==0x0408; // FRONT/F&B
            if((is_front && cull_front) || (!is_front && cull_back)) return;
        }
        for(int y=miny;y<=maxy;++y) for(int x=minx;x<=maxx;++x){
            const int64_t w0=int64_t(x1-x)*(y2-y)-int64_t(x2-x)*(y1-y);
            const int64_t w1=int64_t(x2-x)*(y0-y)-int64_t(x0-x)*(y2-y);
            const int64_t w2=int64_t(x0-x)*(y1-y)-int64_t(x1-x)*(y0-y);
            if((w0>=0&&w1>=0&&w2>=0)||(w0<=0&&w1<=0&&w2<=0)){
                const f32 fa=(f32)w0/area, fb=(f32)w1/area, fc=(f32)w2/area;
                const size_t pixel=static_cast<size_t>(y)*kFbWidth+x;
                const f32 z=std::clamp((fa*a.z+fb*b.z+fc*c.z)*0.5f+0.5f,0.0f,1.0f);
                if(st_.depth_test && !depth_pass(st_.depth_func,z,depth_[pixel])) continue;
                f32 r=fa*a.r+fb*b.r+fc*c.r;
                f32 g=fa*a.g+fb*b.g+fc*c.g;
                f32 bl=fa*a.b+fb*b.b+fc*c.b;
                f32 alpha=fa*a.a+fb*b.a+fc*c.a;
                if(st_.tex_enabled[std::min<u32>(st_.active_unit,1)] != 0){
                    const f32 u=fa*a.u+fb*b.u+fc*c.u;
                    const f32 v=fa*a.v+fb*b.v+fc*c.v;
                    const auto texel=sample_texture(u,v);
                    r*=texel[0]; g*=texel[1]; bl*=texel[2]; alpha*=texel[3];
                }
                // Alpha test: discard fragment BEFORE depth write / blend.
                if(st_.alpha_test && !alpha_pass(st_.alpha_func,alpha,st_.alpha_ref)) continue;
                if(st_.depth_test && st_.depth_write) depth_[pixel]=z;
                if(st_.blend){
                    const u16 dst=fb_[pixel];
                    const f32 dr=((dst>>11)&31)/31.f, dg=((dst>>5)&63)/63.f, db=(dst&31)/31.f, da=1.f;
                    const f32 sr=std::clamp(r,0.f,1.f),sg=std::clamp(g,0.f,1.f),
                              sb=std::clamp(bl,0.f,1.f),sa=std::clamp(alpha,0.f,1.f);
                    r  = sr*blend_factor(st_.blend_src,0,sr,sg,sb,sa,dr,dg,db,da)
                       + dr*blend_factor(st_.blend_dst,0,sr,sg,sb,sa,dr,dg,db,da);
                    g  = sg*blend_factor(st_.blend_src,1,sr,sg,sb,sa,dr,dg,db,da)
                       + dg*blend_factor(st_.blend_dst,1,sr,sg,sb,sa,dr,dg,db,da);
                    bl = sb*blend_factor(st_.blend_src,2,sr,sg,sb,sa,dr,dg,db,da)
                       + db*blend_factor(st_.blend_dst,2,sr,sg,sb,sa,dr,dg,db,da);
                }
                fb_[pixel]=pack565(r,g,bl);
            }
        }
    }
    std::vector<u16> fb_;
    std::vector<f32> depth_;
    std::unordered_map<u32,Texture> textures_;
    std::array<u32,2> bound_tex_{0,0};
    RenderState st_{}; Mat4 mvp_{};
    int vx_=0,vy_=0,vw_=kFbWidth,vh_=kFbHeight;
    f32 cr_=0,cg_=0,cb_=0;
};

std::unique_ptr<IGpuRasterizer> make_soft_rasterizer(){
    return std::make_unique<SoftRasterizer>();
}

} // namespace zeebo::gpu
